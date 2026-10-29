#pragma once

/* When a strike past the strike depth can send its note (expression.c's
 * "Velocity"; Prism's chord pads too). The velocity reads how deep the hit
 * goes during a follow-through after the crossing, but waiting out the
 * whole window put every note that much behind the finger, which shows
 * when recording. So the note goes as soon as its velocity is known:
 *   - the peak is at or past `full_depth` (the depth score is full):
 *     nothing left to measure;
 *   - the key has stopped going down: TILES_STRIKE_SETTLE_SAMPLES fresh
 *     Hall samples, over at least TILES_STRIKE_SETTLE_MS, with no rise of
 *     TILES_STRIKE_SETTLE_RISE since the last one;
 *   - or the follow-through (TILES_STRIKE_FOLLOWTHROUGH_MS) is over: a slow
 *     press still going down.
 * A noisy reading can only delay a note (it looks like a rise), never send
 * it early. Host-tested in test/test_strike_settle.c. */

#include <stdbool.h>
#include <stdint.h>

/* The longest wait after the crossing. Long enough for a light touch to
 * plateau, short of perceptible latency (~10-20 ms). First guess. */
#define TILES_STRIKE_FOLLOWTHROUGH_MS 20u
/* "Stopped going down": depth units (full scale ~1450), ms and samples.
 * A touched pad's Hall is read every pass (~2.5 ms). First guesses. */
#define TILES_STRIKE_SETTLE_RISE 8.0f
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
