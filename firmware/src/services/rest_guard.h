#pragma once

/* Keeps a pad's Hall rest baseline true, from untouched reads (pure; tested
 * in test_rest_guard.c). A steady streak near the baseline creeps it toward
 * the reading (drift); a steady streak a strike's depth away re-zeroes it
 * (an untouched key can't be held down); a baseline taken while touched
 * gives no depth until confirmed at rest. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_REST_NOISE 8              /* raw Z counts between steady reads */
#define TILES_REST_DWELL_MS 400u
#define TILES_REST_REZERO_MS 600u       /* first guess */
#define TILES_REST_REZERO_DEPTH 150     /* = the strike threshold (expression.h) */
#define TILES_REST_DRIFT_SLEW 128

typedef struct {
    int16_t baseline;
    bool confirmed;
    bool streak;            /* a previous untouched read to compare with */
    int16_t last_z;
    uint32_t steady_since_ms;
    uint32_t rezeros;       /* re-zero jumps so far */
} tiles_rest_guard_t;

/* A new baseline; `confirmed` false when the pad was touched as it was
 * taken. Restarts the streak. */
void tiles_rest_guard_set(tiles_rest_guard_t *g, int16_t baseline, bool confirmed);

/* The pad is touched: the streak ends. */
void tiles_rest_guard_touched(tiles_rest_guard_t *g);

/* An untouched read: drift, re-zero or confirm. True if the baseline
 * jumped (a re-zero or a confirmation). */
bool tiles_rest_guard_untouched(tiles_rest_guard_t *g, int16_t z, uint32_t now_ms);

/* |z - baseline|, or 0 while unconfirmed. */
uint16_t tiles_rest_guard_depth(const tiles_rest_guard_t *g, int16_t z);
