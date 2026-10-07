#pragma once

/* Serial-driven Hall calibration capture over the USB-CDC console: single
 * characters typed into any serial terminal. A stand-in until the
 * companion app owns calibration.
 *
 * Calibration rule: measure TWO values per pad, rest and a strong strike.
 * A "regular" press is not repeatable (unit 2, pad 1: 33 for a normal
 * press vs 1697 for a hard strike), while "as hard as it goes" is.
 *
 *   1. Hands off every pad, send 'r': recaptures the rest-Z baseline for
 *      every pad (tiles_hall_recapture_baseline()). The boot baseline is
 *      only as good as what rested on the pads at power-on.
 *   2. Strike pads as hard as they go (a row at a time is fine), send 'm':
 *      prints each pad's depth against that baseline, plus the average.
 *
 * 'f' (a "regular full press" snapshot) still exists but is not part of
 * the procedure.
 *
 * Nothing is saved or applied: the numbers are for a human to pick
 * constants from (e.g. expression.c's aftertouch full scale,
 * standby.c's TILES_STANDBY_HALL_WAKE_DEPTH). Rest-to-strike distance is
 * also the pad's aftertouch headroom. */

#include <stdint.h>

/* Prints the command summary once. Call after tiles_hall_init(). */
void tiles_calibration_init(void);

/* Non-blocking poll for one command character (zero-timeout getchar), so
 * it's cheap every main-loop pass. Call after tiles_hall_scan(). */
void tiles_calibration_scan(void);
