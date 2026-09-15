#include "debug_mode.h"

#include "board_layout.h"
#include "buttons.h"

#include "hardware/watchdog.h"
#include "pico/platform/sections.h"
#include "pico/time.h"

#include "tusb.h"

#include <stdio.h>
#include <string.h>

/* Real feedback: "how diamond square circle hold for 8 secodns." Deliberately
 * NOT the reserved 4-button combo (triangle+diamond+square+circle,
 * services/game_mode.c's own GM_HOLD_MS) -- that one triggers after only
 * 700ms, so a hand that includes triangle while working toward this
 * combo's 8-second hold would fire game mode's secret entry first,
 * several seconds before debug mode itself would ever toggle. Requiring
 * triangle to be UP (not just "don't care") keeps the two combos from
 * ever being satisfied by the same held hand. */
#define DEBUG_MODE_HOLD_MS 8000u

/* Real feedback: "let it run until faliure and then you can check the
 * log for the final thing before crashing" -- if the main loop ever
 * fails to reach the watchdog_update() call at the bottom of main.c's
 * loop within this long, something upstream hung, and the watchdog
 * resets the chip rather than leaving it frozen forever. 1 second is
 * generous headroom over any realistic loop iteration -- even a
 * pathological one where several of this session's own 5ms I2C/PIO
 * timeouts all fired in the same pass would still land well under it --
 * while still recovering (and preserving a crash snapshot, see below)
 * within a second of a genuine hang rather than requiring someone to
 * notice and physically power-cycle it. */
#define DEBUG_WATCHDOG_TIMEOUT_MS 1000u

/* Ring buffer sized for a handful of full loop iterations' worth of
 * trace characters (main.c's own per-stage markers are ~17 characters
 * per pass) -- enough to see a pattern (stuck retrying the same stage
 * vs. reaching it once and stopping), not just the single last letter. */
#define DEBUG_TRACE_RING_SIZE 256u

/* "TILE" as a plain 32-bit constant -- distinctive against whatever
 * random bits SRAM powers up with on a genuine first-ever boot (see
 * this file's own header for why __uninitialized_ram is NOT zeroed the
 * way ordinary .bss is, and so must never be trusted without a check
 * like this one). */
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

/* Both survive a watchdog reset (SRAM keeps its contents -- only an
 * actual power loss clears it), unlike ordinary `static` globals, which
 * the C runtime zeroes on every boot regardless of reset cause. See
 * pico/platform/sections.h's own __uninitialized_ram documentation. */
static debug_trace_ring_t __uninitialized_ram(s_live_trace);
static debug_crash_snapshot_t __uninitialized_ram(s_crash_snapshot);

static bool s_debug_mode_active;
static bool s_combo_held;
static uint32_t s_combo_start_ms;
static bool s_combo_triggered_this_hold;

static void record_to_live_ring(const char *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        s_live_trace.ring[s_live_trace.ring_pos] = data[i];
        s_live_trace.ring_pos = (uint16_t)((s_live_trace.ring_pos + 1u) % DEBUG_TRACE_RING_SIZE);
    }
    s_live_trace.uptime_ms = to_ms_since_boot(get_absolute_time());
}

/* The actual non-blocking CDC send, shared by tiles_debug_trace()/_str()
 * above the live ring recording and by the crash-report dump below --
 * the dump deliberately calls this directly rather than going through
 * tiles_debug_trace()/_str(), so printing the REPORT doesn't itself get
 * recorded into the live ring it's busy reading from. */
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

/* Called once, the first time debug mode is entered after a crash-
 * recovery reboot -- see this file's own header for the full mechanism.
 * Prints the ring in chronological order (oldest to newest, starting
 * from ring_pos -- the slot about to be overwritten next, i.e. the
 * oldest still-held byte) so the LAST characters printed are
 * unambiguously the last thing that happened, not scattered wherever
 * ring_pos physically landed. */
static void dump_crash_report_if_pending(void) {
    if (s_crash_snapshot.magic != DEBUG_CRASH_MAGIC || s_crash_snapshot.reported) {
        return;
    }

    cdc_write_raw_str("\r\n=== CRASH REPORT: watchdog recovered a hang ===\r\n");
    char header[64];
    snprintf(header, sizeof(header), "Uptime when it froze: %lu ms\r\n",
             (unsigned long)s_crash_snapshot.ring_data.uptime_ms);
    cdc_write_raw_str(header);
    cdc_write_raw_str("Last activity before the freeze (oldest to newest):\r\n");
    for (uint16_t i = 0; i < DEBUG_TRACE_RING_SIZE; i++) {
        uint16_t idx = (uint16_t)((s_crash_snapshot.ring_data.ring_pos + i) % DEBUG_TRACE_RING_SIZE);
        char c = s_crash_snapshot.ring_data.ring[idx];
        if (c != '\0') {
            cdc_write_raw(&c, 1);
        }
    }
    cdc_write_raw_str("\r\n=== END REPORT ===\r\n");
    tud_cdc_write_flush();

    s_crash_snapshot.reported = true;
}

void tiles_debug_mode_init(void) {
    s_debug_mode_active = false;
    s_combo_held = false;
    s_combo_start_ms = 0u;
    s_combo_triggered_this_hold = false;

    /* Checked BEFORE (re-)arming the watchdog below, against THIS boot's
     * own reset cause -- confirmed against the pico-sdk's own header
     * comment to read false after a normal `picotool load -x` reflash
     * (that goes through watchdog_reboot()/the bootrom's own UF2 path,
     * which clears the specific scratch marker this checks for), so a
     * routine firmware update is never mistaken for a crash. */
    if (watchdog_enable_caused_reboot() && s_live_trace.uptime_ms != 0u) {
        /* s_live_trace still holds whatever was being recorded at the
         * exact moment of the hang -- SRAM survives a watchdog reset,
         * only real power loss clears it. Snapshot it into the
         * separately-persisted struct before normal recording resumes
         * and starts overwriting s_live_trace fresh -- without this
         * copy, the old content would be gone within the first handful
         * of loop iterations after reboot, long before debug mode could
         * ever be re-entered to read it. Guarded on uptime_ms != 0 as a
         * cheap sanity check (a real crash always ran for SOME time
         * first) rather than trusting watchdog_enable_caused_reboot()
         * alone against whatever garbage s_live_trace might hold if
         * this specific boot path is ever reached in a way this
         * reasoning didn't anticipate. */
        s_crash_snapshot.ring_data = s_live_trace;
        s_crash_snapshot.magic = DEBUG_CRASH_MAGIC;
        s_crash_snapshot.reported = false;
    }

    memset(&s_live_trace, 0, sizeof(s_live_trace));

    /* pause_on_debug=true: if a hardware debugger is ever attached to
     * this board, stepping through breakpoints shouldn't spuriously
     * trigger a "crash" reset. */
    watchdog_enable(DEBUG_WATCHDOG_TIMEOUT_MS, true);
}

void tiles_debug_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
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

    /* One-shot per hold, matching this codebase's own established
     * hold-timer shape (e.g. services/game_mode.c's GM_HOLD_MS,
     * services/op_mode.c's diamond record-arm hold) -- toggles exactly
     * once at the 8-second mark, not repeatedly for as long as the hold
     * continues past it. */
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
