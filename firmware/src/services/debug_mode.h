#pragma once

/*
 * Real-hardware trace logging -- real feedback, after two rounds of
 * guessing at (and bounding, but not curing) the recurring freeze:
 * "dont do half ass fixes, lets figure out whats wrong logically and
 * work through the bugs... implement a debug mode for the instrument
 * that lets youb live reed whats going on so i can let ir run until
 * faliure and then you can check the log for the final thing before
 * crashing."
 *
 * Hold diamond+square+circle (NOT triangle -- see tiles_debug_mode_
 * scan()'s own comment for why that's excluded) for 8 seconds to toggle.
 * Confirmed by the underglow pulsing steady red the whole time debug
 * mode is active (see services/lighting.c's own tiles_lighting_service()
 * -- it checks tiles_debug_mode_is_active() directly and overrides
 * underglow unconditionally, bypassing the standby-active ownership
 * this file deliberately stays out of, so debug mode can be toggled
 * regardless of whatever mode/sub-view currently owns rendering).
 *
 * The actual trace output is NOT printf()/stdio_usb -- this project's
 * entire reason for existing this session is that printf() over
 * USB-CDC blocks for up to 500ms per call whenever nobody's draining
 * it (see main.c's own tud_task() comment, and services/README.md's
 * extensive history of freezes traced to exactly that). Using printf()
 * for a debug-mode trace would risk becoming indistinguishable from the
 * bug it exists to diagnose. tiles_debug_trace()/_str() instead write
 * directly via tud_cdc_write() -- TinyUSB's own CDC write, confirmed by
 * reading the vendored source (cdc_device.c's tud_cdc_n_write(), a
 * plain non-blocking FIFO write; tud_cdc_n_write_flush()'s endpoint
 * claim is skip-if-busy, never a wait) to be non-blocking by
 * construction. If nobody has a terminal open to drain it, the FIFO
 * just fills and further trace bytes get silently DROPPED (checked via
 * tud_cdc_write_available() before every write) -- never queued,
 * never waited for. Debug mode trades "always eventually delivered" for
 * "never blocks the caller," which is the one property this feature
 * actually needs: watch it live with a terminal open, and the LAST
 * thing printed before the stream goes silent is where the freeze is.
 */

#include <stdbool.h>

void tiles_debug_mode_init(void);

/* Must run every main-loop iteration regardless of debug mode's current
 * state -- this is what's watching for the entry/exit combo in the
 * first place. Needs fresh button state (call after tiles_buttons_scan()). */
void tiles_debug_mode_scan(void);

bool tiles_debug_mode_is_active(void);

/* Writes one trace character -- no-op if debug mode is inactive, or if
 * the CDC TX FIFO has no room right now (dropped, not queued). Meant
 * for a single letter marking "about to run stage X," called
 * immediately BEFORE each traced operation so the last character seen
 * if something hangs is unambiguously where it's stuck, not merely the
 * last thing that finished. */
void tiles_debug_trace(char code);

/* Same non-blocking/drop-if-full contract as tiles_debug_trace(), for a
 * short null-terminated string instead of one character. */
void tiles_debug_trace_str(const char *s);

/* Pushes whatever's been written by tiles_debug_trace()/_str() so far
 * out to the host promptly instead of waiting for TinyUSB's own
 * fill-a-full-packet auto-flush (see cdc_device.c's tud_cdc_n_write()) --
 * call once per main-loop iteration, not per trace call. No-op (and
 * non-blocking either way) if debug mode is inactive. */
void tiles_debug_trace_flush(void);
