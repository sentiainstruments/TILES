#include "debug_mode.h"

#include "board_layout.h"
#include "buttons.h"

#include "hardware/watchdog.h"
#include "pico/platform/sections.h"
#include "pico/time.h"

#include "tusb.h"

#include <stdio.h>
#include <string.h>

/* Deliberately not the 4-button combo (game mode, 700 ms): a hand working
 * toward this 8 s hold with triangle down would fire game mode first.
 * Triangle must be UP so no hand satisfies both. */
#define DEBUG_MODE_HOLD_MS 8000u

/* If main.c's loop doesn't feed the watchdog within this, something hung
 * and the chip resets. 1 s is far above any real pass (even several 5 ms
 * I2C/PIO timeouts in one pass), and recovers within a second. */
#define DEBUG_WATCHDOG_TIMEOUT_MS 1000u

/* A few loop passes' worth of trace (~17 characters per pass), enough to
 * see a pattern, not just the last letter. */
#define DEBUG_TRACE_RING_SIZE 256u

/* "TILE": marks valid data in __uninitialized_ram, which holds random
 * bits after a real power-on and must never be trusted without a check. */
#define DEBUG_CRASH_MAGIC 0x454c4954u

typedef struct {
    char ring[DEBUG_TRACE_RING_SIZE];
    uint16_t ring_pos; /* next write index, wraps */
    uint32_t uptime_ms; /* last time anything was recorded */
} debug_trace_ring_t;

typedef struct {
    uint32_t magic;
    debug_trace_ring_t ring_data;
    bool reported;
} debug_crash_snapshot_t;

/* These survive a watchdog reset (SRAM keeps its contents; only power
 * loss clears it), unlike ordinary statics. s_debug_mode_active is here
 * too, so debug mode stays on across repeated crashes instead of needing
 * the combo again after each recovery. Whether each is trusted on a given
 * boot is decided in tiles_debug_mode_capture_crash_snapshot(). */
static debug_trace_ring_t __uninitialized_ram(s_live_trace);
static debug_crash_snapshot_t __uninitialized_ram(s_crash_snapshot);
static bool __uninitialized_ram(s_debug_mode_active);

static bool s_combo_held;
static uint32_t s_combo_start_ms;
static bool s_combo_triggered_this_hold;
/* Set when debug mode resumed after a crash-recovery boot; the report is
 * dumped once from tiles_debug_mode_scan() after USB has had time to
 * enumerate. */
static bool s_pending_boot_dump;

static void record_to_live_ring(const char *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        s_live_trace.ring[s_live_trace.ring_pos] = data[i];
        s_live_trace.ring_pos = (uint16_t)((s_live_trace.ring_pos + 1u) % DEBUG_TRACE_RING_SIZE);
    }
    s_live_trace.uptime_ms = to_ms_since_boot(get_absolute_time());
}

/* The non-blocking CDC send. The report dump calls it directly (not via
 * tiles_debug_trace()) so printing the report doesn't record into the ring
 * it is reading. */
static void cdc_write_raw(const char *data, uint32_t len) {
    if (!s_debug_mode_active || len == 0u) {
        return;
    }
    uint32_t available = tud_cdc_write_available();
    if (available == 0u) {
        return;
    }
    if (len > available) {
        len = available;
    }
    tud_cdc_write(data, len);
}

static void cdc_write_raw_str(const char *s) {
    if (s == NULL) {
        return;
    }
    cdc_write_raw(s, (uint32_t)strlen(s));
}

/* Budget for one whole report dump (shared by every cdc_write_paced() in
 * it). Must stay under the watchdog timeout: at 2000 ms, a dump with no
 * host connected outlasted the 1 s watchdog, reset the board, and re-armed
 * the same dump on the next boot, a reset loop of its own making. (The
 * loop below also feeds the watchdog.) */
#define DEBUG_REPORT_DUMP_TIMEOUT_MS 400u

/* Writes `s` in chunks, pumping tud_task() and flushing between them so
 * the 64-byte CDC FIFO drains (queued back to back, the first real report
 * lost most of its text). Bounded by the shared `deadline`: with no host,
 * room never appears and an unbounded loop would hang. Feeds the watchdog
 * while waiting, since waiting on a host is progress, not a hang. Returns
 * false once the deadline passes so the caller stops. */
static bool cdc_write_paced(const char *s, absolute_time_t deadline) {
    if (s == NULL) {
        return true;
    }
    uint32_t len = (uint32_t)strlen(s);
    uint32_t sent = 0u;
    while (sent < len) {
        tud_task();
        watchdog_update();
        if (time_reached(deadline)) {
            return false;
        }
        uint32_t available = tud_cdc_write_available();
        if (available == 0u) {
            continue;
        }
        uint32_t chunk = len - sent;
        if (chunk > available) {
            chunk = available;
        }
        uint32_t written = tud_cdc_write(s + sent, chunk);
        sent += written;
        tud_cdc_write_flush();
    }
    return true;
}

/* Prints the snapshot once, oldest to newest (starting at ring_pos), so
 * the LAST characters printed are the last thing that happened. Uses
 * cdc_write_paced() throughout. */
