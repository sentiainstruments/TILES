#pragma once

/* When a strike can send its note: once the peak fills the depth score, or
 * the key stops going down (TILES_STRIKE_SETTLE_SAMPLES fresh samples
 * without a rise), or the follow-through ends. Noise can only delay a note.
 * Tested in test_strike_settle.c. */

#include <stdbool.h>
#include <stdint.h>

#include "tune.h"

/* The longest wait after the crossing, and the rise that counts as still
 * going down (live: tune.h). */
#define TILES_STRIKE_FOLLOWTHROUGH_MS (tiles_tune()->followthrough_ms)
#define TILES_STRIKE_SETTLE_RISE (tiles_tune()->settle_rise)
/* "Stopped going down" also needs this long and this many fresh samples;
 * a touched pad's Hall is read every pass (~2.5 ms). First guesses. */
#define TILES_STRIKE_SETTLE_MS 4u
#define TILES_STRIKE_SETTLE_SAMPLES 2u

typedef struct {
    uint32_t crossed_ms;
    float rise_depth;  /* depth at the last rise */
    uint32_t rise_ms;  /* when it was read */
    uint8_t samples_since_rise;
} tiles_strike_settle_t;

/* The strike just crossed: `depth` read at `now_ms`. */
void tiles_strike_settle_begin(tiles_strike_settle_t *st, float depth, uint32_t now_ms);

/* One fresh Hall sample after the crossing. */
void tiles_strike_settle_sample(tiles_strike_settle_t *st, float depth, uint32_t sample_ms);

/* True once the note can go (see above). */
bool tiles_strike_settle_ready(const tiles_strike_settle_t *st, float peak_depth, float full_depth,
                               uint32_t now_ms);
