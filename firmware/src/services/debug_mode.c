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

/* All three survive a watchdog reset (SRAM keeps its contents -- only
 * an actual power loss clears it), unlike ordinary `static` globals,
 * which the C runtime zeroes on every boot regardless of reset cause.
 * See pico/platform/sections.h's own __uninitialized_ram documentation.
 * s_debug_mode_active joined the other two after real feedback caught a
 * SECOND crash mid-investigation of the first: debug mode resetting to
 * off on every reboot meant live tracing (and the underglow pulse) went
 * silent right when a repeated failure needed it most, requiring the
 * combo to be re-held after every single recovery. Whether it's trusted
 * on a given boot (vs. forced to a known `false`) is decided in tiles_
 * debug_mode_init() below, the same way s_live_trace's own content is --
 * see that function's own comment. */
static debug_trace_ring_t __uninitialized_ram(s_live_trace);
static debug_crash_snapshot_t __uninitialized_ram(s_crash_snapshot);
static bool __uninitialized_ram(s_debug_mode_active);

static bool s_combo_held;
static uint32_t s_combo_start_ms;
static bool s_combo_triggered_this_hold;
/* Set in tiles_debug_mode_init() when debug mode auto-resumed active
 * after a crash-recovery reboot -- session-local (ordinary .bss is
 * fine here, this only ever needs to matter for the boot that sets it),
 * consumed once by tiles_debug_mode_scan() a couple seconds in, once
 * USB has had time to actually re-enumerate. See that function's own
 * comment for why this can't just dump immediately from init(). */
static bool s_pending_boot_dump;

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

/* Total budget for one entire report dump (every cdc_write_paced() call
 * within it shares this SAME deadline, computed once by the caller) --
 * generous for the success case (a real terminal draining the port
 * finishes in a small fraction of this), but a hard stop otherwise.
 * Real feedback, after this exact mechanism made the problem WORSE
 * instead of better: "this was not as prominent of an issue before."
 * Root cause: this used to be 2000ms -- LONGER than DEBUG_WATCHDOG_
 * TIMEOUT_MS's own 1000ms -- and this entire dump runs synchronously
 * inside ONE main-loop iteration, with watchdog_update() only reached
 * at the very end of main.c's loop, after tiles_debug_mode_scan() (and
 * so this whole dump) has already returned. If nobody was connected
 * yet right when the auto-resumed dump fired (exactly the case right
 * after a crash-recovery reboot, before a host-side monitor has had a
 * chance to reconnect), the dump could burn most of its own 2-second
 * budget waiting for FIFO room that never appeared -- comfortably
 * exceeding the watchdog's 1-second one WHILE STILL INSIDE THIS SAME
 * FUNCTION, triggering ANOTHER watchdog reset before the dump even
 * finished. That reset re-arms the identical auto-resume dump on the
 * very next boot, which could hit the exact same problem again -- a
 * self-inflicted reset loop from the crash reporter itself, layered on
 * top of whatever the original tud_task() freeze's own real frequency
 * is, and entirely capable of explaining "it did it again" happening
 * MORE often right after this feature shipped, not less. Fixed two
 * ways, not one -- see watchdog_update() a few lines below for the
 * more important of the two: this constant also dropped to comfortably
 * UNDER the watchdog timeout as defense in depth, so even code that
 * forgets to pet the watchdog during a long wait can't reproduce this
 * class of bug against this specific timer again. */
#define DEBUG_REPORT_DUMP_TIMEOUT_MS 400u

/* Writes `s` a few bytes at a time, pumping tud_task() and flushing
 * between chunks so the USB stack actually gets a chance to drain the
 * 64-byte CDC TX FIFO (CFG_TUD_CDC_TX_BUFSIZE) before the next chunk
 * tries to queue more into it. Without this, a report's worth of text
 * (a few hundred bytes) queued back-to-back with nothing pumping in
 * between overflows that FIFO fast -- cdc_write_raw()'s own "truncate
 * to whatever's available" contract (correctly non-blocking, see this
 * file's header) then silently drops most of it, which is exactly what
 * happened the first time this ran on real hardware: the report came
 * back with whole phrases missing, different calls' output concatenated
 * mid-word. tud_task() is safe to call this often -- it's designed to
 * be pumped frequently and returns quickly when there's nothing to do.
 * Bounded by `deadline` (shared across an entire dump, see the caller)
 * rather than looping until every byte is confirmed sent -- if nobody's
 * actually connected to drain the port, `available` never recovers, and
 * an unbounded version of this exact loop would hang forever waiting
 * for room that will never appear. That would be a real, ugly irony:
 * the freeze-diagnostic tool causing a NEW freeze of its own, exactly
 * the failure class this entire session has been about removing.
 * Pets the hardware watchdog on every spin, same as main.c's own loop
 * does once per iteration -- this loop can legitimately run for a
 * while (waiting on a host that isn't connected yet is expected, not a
 * hang: tud_task() is being called and real progress is being checked
 * for on every pass), and DEBUG_REPORT_DUMP_TIMEOUT_MS's own history
 * just above is exactly what happens when a bounded-but-slow operation
 * like this one isn't distinguished from an actual stuck main loop.
 * Returns false the moment the shared deadline is reached (whether or
 * not this specific call finished), so the caller can stop attempting
 * the rest of the report rather than let each remaining piece burn its
 * own full timeout in turn. */
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

