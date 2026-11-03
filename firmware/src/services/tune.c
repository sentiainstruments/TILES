#include "tune.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tiles_tune_t s_tune;
static tiles_tune_t s_default;
static bool s_ready;

typedef enum { TUNE_FLOAT, TUNE_UINT } tune_type_t;

typedef struct {
    const char *key;
    const char *group;
    tune_type_t type;
    size_t offset;
    float min;
    float max;
} tune_entry_t;

#define F(field) TUNE_FLOAT, offsetof(tiles_tune_t, field)
#define U(field) TUNE_UINT, offsetof(tiles_tune_t, field)

/* Ranges refuse nonsense, not taste. */
static const tune_entry_t TABLE[] = {
    {"strike.depth", "velocity", F(strike_depth), 60.0f, 600.0f},
    {"strike.overshoot_full", "velocity", F(overshoot_full), 50.0f, 3000.0f},
    {"strike.followthrough_ms", "velocity", U(followthrough_ms), 0.0f, 100.0f},
    {"strike.settle_rise", "velocity", F(settle_rise), 1.0f, 100.0f},
    {"velocity.time_fast_ms", "velocity", U(time_fast_ms), 0.0f, 100.0f},
    {"velocity.time_slow_ms", "velocity", U(time_slow_ms), 20.0f, 2000.0f},
    {"velocity.depth_weight", "velocity", F(depth_weight), 0.0f, 1.0f},
    {"velocity.curve", "velocity", F(velocity_curve), 0.2f, 5.0f},
    {"velocity.min", "velocity", U(min_velocity), 1.0f, 100.0f},
    {"pressure.smoothing", "pressure", F(pressure_smoothing), 0.01f, 1.0f},
    {"release.smoothing", "pressure", F(release_smoothing), 0.01f, 1.0f},
    {"release.rate_min", "pressure", F(release_rate_min), 0.0f, 50.0f},
    {"release.rate_max", "pressure", F(release_rate_max), 0.5f, 200.0f},
    {"key.up_depth", "key", U(key_up_depth), 10.0f, 500.0f},
    {"key.retrigger_depth", "key", F(retrigger_depth), 0.0f, 500.0f},
    {"key.retrigger_grace_ms", "key", U(retrigger_grace_ms), 0.0f, 1000.0f},
    {"key.touch_dropout_ms", "key", U(touch_dropout_ms), 0.0f, 200.0f},
    {"key.pressed_dropout_ms", "key", U(pressed_dropout_ms), 0.0f, 2000.0f},
    {"key.strike_from_rest_ms", "key", U(strike_from_rest_ms), 0.0f, 2000.0f},
    {"bend.deadzone", "bend", F(bend.deadzone), 0.0f, 0.2f},
    {"bend.settle_ms", "bend", U(bend.settle_ms), 0.0f, 1000.0f},
    {"bend.smoothing", "bend", F(bend.smoothing), 0.005f, 1.0f},
    {"bend.recenter_slow", "bend", F(bend.recenter_slow), 0.0f, 0.1f},
    {"bend.recenter_fast", "bend", F(bend.recenter_fast), 0.0f, 1.0f},
    {"bend.depth_activity_full", "bend", F(bend.depth_activity_full), 0.5f, 100.0f},
    {"bend.arm_ms", "bend", U(bend.arm_ms), 0.0f, 500.0f},
    {"vibrato.floor", "vibrato", F(bend.vibrato_floor), 0.0f, 200.0f},
    {"vibrato.full", "vibrato", F(bend.vibrato_full), 0.5f, 400.0f},
    {"vibrato.arm_ms", "vibrato", U(bend.vibrato_arm_ms), 0.0f, 1000.0f},
    {"vibrato.rate_hz", "vibrato", F(bend.vibrato_rate_hz), 1.0f, 15.0f},
    {"vibrato.depth", "vibrato", F(bend.vibrato_depth), 0.0f, 2000.0f},
    {"path.min_z_range", "path", F(bend.path_min_z_range), 10.0f, 2000.0f},
    {"path.max_residual", "path", F(bend.path_max_residual), 5.0f, 2000.0f},
    {"path.max_slope", "path", F(bend.path_max_slope), 0.05f, 5.0f},
    {"path.min_strokes", "path", U(bend.path_min_strokes), 1.0f, 100.0f},
    {"path.average_strokes", "path", U(bend.path_average_strokes), 1.0f, 500.0f},
};
#define TABLE_COUNT (sizeof(TABLE) / sizeof(TABLE[0]))

