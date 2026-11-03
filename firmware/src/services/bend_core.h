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

/* How tilt is told apart from depth (expression.pitch_bend_mode); latched
 * per note. Measured against each other: services/HISTORY.md. */
typedef enum {
    TILES_BEND_MODE_LEGACY = 0, /* raw X/Y vs a baseline chasing depth, |B| frozen in a run */
    TILES_BEND_MODE_COSINE,     /* each sample's direction cosine, filtered after dividing */
    TILES_BEND_MODE_PATH,       /* the pad's press path (x = a + b*z) subtracted first */
    TILES_BEND_MODE_COUNT,
} tiles_bend_mode_t;

/* "legacy", "cosine", "path": the setting's values. */
extern const char *const TILES_BEND_MODE_NAMES[TILES_BEND_MODE_COUNT];

typedef struct {
    float max_cosine_deviation; /* full bend; smaller = more sensitive */
    float wire_range_semitones; /* the range declared with RPN 0 */
} tiles_bend_config_t;

/* One pad's press path: how X and Y move with Z on a straight press,
 * learned from strike down-strokes and kept across notes (RAM only). */
typedef struct {
    float bx, by;     /* dX/dZ, dY/dZ */
    uint16_t strokes; /* strokes learned */
    /* the stroke being recorded, relative to its first sample */
    uint16_t n;
    float x0, y0, z0;
    float sx, sy, sz, sxx, syy, szz, szx, szy;
    float zmin, zmax;
} tiles_press_path_t;

typedef struct {
    uint16_t center;    /* 8192, or above it for a quarter tone */
    uint32_t start_ms;  /* note-on; the baseline waits BEND_SETTLE_MS */
    bool settled;
    uint8_t mode;       /* tiles_bend_mode_t */
    float x1, y1;       /* first smoothing stage */
    float x2, y2;       /* second stage: the live delta and recentring read it */
    float z1, z2;       /* PATH: smoothed raw Z, for the anchor */
    float magnitude;    /* one stage of |B| */
    float run_magnitude; /* LEGACY: |B| for both cosine terms, frozen during a run */
    float baseline_x, baseline_y;
    float path_bx, path_by;   /* PATH: this note's slope */
    bool path_learned;        /* PATH: false = the note's own direction at settle */
    float anchor_x, anchor_y; /* PATH: a, set at settle */
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
 * `depth` is a fresh depth read; `path` is the pad's (PATH only, may be
 * NULL). */
void tiles_bend_start(tiles_bend_t *b, tiles_bend_mode_t mode, const tiles_press_path_t *path, int16_t x,
                      int16_t y, int16_t z, float depth, uint16_t center, uint32_t now_ms);

/* One valid sample and the pad's smoothed depth. False until the baseline
 * has settled; then true with the 14-bit bend in *out_bend. */
bool tiles_bend_step(tiles_bend_t *b, const tiles_bend_config_t *cfg, int16_t x, int16_t y, int16_t z,
                     float smoothed_depth, uint32_t now_ms, uint16_t *out_bend);

/* Press path learning: begin at touch, a sample per fresh read until the
 * note fires, then end, which learns the stroke if it looks like a straight
 * press. True if it learned. */
void tiles_press_path_reset(tiles_press_path_t *p);
void tiles_press_path_stroke_begin(tiles_press_path_t *p);
void tiles_press_path_stroke_sample(tiles_press_path_t *p, int16_t x, int16_t y, int16_t z);
bool tiles_press_path_stroke_end(tiles_press_path_t *p);
