#pragma once

/* Hall scanning: the 24 TMAG5273s through the muxes into per-pad X/Y/Z.
 * Touched pads read every call, the rest round-robin (~50 ms each, too slow
 * for a strike); a released pad keeps being read until its key is up, so
 * expression.c can tell a lift from a dropout. Depth is Z from the rest
 * baseline, kept true by rest_guard.h. Failed sensors are retried while no
 * pad is touched. */

#include <stdbool.h>
#include <stdint.h>

#include "tune.h"

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
    uint32_t sample_time_ms; /* to_ms_since_boot() when read */
    bool valid;              /* false if the last read failed or the pad never initialized */
} tiles_hall_sample_t;

/* Sets up every sensor and captures rest baselines; after board_i2c_init().
 * False if any failed (it's skipped, not blocking). A pad touched at
 * power-on is re-zeroed by the rest guard once let go. */
bool tiles_hall_init(void);

/* True if pad (1-24)'s sensor was identified and configured at init. */
bool tiles_hall_last_init_ok(uint8_t logical_pad);

/* Reads every touched pad (in mux order), then advances the background
 * round-robin by one untouched pad and feeds that read to its rest guard
 * (so the baseline only ever moves on untouched pads). Call every
 * main-loop pass. */
void tiles_hall_scan(void);

/* Latest raw sample for pad 1-24. Zeroed and invalid if out of range. */
tiles_hall_sample_t tiles_hall_get_sample(uint8_t logical_pad);

/* Recaptures rest baselines (end of the boot animation, calibration). A pad
 * touched meanwhile keeps its old one, unconfirmed. False if a read
 * failed. */
bool tiles_hall_recapture_baseline(void);

/* |Z - rest Z| for one pad: an uncalibrated distance from rest, >= 0
 * whatever the sensor's polarity, times the pad's depth gain (1 unless
 * key.depth_gain is set, tune.h). 0 if out of range, uninitialized, not
 * yet sampled, or its rest unconfirmed. */
uint16_t tiles_hall_get_depth(uint8_t logical_pad);

/* That gain, for pad 1-24 (1 if out of range). */
float tiles_hall_get_depth_gain(uint8_t logical_pad);

/* Pad 1-24's rest baseline (raw Z) and whether it was confirmed at rest.
 * False if out of range or the sensor never initialized. */
bool tiles_hall_get_rest(uint8_t logical_pad, int16_t *baseline_z, bool *confirmed);

/* For INFO: re-zeros so far (all pads), pads whose rest is unconfirmed
 * now, sensors set up again since boot (one that reset or came back), pads
 * whose sensor isn't answering now, and the time init and the boot
 * recapture took. */
typedef struct {
    uint32_t rezeros;
    uint8_t unconfirmed;
    uint32_t reinits;
    uint8_t dead;
    uint32_t boot_ms;
} tiles_hall_rest_info_t;
tiles_hall_rest_info_t tiles_hall_get_rest_info(void);

/* Depth below which a key counts as up (live: tune.h). */
#define TILES_HALL_KEY_UP_DEPTH (tiles_tune()->key_up_depth)
#define TILES_HALL_RELEASE_FOLLOW_MS 300u
