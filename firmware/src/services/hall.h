#pragma once

/*
 * Hall sensor scanning: sequences through all 24 pads' TMAG5273 sensors
 * via their TCA9548A mux channels (only one channel across all three
 * muxes enabled at a time, enforced structurally here -- every
 * selection disables all three muxes first), reading raw X/Y/Z into a
 * per-pad array.
 *
 * Scan priority: a touched pad (per services/touch.h) is scanned every
 * call; untouched pads round-robin, one per call, in the background.
 * A pure round-robin only reaches a given pad roughly every 24 calls
 * (~240ms at the current main-loop rate) -- nowhere near fast enough to
 * see a 30-80ms finger strike happen, which is what
 * services/expression.c needs to derive velocity from. Prioritizing
 * touched pads concentrates sampling where a strike could actually be
 * in progress.
 *
 * V1 scope: raw XYZ + a per-pad rest baseline (captured once at init)
 * and a derived depth magnitude from it. No axis selection (deciding
 * whether Z is really the right axis for every pad, vs X/Y) -- Z is
 * assumed for all pads per docs/architecture/defaults-and-safeguards.md
 * "V1 sensing scope" (magnet motion is expected to project mostly onto
 * Z given the switch's straight vertical travel and the sensor's flat
 * mount below it). No per-pad calibration curve, no tilt/lateral use of
 * X/Y yet.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    uint32_t sample_time_ms; /* to_ms_since_boot() at the moment this sample was taken */
    bool valid;              /* false if the last read for this pad failed, or it was never successfully initialized */
} tiles_hall_sample_t;

/* Disables all Hall mux channels, then configures all 24 physical
 * TMAG5273 sensors one at a time (select channel -> identify -> init ->
 * capture a rest-Z baseline -> deselect all). Must run after
 * board_i2c_init(). Returns false if any sensor failed identification
 * or configuration; check tiles_hall_last_init_ok() per pad to see
 * which ones. A pad that failed init is skipped by tiles_hall_scan()
 * rather than blocking the rest -- matches the "a failed subsystem
 * disables itself" principle.
 *
 * The baseline capture assumes every pad is at rest (untouched) at the
 * moment this runs -- true for a normal boot, not necessarily true if
 * something is resting on a pad right at power-on. No re-baseline
 * mechanism exists yet (see the defaults doc's gated slow-tracker
 * design, not implemented). */
bool tiles_hall_init(void);

/* True if pad (1-24)'s sensor was successfully identified and
 * configured during tiles_hall_init(). */
bool tiles_hall_last_init_ok(uint8_t logical_pad);

/* Services one pad per call if nothing is touched; if any pads are
 * touched, services every touched pad this call (in mux order) before
 * returning, then still advances the background round-robin by one
 * untouched pad. Call every main-loop iteration. */
void tiles_hall_scan(void);

/* Latest raw sample for one pad (1-24). Returns a zeroed, invalid
 * sample if the pad number is out of range. */
tiles_hall_sample_t tiles_hall_get_sample(uint8_t logical_pad);

/* |current Z - rest-baseline Z| for one pad -- an uncalibrated
 * "how far from rest" magnitude, always >= 0 regardless of the sensor's
 * actual (currently unknown) polarity. 0 if the pad is out of range,
 * was never initialized, or has no valid sample yet. */
uint16_t tiles_hall_get_depth(uint8_t logical_pad);
