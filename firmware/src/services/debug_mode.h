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
 *
 * ---- Surviving the freeze itself ----------------------------------------
 *
 * The above only helps if a terminal happened to be open and someone was
 * watching at the exact moment it froze. Real feedback afterward: "yeah
 * [add the power-mode trace] and implement in case of crash a report is
 * generated capturing last activity before blackout." Two more pieces,
 * both always-on regardless of whether debug mode itself is toggled:
 *
 * 1. tiles_debug_trace()/_str() now ALSO record into a small ring buffer
 *    placed in `__uninitialized_ram` (pico/platform/sections.h) -- RAM
 *    that survives a reset (unlike ordinary `.bss`, which the C runtime
 *    zeroes on every boot, this section is explicitly left alone).
 *    Recording into it is just a couple of memory writes, unconditional
 *    and free of any of the blocking/dropping concerns above, so there's
 *    no reason to gate it behind whether anyone's watching live.
 *
 * 2. A hardware watchdog (hardware/watchdog.h), enabled here for the
 *    first time in this project, turns "the main loop stopped calling
 *    watchdog_update()" (main.c's job, once per iteration, at the very
 *    end -- if a hang happens anywhere upstream, that call is never
 *    reached) into an ACTUAL RESET after 1 second, instead of the
 *    instrument sitting frozen forever until someone finds the cable and
 *    power-cycles it. On the reboot that follows, tiles_debug_mode_init()
 *    checks watchdog_enable_caused_reboot() -- confirmed against the
 *    pico-sdk's own header comment to correctly read false after a
 *    normal `picotool load -x` reflash (that goes through a DIFFERENT
 *    watchdog_reboot() path that clears the same marker), so a routine
 *    firmware update never gets mistaken for a crash. If it reads true,
 *    whatever the ring buffer held at that exact moment (SRAM retains
 *    its contents across a watchdog reset -- only an actual power loss
 *    clears it) gets copied into a second, similarly-persistent snapshot
 *    before normal recording resumes fresh. The NEXT time debug mode is
 *    entered (same 8-second hold), that snapshot -- if one is pending
 *    and hasn't already been shown -- is dumped as a one-time readable
 *    report before live tracing continues, then marked as reported so it
 *    doesn't repeat on every subsequent entry. No new gesture, no
 *    separate retrieval step: notice something seemed off, hold the
 *    combo, and the last thing that happened is right there.
 */

#include <stdbool.h>

/* Root-cause finding from this session's own crash-report analysis:
 * EVERY report captured so far -- both boards, independently, several
 * times -- showed the identical signature, "uptime when it froze: 20
 * ms" and a ring completely full of just one write_pad() call's own
 * trace characters repeating, nothing else interleaved. That's not the
 * real freeze -- tiles_debug_mode_init() (below) used to be where the
 * ring got copied into the reportable snapshot, and main() doesn't
 * call it until AFTER tiles_lighting_init()/tiles_buttons_init()/
 * tiles_touch_init()/tiles_hall_init()/the boot sequence have all
 * already run and called tiles_debug_trace() themselves -- since
 * record_to_live_ring() (debug_mode.c) writes unconditionally,
 * regardless of whether debug mode itself is toggled on, every one of
 * those calls overwrites more of whatever the ring held from the
 * ACTUAL moment of the original hang, long before the snapshot copy
 * ever ran. Every report this session has shown the recovery boot's
 * OWN early activity, never the real thing. Must be called at the
 * very top of main(), immediately after computing crash_recovered via
 * watchdog_enable_caused_reboot() and before ANYTHING else (including
 * board_init()) runs -- only __uninitialized_ram state, no hardware
 * dependency, so there's nothing stopping it running that early. */
void tiles_debug_mode_capture_crash_snapshot(bool crash_recovered);

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
