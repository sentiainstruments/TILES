#pragma once

/* Pitch bend from key tilt, no hardware: expression.c feeds it one Hall
 * sample per pass while a note is held. Tested in test/test_bend_core.c;
 * captures replay through it (test/bend_replay.c). */

#include <stdbool.h>
#include <stdint.h>

#define TILES_BEND_CENTER 8192u

/* The MPE spec's default bend range: what a receiver uses if it ignores
 * RPN 0. */
#define TILES_BEND_RECEIVER_DEFAULT_RANGE_SEMITONES 48.0f

typedef struct {
    float max_cosine_deviation; /* full bend; smaller = more sensitive */
    float wire_range_semitones; /* the range declared with RPN 0 */
} tiles_bend_config_t;

typedef struct {
    uint16_t center;    /* 8192, or above it for a quarter tone */
    uint32_t start_ms;  /* note-on; the baseline waits TILES_BEND_SETTLE_MS */
    bool settled;
    float x1, y1;       /* first smoothing stage */
    float x2, y2;       /* second stage: the live delta and recentring read it */
    float magnitude;    /* one stage of |B| */
    float run_magnitude; /* |B| for both cosine terms, frozen during a run */
    float baseline_x, baseline_y;
    float prev_depth;
    float depth_rate;   /* EMA of the signed depth delta */
    float delta;        /* this pass's compensated cosine delta */
    bool run_active;    /* the delta is outside the deadzone */
    bool run_positive;
    uint32_t run_start_ms;
    float wiggle_energy; /* gap between the two stages, for vibrato */
    bool wiggle_active;
    uint32_t wiggle_start_ms;
} tiles_bend_t;

/* At note-on: seeds every stage from this sample, so nothing ramps from 0.
 * `depth` is a fresh depth read. */
void tiles_bend_start(tiles_bend_t *b, int16_t x, int16_t y, int16_t z, float depth, uint16_t center,
                      uint32_t now_ms);

/* One valid sample and the pad's smoothed depth. False until the baseline
 * has settled; then true with the 14-bit bend in *out_bend. */
bool tiles_bend_step(tiles_bend_t *b, const tiles_bend_config_t *cfg, int16_t x, int16_t y, int16_t z,
                     float smoothed_depth, uint32_t now_ms, uint16_t *out_bend);
