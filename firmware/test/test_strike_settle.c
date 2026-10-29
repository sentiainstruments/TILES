/*
 * services/strike_settle.c -- when a strike's note can go: full depth at
 * once, a settled key early, a slow press at the end of the follow-through,
 * and noise only ever delaying it. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>

#include "strike_settle.h"

#define FULL 700.0f

int main(void) {
    tiles_strike_settle_t st;

    /* 1. a hit already past full depth goes at the crossing */
    tiles_strike_settle_begin(&st, 720.0f, 1000u);
    assert(tiles_strike_settle_ready(&st, 720.0f, FULL, 1000u));

    /* 2. a light touch that plateaus goes after two flat samples and 4 ms */
    tiles_strike_settle_begin(&st, 160.0f, 1000u);
    assert(!tiles_strike_settle_ready(&st, 160.0f, FULL, 1000u));
    tiles_strike_settle_sample(&st, 163.0f, 1002u);
    assert(!tiles_strike_settle_ready(&st, 163.0f, FULL, 1002u)); /* one sample, 2 ms */
    tiles_strike_settle_sample(&st, 165.0f, 1004u);
    assert(tiles_strike_settle_ready(&st, 165.0f, FULL, 1004u)); /* two samples, 4 ms */

    /* 3. two quick samples inside 4 ms aren't enough on their own */
    tiles_strike_settle_begin(&st, 160.0f, 1000u);
    tiles_strike_settle_sample(&st, 160.0f, 1001u);
    tiles_strike_settle_sample(&st, 160.0f, 1002u);
    assert(!tiles_strike_settle_ready(&st, 160.0f, FULL, 1002u));
    assert(tiles_strike_settle_ready(&st, 160.0f, FULL, 1004u));

    /* 4. no fresh samples (a slow bus): only the follow-through sends it */
    tiles_strike_settle_begin(&st, 200.0f, 1000u);
    assert(!tiles_strike_settle_ready(&st, 200.0f, FULL, 1010u));
    assert(!tiles_strike_settle_ready(&st, 200.0f, FULL, 1019u));
    assert(tiles_strike_settle_ready(&st, 200.0f, FULL, 1020u));

    /* 5. a press still going down waits; it goes once it stops */
    tiles_strike_settle_begin(&st, 150.0f, 1000u);
    float depth = 150.0f;
    uint32_t t = 1000u;
    for (int i = 0; i < 4; i++) {
        depth += 40.0f;
        t += 2u;
        tiles_strike_settle_sample(&st, depth, t);
        assert(!tiles_strike_settle_ready(&st, depth, FULL, t));
    }
    tiles_strike_settle_sample(&st, depth + 2.0f, t + 2u);
    tiles_strike_settle_sample(&st, depth - 5.0f, t + 4u); /* springing back counts as stopped */
    assert(tiles_strike_settle_ready(&st, depth + 2.0f, FULL, t + 4u));

    /* 6. it reaches full depth mid-travel: goes then */
    tiles_strike_settle_begin(&st, 150.0f, 1000u);
    tiles_strike_settle_sample(&st, 450.0f, 1002u);
    assert(!tiles_strike_settle_ready(&st, 450.0f, FULL, 1002u));
    tiles_strike_settle_sample(&st, 710.0f, 1004u);
    assert(tiles_strike_settle_ready(&st, 710.0f, FULL, 1004u));

    /* 7. jitter at the rise size keeps it waiting (late, never early) */
    tiles_strike_settle_begin(&st, 300.0f, 1000u);
    float jitter = 300.0f;
    for (uint32_t ms = 1002u; ms < 1020u; ms += 2u) {
        jitter += 9.0f;
        tiles_strike_settle_sample(&st, jitter, ms);
        assert(!tiles_strike_settle_ready(&st, jitter, FULL, ms));
    }
    assert(tiles_strike_settle_ready(&st, jitter, FULL, 1020u));

    printf("strike_settle: all tests pass\n");
    return 0;
}