static void dump_crash_report_if_pending(void) {
    if (s_crash_snapshot.magic != DEBUG_CRASH_MAGIC || s_crash_snapshot.reported) {
        return;
    }

    /* Marked reported BEFORE dumping: a dump that fails (no host) is treated
     * as lost rather than retried on every later entry. Delivery is never
     * guaranteed; not blocking is. */
    s_crash_snapshot.reported = true;

    absolute_time_t deadline = make_timeout_time_ms(DEBUG_REPORT_DUMP_TIMEOUT_MS);
    bool ok = cdc_write_paced("\r\n=== CRASH REPORT: watchdog recovered a hang ===\r\n", deadline);

    if (ok) {
        char header[64];
        snprintf(header, sizeof(header), "Uptime when it froze: %lu ms\r\n",
                 (unsigned long)s_crash_snapshot.ring_data.uptime_ms);
        ok = cdc_write_paced(header, deadline);
    }
    if (ok) {
        ok = cdc_write_paced("Last activity before the freeze (oldest to newest):\r\n", deadline);
    }
    if (ok) {
        /* Built into one string and sent in one paced write rather than one
         * tud_task() pump per character. +1 for the terminator. */
        char ring_text[DEBUG_TRACE_RING_SIZE + 1u];
        uint16_t ring_text_len = 0u;
        for (uint16_t i = 0; i < DEBUG_TRACE_RING_SIZE; i++) {
            uint16_t idx = (uint16_t)((s_crash_snapshot.ring_data.ring_pos + i) % DEBUG_TRACE_RING_SIZE);
            char c = s_crash_snapshot.ring_data.ring[idx];
            if (c != '\0') {
                ring_text[ring_text_len++] = c;
            }
        }
        ring_text[ring_text_len] = '\0';
        ok = cdc_write_paced(ring_text, deadline);
    }
    if (ok) {
        cdc_write_paced("\r\n=== END REPORT ===\r\n", deadline);
    }
}

void tiles_debug_mode_capture_crash_snapshot(bool crash_recovered) {
    /* uptime_ms != 0 as a sanity check (a real crash ran for some time) on
     * top of the watchdog flag. */
    bool trace_looks_real = crash_recovered && s_live_trace.uptime_ms != 0u;

    if (trace_looks_real) {
        /* s_live_trace still holds the moment of the hang: copy it now, before
         * any init below starts tracing over it. */
        s_crash_snapshot.ring_data = s_live_trace;
        s_crash_snapshot.magic = DEBUG_CRASH_MAGIC;
        s_crash_snapshot.reported = false;
        /* s_debug_mode_active survived the reset too: if it was on, tracing and
         * the pulse resume automatically. The report is dumped a couple of
         * seconds later from tiles_debug_mode_scan(), since USB hasn't been
         * serviced at all yet this early. */
        s_pending_boot_dump = s_debug_mode_active;
    } else if (!crash_recovered) {
        /* Fresh boot (power-on, reset pin, reflash): leftover SRAM can't be
         * trusted, start off. */
        s_debug_mode_active = false;
    }
}

void tiles_debug_mode_init(void) {
    s_combo_held = false;
    s_combo_start_ms = 0u;
    s_combo_triggered_this_hold = false;
    /* The snapshot and s_pending_boot_dump were already settled by
     * tiles_debug_mode_capture_crash_snapshot() at the top of main(). */

    memset(&s_live_trace, 0, sizeof(s_live_trace));

    /* pause_on_debug: a hardware debugger at a breakpoint shouldn't trigger a
     * "crash" reset. */
    watchdog_enable(DEBUG_WATCHDOG_TIMEOUT_MS, true);
}

/* How long after boot to wait for USB to re-enumerate before the
 * auto-resumed report dump. Generous; it only happens once. */
#define DEBUG_BOOT_DUMP_DELAY_MS 2000u

void tiles_debug_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (s_pending_boot_dump && now_ms >= DEBUG_BOOT_DUMP_DELAY_MS) {
        s_pending_boot_dump = false;
        dump_crash_report_if_pending();
    }

    bool diamond = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
    bool square = tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
    bool circle = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    bool triangle = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);

    bool combo_now = diamond && square && circle && !triangle;

    if (combo_now && !s_combo_held) {
        s_combo_held = true;
        s_combo_start_ms = now_ms;
        s_combo_triggered_this_hold = false;
    } else if (!combo_now) {
        s_combo_held = false;
    }

    /* Toggles once at 8 s, not repeatedly while the hold continues. */
    if (s_combo_held && !s_combo_triggered_this_hold && (now_ms - s_combo_start_ms) >= DEBUG_MODE_HOLD_MS) {
        s_combo_triggered_this_hold = true;
        s_debug_mode_active = !s_debug_mode_active;
        if (s_debug_mode_active) {
            dump_crash_report_if_pending();
        }
    }
}

bool tiles_debug_mode_is_active(void) {
    return s_debug_mode_active;
}

void tiles_debug_trace(char code) {
    record_to_live_ring(&code, 1);
    cdc_write_raw(&code, 1);
}

void tiles_debug_trace_str(const char *s) {
    if (s == NULL) {
        return;
    }
    uint32_t len = (uint32_t)strlen(s);
    record_to_live_ring(s, len);
    cdc_write_raw(s, len);
}

void tiles_debug_trace_flush(void) {
    if (!s_debug_mode_active) {
        return;
    }
    tud_cdc_write_flush();
}
