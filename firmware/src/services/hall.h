#pragma once

/*
 * Hall sensor scanning: sequences through all 24 pads' TMAG5273 sensors
 * via their TCA9548A mux channels (only one channel across all three
 * muxes enabled at a time, enforced structurally here -- every
 * selection disables all three muxes first), reading raw X/Y/Z into a
 * per-pad array.
 *
 * V1 scope: raw XYZ only. No calibration, no axis selection (deciding
 * which raw axis is actually vertical press depth per pad), no
 * filtering -- see docs/architecture/defaults-and-safeguards.md "V1
 * sensing scope". Those are the next layer once this raw scan is
 * verified against real hardware.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    bool valid; /* false if the last read for this pad failed, or it was never successfully initialized */
} tiles_hall_sample_t;

/* Disables all Hall mux channels, then configures all 24 physical
 * TMAG5273 sensors one at a time (select channel -> identify -> init ->
 * deselect all). Must run after board_i2c_init(). Returns false if any
 * sensor failed identification or configuration; check
 * tiles_hall_last_init_ok() per pad to see which ones. A pad that
 * failed init is skipped by tiles_hall_scan() rather than blocking the
 * rest -- matches the "a failed subsystem disables itself" principle. */
bool tiles_hall_init(void);

/* True if pad (1-24)'s sensor was successfully identified and
 * configured during tiles_hall_init(). */
bool tiles_hall_last_init_ok(uint8_t logical_pad);

/* Services one pad per call, round-robining through every
 * successfully-initialized pad. Call this every main-loop iteration for
 * a running scan -- a full 24-pad sweep is many I2C transactions across
 * many calls, not one blocking call. */
void tiles_hall_scan(void);

/* Latest sample for one pad (1-24). Returns a zeroed, invalid sample if
 * the pad number is out of range. */
tiles_hall_sample_t tiles_hall_get_sample(uint8_t logical_pad);
