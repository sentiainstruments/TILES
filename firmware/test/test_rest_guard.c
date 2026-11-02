/*
 * services/rest_guard.c -- a pad's rest position stays true: drift creeps,
 * a key that reads pressed with no finger re-zeroes, and a baseline taken
 * under a finger keeps the pad silent until it's seen at rest. Run with
 * test/run.sh.
 */

#include <assert.h>
#include <stdio.h>

#include "rest_guard.h"

/* Untouched reads of `z` every 50 ms (the background round-robin's pace)
 * from `from_ms` for `for_ms`; true if any jumped the baseline. */
static bool hold(tiles_rest_guard_t *g, int16_t z, uint32_t from_ms, uint32_t for_ms) {
    bool jumped = false;
    for (uint32_t t = from_ms; t <= from_ms + for_ms; t += 50u) {
        jumped |= tiles_rest_guard_untouched(g, z, t);
    }
    return jumped;
}

int main(void) {
    tiles_rest_guard_t g = {0};

    /* 1. At rest: no depth, no jump */
    tiles_rest_guard_set(&g, 1000, true);
    assert(!hold(&g, 1002, 0u, 2000u));
    assert(tiles_rest_guard_depth(&g, 1002) <= 2u);

    /* 2. Slow drift creeps the baseline, never jumps */
    tiles_rest_guard_set(&g, 1000, true);
    assert(!hold(&g, 1060, 0u, 3000u));
    assert(g.baseline > 1000 && g.baseline < 1060);
    assert(g.rezeros == 0u);

    /* 3. The reported case: the baseline was taken with the key pressed.
     * Released and untouched, the key reads a strike deep from it; after
     * TILES_REST_REZERO_MS steady it re-zeroes to the real rest. */
    tiles_rest_guard_set(&g, 1900, true); /* pressed */
    assert(tiles_rest_guard_depth(&g, 1000) >= 150u);
    assert(!hold(&g, 1000, 0u, 500u));    /* not yet */
    assert(hold(&g, 1000, 550u, 200u));   /* re-zeroed */
    assert(g.baseline == 1000 && g.rezeros == 1u);
    assert(tiles_rest_guard_depth(&g, 1000) == 0u);
    assert(tiles_rest_guard_depth(&g, 1900) >= 150u); /* a press reads as one again */

    /* 4. A key still moving (springing back) never re-zeroes mid-motion */
    tiles_rest_guard_set(&g, 1900, true);
    for (uint32_t t = 0; t < 2000u; t += 50u) {
        assert(!tiles_rest_guard_untouched(&g, (int16_t)(1000 + (t / 50u % 2u) * 40), t)); /* 40-count wobble */
    }
    assert(g.baseline == 1900);

    /* 5. A touch ends the streak: touched time never counts as steady */
    tiles_rest_guard_set(&g, 1900, true);
    hold(&g, 1000, 0u, 400u);
    tiles_rest_guard_touched(&g);
    assert(!hold(&g, 1000, 450u, 500u)); /* the clock restarted after the touch */
    assert(hold(&g, 1000, 1000u, 200u));

    /* 6. Unconfirmed (touched when the baseline was taken): silent until a
     * steady untouched dwell sets it, then plays from the real rest */
    tiles_rest_guard_set(&g, 1900, false);
    assert(tiles_rest_guard_depth(&g, 1000) == 0u && tiles_rest_guard_depth(&g, 2500) == 0u);
    assert(!hold(&g, 1000, 0u, 350u));
    assert(hold(&g, 1000, 400u, 100u));
    assert(g.confirmed && g.baseline == 1000);
    assert(tiles_rest_guard_depth(&g, 1900) >= 150u);

    /* 7. Under the re-zero depth (a light finger the touch sensor missed
     * reads under ~100): drift only, no jump */
    uint32_t before = g.rezeros; /* the count runs on across baselines */
    tiles_rest_guard_set(&g, 1000, true);
    assert(!hold(&g, 1100, 0u, 1500u));
    assert(g.rezeros == before && g.baseline < 1100);

    printf("rest_guard: all tests pass\n");
    return 0;
}