static void ensure_ready(void) {
    if (s_ready) {
        return;
    }
    s_default = (tiles_tune_t){
        .strike_depth = TILES_TUNE_DEFAULT_STRIKE_DEPTH,
        .overshoot_full = TILES_TUNE_DEFAULT_OVERSHOOT_FULL,
        .followthrough_ms = TILES_TUNE_DEFAULT_FOLLOWTHROUGH_MS,
        .settle_rise = TILES_TUNE_DEFAULT_SETTLE_RISE,
        .time_fast_ms = TILES_TUNE_DEFAULT_TIME_FAST_MS,
        .time_slow_ms = TILES_TUNE_DEFAULT_TIME_SLOW_MS,
        .depth_weight = TILES_TUNE_DEFAULT_DEPTH_WEIGHT,
        .velocity_curve = TILES_TUNE_DEFAULT_VELOCITY_CURVE,
        .min_velocity = TILES_TUNE_DEFAULT_MIN_VELOCITY,
        .pressure_smoothing = TILES_TUNE_DEFAULT_PRESSURE_SMOOTHING,
        .release_smoothing = TILES_TUNE_DEFAULT_RELEASE_SMOOTHING,
        .release_rate_min = TILES_TUNE_DEFAULT_RELEASE_RATE_MIN,
        .release_rate_max = TILES_TUNE_DEFAULT_RELEASE_RATE_MAX,
        .key_up_depth = TILES_TUNE_DEFAULT_KEY_UP_DEPTH,
        .retrigger_depth = TILES_TUNE_DEFAULT_RETRIGGER_DEPTH,
        .retrigger_grace_ms = TILES_TUNE_DEFAULT_RETRIGGER_GRACE_MS,
        .touch_dropout_ms = TILES_TUNE_DEFAULT_TOUCH_DROPOUT_MS,
        .pressed_dropout_ms = TILES_TUNE_DEFAULT_PRESSED_DROPOUT_MS,
        .strike_from_rest_ms = TILES_TUNE_DEFAULT_STRIKE_FROM_REST_MS,
        .bend = TILES_BEND_TUNING_DEFAULT,
    };
    s_tune = s_default;
    s_ready = true;
}

const tiles_tune_t *tiles_tune(void) {
    ensure_ready();
    return &s_tune;
}

static float get(const tiles_tune_t *t, const tune_entry_t *e) {
    const void *p = (const char *)t + e->offset;
    return e->type == TUNE_FLOAT ? *(const float *)p : (float)*(const uint32_t *)p;
}

static void put(tiles_tune_t *t, const tune_entry_t *e, float v) {
    void *p = (char *)t + e->offset;
    if (e->type == TUNE_FLOAT) {
        *(float *)p = v;
    } else {
        *(uint32_t *)p = (uint32_t)lroundf(v);
    }
}

/* Orders the code relies on (a division, or a threshold above another). */
static bool consistent(const tiles_tune_t *t) {
    return (float)t->key_up_depth < t->strike_depth && t->retrigger_depth < t->strike_depth &&
           t->time_fast_ms < t->time_slow_ms && t->release_rate_min < t->release_rate_max &&
           t->bend.recenter_slow <= t->bend.recenter_fast && t->bend.vibrato_floor < t->bend.vibrato_full;
}

static const tune_entry_t *find(const char *key) {
    for (size_t i = 0; i < TABLE_COUNT; i++) {
        if (strcmp(TABLE[i].key, key) == 0) {
            return &TABLE[i];
        }
    }
    return NULL;
}

uint16_t tiles_tune_count(void) {
    return (uint16_t)TABLE_COUNT;
}

bool tiles_tune_describe(uint16_t index, char *out, size_t cap) {
    if (index >= TABLE_COUNT) {
        return false;
    }
    ensure_ready();
    const tune_entry_t *e = &TABLE[index];
    snprintf(out, cap, "key=%s value=%.6g default=%.6g min=%.6g max=%.6g group=%s", e->key, (double)get(&s_tune, e),
             (double)get(&s_default, e), (double)e->min, (double)e->max, e->group);
    return true;
}

tiles_tune_result_t tiles_tune_set(const char *key, const char *value) {
    ensure_ready();
    const tune_entry_t *e = key != NULL ? find(key) : NULL;
    if (e == NULL) {
        return TILES_TUNE_UNKNOWN_KEY;
    }
    char *end = NULL;
    float v = value != NULL ? strtof(value, &end) : 0.0f;
    if (value == NULL || end == value || *end != '\0' || !isfinite(v)) {
        return TILES_TUNE_BAD_VALUE;
    }
    if (v < e->min || v > e->max) {
        return TILES_TUNE_OUT_OF_RANGE;
    }
    tiles_tune_t next = s_tune;
    put(&next, e, v);
    if (!consistent(&next)) {
        return TILES_TUNE_CONFLICT;
    }
    s_tune = next;
    return TILES_TUNE_OK;
}

bool tiles_tune_reset(const char *key) {
    ensure_ready();
    if (key == NULL) {
        s_tune = s_default;
        return true;
    }
    const tune_entry_t *e = find(key);
    if (e == NULL) {
        return false;
    }
    tiles_tune_t next = s_tune;
    put(&next, e, get(&s_default, e));
    if (consistent(&next)) {
        s_tune = next;
    } else {
        s_tune = s_default; /* its partner was moved past it: back to a known-good table */
    }
    return true;
}

uint16_t tiles_tune_changed(void) {
    ensure_ready();
    uint16_t n = 0u;
    for (size_t i = 0; i < TABLE_COUNT; i++) {
        if (get(&s_tune, &TABLE[i]) != get(&s_default, &TABLE[i])) {
            n++;
        }
    }
    return n;
}
