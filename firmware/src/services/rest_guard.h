#pragma once

/* A pad's rest position (its Hall baseline), kept true. Keys spring back,
 * so an untouched key is at rest: services/hall.c feeds every untouched
 * read here, and depth is measured from the result. Pure, no hardware;
 * host-tested in test/test_rest_guard.c.
 * (docs/architecture/defaults-and-safeguards.md "Pad baseline
 * calibration and drift compensation".)
 *
 * A read counts as steady when it is within TILES_REST_NOISE of the one
 * before; a steady streak ends at any bigger step or any touched read.
 *   - Drift: steady TILES_REST_DWELL_MS near the baseline, the baseline
 *     creeps toward the reading (1/128 of the gap a read), following
 *     thermal and mechanical drift. Never a jump.
 *   - Re-zero: steady TILES_REST_REZERO_MS at least TILES_REST_REZERO_DEPTH
 *     (a strike's depth) from the baseline: an untouched key can't be
 *     held down, so the baseline was taken pressed (a finger on the key
 *     at start-up, a knock, an object since lifted). It jumps to the
 *     reading.
 *   - Unconfirmed: a baseline taken while the pad was touched isn't
 *     trusted. The pad reports no depth (it can't play or press anything)
 *     until a steady TILES_REST_DWELL_MS untouched sets it.
 * The first two rules repair what slips past the third: a finger the
 * touch sensor missed at start-up reads as untouched. */

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
