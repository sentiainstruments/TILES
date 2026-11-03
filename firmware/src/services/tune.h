#pragma once

/* The feel constants as one live table: what strike, velocity, pressure,
 * key and bend code reads every pass, in every mode. TUNE over USB changes
 * them until power-off (the studio, tools/studio/); values that work become
 * the defaults here and in bend_core.c. No hardware: host-testable. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bend_core.h"

/* ---- Strike and velocity (every mode's velocity: expression.h) ----
 * From ~140 captured touches (16-count steps): contact and settling read
 * 32-96, deliberate presses 192-736, a full press ~900-1450. */
/* Depth travel that makes a press (and starts velocity). 150 sits in the
 * captured gap; 300 made light playing impossible. */
#define TILES_TUNE_DEFAULT_STRIKE_DEPTH 150.0f
/* Overshoot past it, by the end of the follow-through, at which the depth
 * score maxes out. Unmeasured. */
#define TILES_TUNE_DEFAULT_OVERSHOOT_FULL 550.0f
/* The longest wait after the crossing (strike_settle.h): long enough for a
 * light touch to plateau, short of perceptible latency. First guess. */
#define TILES_TUNE_DEFAULT_FOLLOWTHROUGH_MS 20u
/* Depth units counted as "still going down". First guess. */
#define TILES_TUNE_DEFAULT_SETTLE_RISE 8.0f
/* Velocity = depth score x weight + time score (touch to threshold) x the
 * rest, floored. Not acceleration: 16-count steps made it noise. Guesses. */
#define TILES_TUNE_DEFAULT_TIME_FAST_MS 10u
#define TILES_TUNE_DEFAULT_TIME_SLOW_MS 300u
#define TILES_TUNE_DEFAULT_DEPTH_WEIGHT 0.85f
#define TILES_TUNE_DEFAULT_VELOCITY_CURVE 1.0f
#define TILES_TUNE_DEFAULT_MIN_VELOCITY 8u

/* ---- Pressure and release ---- */
/* EMA on the depth that feeds pressure: continuous, not sensor noise,
 * without noticeable lag. Not applied to velocity. First guess. */
#define TILES_TUNE_DEFAULT_PRESSURE_SMOOTHING 0.35f
/* Release velocity from the smoothed fall rate at lift-off (depth
 * units/ms): at or below min the floor, at or above max 127. Guesses. */
#define TILES_TUNE_DEFAULT_RELEASE_SMOOTHING 0.5f
#define TILES_TUNE_DEFAULT_RELEASE_RATE_MIN 2.0f
#define TILES_TUNE_DEFAULT_RELEASE_RATE_MAX 25.0f

/* ---- Key and touch ---- */
/* Below this depth a key counts as up: above bare contact (a resting
 * finger reads up to ~96), under the strike depth. */
#define TILES_TUNE_DEFAULT_KEY_UP_DEPTH 100u
/* A held key easing back this near rest re-arms a strike (retrigger
 * without lifting), not within the grace after note-on (a fast hit's
 * rebound). Unmeasured. */
#define TILES_TUNE_DEFAULT_RETRIGGER_DEPTH 40.0f
#define TILES_TUNE_DEFAULT_RETRIGGER_GRACE_MS 50u
/* Touch counts as down this long after the last touched reading: a hard
 * hit can bounce the finger off. Unmeasured. */
#define TILES_TUNE_DEFAULT_TOUCH_DROPOUT_MS 12u
/* A resting finger's touch can drop out longer: a note whose key still
 * reads pressed outlives a dropout this long, and a touch starting on a
 * pressed key strikes only if the key was up within strike_from_rest_ms.
 * Guesses. */
#define TILES_TUNE_DEFAULT_PRESSED_DROPOUT_MS 300u
#define TILES_TUNE_DEFAULT_STRIKE_FROM_REST_MS 150u

typedef struct {
    float strike_depth;
    float overshoot_full;
    uint32_t followthrough_ms;
    float settle_rise;
    uint32_t time_fast_ms;
    uint32_t time_slow_ms;
    float depth_weight;
    float velocity_curve;
    uint32_t min_velocity;
    float pressure_smoothing;
    float release_smoothing;
    float release_rate_min;
    float release_rate_max;
    uint32_t key_up_depth;
    float retrigger_depth;
    uint32_t retrigger_grace_ms;
    uint32_t touch_dropout_ms;
    uint32_t pressed_dropout_ms;
    uint32_t strike_from_rest_ms;
    tiles_bend_tuning_t bend;
} tiles_tune_t;

/* The values in effect. */
const tiles_tune_t *tiles_tune(void);

typedef enum {
    TILES_TUNE_OK = 0,
    TILES_TUNE_UNKNOWN_KEY,
    TILES_TUNE_BAD_VALUE,
    TILES_TUNE_OUT_OF_RANGE,
    TILES_TUNE_CONFLICT, /* breaks an order, e.g. key-up depth >= strike depth */
} tiles_tune_result_t;

/* TUNE over USB: keys like "strike.depth" and "bend.deadzone". */
uint16_t tiles_tune_count(void);
/* "key=strike.depth value=150 default=150 min=60 max=600 group=velocity" */
bool tiles_tune_describe(uint16_t index, char *out, size_t cap);
tiles_tune_result_t tiles_tune_set(const char *key, const char *value);
/* NULL = every key. False for an unknown key. */
bool tiles_tune_reset(const char *key);
/* Keys away from their default. */
uint16_t tiles_tune_changed(void);
