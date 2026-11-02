#pragma once

/* Debug mode and crash recorder. Hold diamond + square + circle 8 s: the
 * underglow pulses magenta and trace characters stream on the CDC console
 * (the last one before it stops is where it hung). Tracing never printf()s
 * (a blocked printf caused freezes); unread bytes drop.
 * Always on: traces go to a ring in __uninitialized_ram, and a 1 s watchdog
 * turns a hang into a reset; the next debug session prints the ring. */

#include <stdbool.h>

/* Copies the pre-crash trace ring into the report snapshot. Call at the
 * VERY TOP of main(), right after computing crash_recovered and before
 * anything else (board_init() included): every init step traces into the
 * same ring, and when this ran later, every report showed the recovery
 * boot's own early activity instead of the hang. Touches only
 * __uninitialized_ram, so it can run that early. */
void tiles_debug_mode_capture_crash_snapshot(bool crash_recovered);

void tiles_debug_mode_init(void);

/* Call every main-loop pass (it watches for the toggle combo), after
 * tiles_buttons_scan(). */
void tiles_debug_mode_scan(void);

bool tiles_debug_mode_is_active(void);

/* Writes one trace character. No-op if debug mode is off or the CDC FIFO
 * is full (dropped). Call it right BEFORE each traced step, so the last
 * character seen names the step that hung, not the last one that finished. */
void tiles_debug_trace(char code);

/* Same contract as tiles_debug_trace(), for a short string. */
void tiles_debug_trace_str(const char *s);

/* Pushes pending trace bytes to the host now rather than waiting for a
 * full packet. Call once per main-loop pass. Non-blocking; no-op when off. */
void tiles_debug_trace_flush(void);
