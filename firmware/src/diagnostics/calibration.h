#pragma once

/*
 * Serial-driven Hall calibration capture: a stand-in for the real
 * calibration flow that will eventually live behind usb_vendor/ and the
 * companion app (neither exists yet) -- exposed over the same USB-CDC
 * stdio channel diagnostics/i2c_scan.c already uses, single-character
 * commands typed into any serial terminal.
 *
 * **Standing calibration rule, real feedback: "calculate two values. no
 * press and strong strke."** Only two measurements matter in practice --
 * rest baseline and one deliberate strong strike per pad -- not a
 * separate "regular full press" in between. Real per-unit data backs
 * this: on one earlier unit, a normal full press already bottomed the
 * pad out mechanically (no meaningfully harder position past it -- see
 * diagnostics/README.md's own capture-session entry), while on another
 * (unit 2), a normal press and a real hard strike gave wildly different
 * depths (33 vs 1697 on one sampled pad) -- the "regular" number is
 * inherently unreliable to standardize by feel from one attempt to the
 * next, but "as hard as it physically goes" is a stable, repeatable
 * target either way. The workflow is now just:
 *
 *   1. Hands off every pad, then send 'r' -- re-captures hall.c's
 *      rest-Z baseline for every pad from a fresh read (see
 *      tiles_hall_recapture_baseline()). Needed because the boot-time
 *      baseline is only as good as whatever was resting on the pads at
 *      power-on -- e.g. meaningless before the magnets are in their
 *      final position.
 *   2. Press a pad (or all of them, if one person can manage it --
 *      realistically this means one row/section at a time, or one pad
 *      at a time reading off just that pad's own row from the printed
 *      table) as hard/far as it physically goes, then send 'm' --
 *      snapshots tiles_hall_get_depth() for every pad against the
 *      just-captured baseline and prints a table + average.
 *
 * 'f' (a separate "regular full press" snapshot, distinct from 'm')
 * still exists below for now -- removing it wasn't asked for -- but it
 * is NOT part of the real calibration procedure anymore; don't reach
 * for it as a first step.
 *
 * This does NOT persist anything (storage/ doesn't exist yet) or
 * compute/apply a calibration curve automatically -- it prints raw
 * numbers for a human to read off the terminal and use to pick real
 * constants (e.g. expression.c's aftertouch full-scale depth default,
 * standby.c's TILES_STANDBY_HALL_WAKE_DEPTH) instead of guessing, and
 * that picking/editing still happens by hand afterward. The gap between
 * baseline and the strong-strike depth is also the practical "aftertouch
 * headroom" for a given pad/unit -- how much further travel exists past
 * note-on for continued-pressure modulation to work with.
 */

#include <stdint.h>

/* Prints the command summary once. Call after tiles_hall_init(). */
void tiles_calibration_init(void);

/* Polls stdio for one command character (non-blocking -- a zero-timeout
 * getchar_timeout_us() read, so this is cheap to call every main-loop
 * iteration even when nothing has been typed) and acts on it if present.
 * Call every main-loop iteration, after tiles_hall_scan(). */
void tiles_calibration_scan(void);