/* Called once, the first time debug mode is entered after a crash-
 * recovery reboot -- see this file's own header for the full mechanism.
 * Prints the ring in chronological order (oldest to newest, starting
 * from ring_pos -- the slot about to be overwritten next, i.e. the
 * oldest still-held byte) so the LAST characters printed are
 * unambiguously the last thing that happened, not scattered wherever
 * ring_pos physically landed. Uses cdc_write_paced() above throughout,
 * NOT the plain cdc_write_raw()/tiles_debug_trace_str() this file uses
 * everywhere else -- see that function's own comment for why a report
 * this size specifically needs the pacing. */
static void dump_crash_report_if_pending(void) {
    if (s_crash_snapshot.magic != DEBUG_CRASH_MAGIC || s_crash_snapshot.reported) {
        return;
    }

    /* Marked reported UP FRONT, not after a successful dump -- if nobody
     * turns out to be connected and every write below times out, the
     * alternative (retry on every future debug-mode entry) would just
     * mean every subsequent entry re-burns the same 2-second budget on
     * a dump that's already proven doomed. A report that failed to
     * deliver once is treated as lost, not retried -- consistent with
     * this whole feature's own "never guarantee delivery, only ever
     * guarantee not blocking" contract (see this file's header). */
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
        /* Built into a plain string and sent through cdc_write_paced()
         * in one go, rather than one tud_task()-pumped call per
         * character -- a few hundred individual pumps for the ring
         * alone would work but is needless overhead for a one-time dump
         * when batching is just as safe. +1 for the null terminator,
         * DEBUG_TRACE_RING_SIZE '\0' ring slots (an all-empty ring, the
         * theoretical minimum) still leaves room for it. */
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

void tiles_debug_mode_init(void) {
    s_combo_held = false;
    s_combo_start_ms = 0u;
    s_combo_triggered_this_hold = false;
    s_pending_boot_dump = false;

    /* Checked BEFORE (re-)arming the watchdog below, against THIS boot's
     * own reset cause -- confirmed against the pico-sdk's own header
     * comment to read false after a normal `picotool load -x` reflash
     * (that goes through watchdog_reboot()/the bootrom's own UF2 path,
     * which clears the specific scratch marker this checks for), so a
     * routine firmware update is never mistaken for a crash. */
    bool crash_recovered = watchdog_enable_caused_reboot() && s_live_trace.uptime_ms != 0u;

    if (crash_recovered) {
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
        /* s_debug_mode_active is trusted as-is here -- it too survived
         * the reset (see this file's header for why it joined the
         * other two __uninitialized_ram fields), so if it was true the
         * instant before the hang, it's still true now: live tracing
         * and the underglow pulse resume automatically, no re-entry
         * needed. The pending report gets dumped from tiles_debug_
         * mode_scan() a couple seconds in, not immediately here -- USB
         * hasn't been serviced even once yet at this point in boot
         * (tud_task() first runs at the top of main.c's own loop, which
         * hasn't started), so cdc_write_paced()'s pumping would just be
         * racing a re-enumeration that hasn't happened yet. */
        s_pending_boot_dump = s_debug_mode_active;
    } else {
        /* Genuinely fresh boot (power-on, RUN-pin reset, or a normal
         * picotool reflash) -- s_debug_mode_active's leftover SRAM
         * content can't be trusted the way it can after a confirmed
         * crash-recovery reboot, so start from a known state. */
        s_debug_mode_active = false;
    }

    memset(&s_live_trace, 0, sizeof(s_live_trace));

    /* pause_on_debug=true: if a hardware debugger is ever attached to
     * this board, stepping through breakpoints shouldn't spuriously
     * trigger a "crash" reset. */
    watchdog_enable(DEBUG_WATCHDOG_TIMEOUT_MS, true);
}

/* How long after boot to trust that USB has actually finished
 * re-enumerating before attempting the auto-resumed report dump --
 * generous (real enumeration is typically much faster), and this only
 * ever needs to happen once, so there's no cost to being conservative
 * here. Measured from boot, not from the crash -- irrelevant how long
 * the PREVIOUS session ran, only how long THIS one has had to get USB
 * back up. */
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
