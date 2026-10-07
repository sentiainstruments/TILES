#pragma once

/* Debug mode and crash recorder, for finding real-hardware freezes.
 *
 * Hold diamond+square+circle (triangle UP) for 8 s to toggle. While on,
 * the underglow pulses Sentia magenta (services/lighting.c overrides it
 * whatever mode owns rendering; red means a crash, see
 * crash_indicator.h) and trace characters stream live over the USB-CDC
 * console: the last character before the stream stops is where it hung.
 *
 * Tracing never uses printf(): a blocked USB-CDC printf was itself the
 * cause of several freezes. tiles_debug_trace()/_str() use tud_cdc_write()
 * directly, which never blocks (checked against TinyUSB's cdc_device.c);
 * if nobody drains the console, trace bytes are dropped, never waited for.
 *
 * ---- Surviving the freeze -------------------------------------------------
 * Always on, whether or not debug mode is toggled:
 *
 * 1. Every trace call is also recorded into a ring buffer in
 *    `__uninitialized_ram`, which survives a reset (unlike .bss).
 * 2. A 1 s hardware watchdog turns a hung main loop (main.c feeds it once
 *    per pass) into a reset instead of a frozen instrument. On the next
 *    boot, watchdog_enable_caused_reboot() (false after a normal picotool
 *    reflash) triggers copying the ring into a persistent snapshot. The
 *    next time debug mode is entered, that snapshot is printed once as a
 *    crash report before live tracing continues. */

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
