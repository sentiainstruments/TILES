#pragma once

/* Hall sensor scanning: reads the 24 TMAG5273s through their TCA9548A
 * channels (one channel open at a time across the three muxes: a read
 * switches from the last one) into a per-pad X/Y/Z array; a key at rest
 * reads Z only. Sensors that fail, or reset and stop measuring, are set
 * up again in the background while no pad is touched.
 *
 * Touched pads (services/touch.h) are read every call; untouched pads
 * round-robin one per call. Round-robin alone reaches a pad only every ~24
 * calls (~50 ms), far too slow to see a 30-80 ms strike, which
 * services/expression.c needs for velocity. A pad just released is also
 * read every call until it reads below TILES_HALL_KEY_UP_DEPTH (the key
 * came back up) or TILES_HALL_RELEASE_FOLLOW_MS passes: otherwise its depth
 * stays at the last pressed reading after the finger leaves, and
 * expression.c can't tell a lift from a touch dropout over a held key.
 *
 * Provides raw XYZ, a per-pad rest baseline (captured at init and at the
 * end of the boot animation, then kept true by services/rest_guard.h:
 * drift, a re-zero when an untouched key reads pressed, and silence for a
 * pad touched as its baseline was taken; docs/architecture/
 * defaults-and-safeguards.md "Pad baseline calibration and drift
 * compensation"), and a depth magnitude from it. Depth uses Z
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
 * The baseline assumes nothing rests on the pads at power-on; if it did,
 * the rest guard re-zeroes the pad once it's let go (rest_guard.h). */
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

/* Recaptures every initialized pad's rest-Z baseline from a fresh read,
 * like init does (same "at rest now" assumption), at the end of the boot
 * animation and from the serial calibration flow (diagnostics/
 * calibration.h). A pad touched meanwhile keeps its old baseline, marked
 * unconfirmed: it reports no depth until seen at rest (rest_guard.h).
 * Uninitialized pads are skipped. False if any read failed; that pad
 * keeps its old baseline. */
bool tiles_hall_recapture_baseline(void);

/* |Z - rest Z| for one pad: an uncalibrated distance from rest, >= 0
 * whatever the sensor's polarity. 0 if out of range, uninitialized, not
 * yet sampled, or its rest unconfirmed. */
uint16_t tiles_hall_get_depth(uint8_t logical_pad);

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

/* Depth below which a key counts as up: above bare contact (a resting
 * finger reads up to ~96), under the strike threshold (150, expression.h)
 * with room for hysteresis. */
#define TILES_HALL_KEY_UP_DEPTH 100u
#define TILES_HALL_RELEASE_FOLLOW_MS 300u
