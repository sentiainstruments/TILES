#pragma once

/* Hall sensor scanning: reads the 24 TMAG5273s through their TCA9548A
 * channels (every selection disables all three muxes first, so only one
 * channel is ever open) into a per-pad X/Y/Z array.
 *
 * Touched pads (services/touch.h) are read every call; untouched pads
 * round-robin one per call. Round-robin alone reaches a pad only every ~24
 * calls (~240 ms), far too slow to see a 30-80 ms strike, which
 * services/expression.c needs for velocity.
 *
 * Provides raw XYZ, a per-pad rest baseline (captured at init,
 * recapturable on demand, and slowly drift-corrected in the background per
 * docs/architecture/defaults-and-safeguards.md "Pad baseline calibration
 * and drift compensation"), and a depth magnitude from it. Depth uses Z
 * for every pad (same doc, "Sensing": straight vertical travel,
 * flat-mounted sensor). No per-pad calibration curve yet. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    uint32_t sample_time_ms; /* to_ms_since_boot() when read */
    bool valid;              /* false if the last read failed or the pad never initialized */
} tiles_hall_sample_t;

/* Disables all Hall mux channels, then sets up each sensor in turn
 * (select, identify, init, capture rest-Z baseline, deselect). Call after
 * board_i2c_init(). False if any sensor failed; see
 * tiles_hall_last_init_ok(). Failed pads are skipped by the scan instead of
 * blocking the rest.
 *
 * The baseline assumes nothing rests on the pads at power-on. If it did,
 * tiles_hall_recapture_baseline() fixes it at once; the background drift
 * tracker corrects slow thermal/mechanical drift over a session. */
bool tiles_hall_init(void);

/* True if pad (1-24)'s sensor was identified and configured at init. */
bool tiles_hall_last_init_ok(uint8_t logical_pad);

/* Reads every touched pad (in mux order), then advances the background
 * round-robin by one untouched pad and feeds that read to the drift
 * tracker (so drift is only ever corrected on untouched pads). Call every
 * main-loop pass. */
void tiles_hall_scan(void);

/* Latest raw sample for pad 1-24. Zeroed and invalid if out of range. */
tiles_hall_sample_t tiles_hall_get_sample(uint8_t logical_pad);

/* Recaptures every initialized pad's rest-Z baseline from a fresh read,
 * like init does (same "at rest now" assumption). Used by the serial
 * calibration flow (diagnostics/calibration.h). Uninitialized pads are
 * skipped. False if any read failed; that pad keeps its old baseline. */
bool tiles_hall_recapture_baseline(void);

/* |Z - rest Z| for one pad: an uncalibrated distance from rest, >= 0
 * whatever the sensor's polarity. 0 if out of range, uninitialized or
 * not yet sampled. */
uint16_t tiles_hall_get_depth(uint8_t logical_pad);
