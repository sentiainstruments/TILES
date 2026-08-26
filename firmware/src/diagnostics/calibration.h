#pragma once

/*
 * Serial-driven Hall calibration capture: a stand-in for the real
 * calibration flow that will eventually live behind usb_vendor/ and the
 * companion app (neither exists yet) -- exposed over the same USB-CDC
 * stdio channel diagnostics/i2c_scan.c already uses, single-character
 * commands typed into any serial terminal.
 *
 * Three-step flow, matching how a rest/full-press/max-press capture
 * actually has to happen physically (one gesture across all 24 pads at
 * a time, not per-pad):
 *
 *   1. Hands off every pad, then send 'r' -- re-captures hall.c's
 *      rest-Z baseline for every pad from a fresh read (see
 *      tiles_hall_recapture_baseline()). Needed because the boot-time
 *      baseline is only as good as whatever was resting on the pads at
 *      power-on -- e.g. meaningless before the magnets are in their
 *      final position.
 *   2. Press every pad down to a normal, regular full press, then send
 *      'f' -- snapshots tiles_hall_get_depth() for every pad against
 *      the just-captured baseline and prints a table + average.
 *   3. Press every pad down as hard/far as it physically goes, then
 *      send 'm' -- same snapshot, labeled "max-press", for the
 *      saturation end of the range.
 *
 * This does NOT persist anything (storage/ doesn't exist yet) or
 * compute/apply a calibration curve automatically -- it prints raw
 * numbers for a human to read off the terminal and use to pick real
 * constants (e.g. expression.c's aftertouch full-scale depth default,
 * standby.c's TILES_STANDBY_HALL_WAKE_DEPTH) instead of guessing, and
 * that picking/editing still happens by hand afterward.
 */

#include <stdint.h>

/* Prints the command summary once. Call after tiles_hall_init(). */
void tiles_calibration_init(void);

/* Polls stdio for one command character (non-blocking -- a zero-timeout
 * getchar_timeout_us() read, so this is cheap to call every main-loop
 * iteration even when nothing has been typed) and acts on it if present.
 * Call every main-loop iteration, after tiles_hall_scan(). */
void tiles_calibration_scan(void);
