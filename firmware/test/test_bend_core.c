/*
 * services/bend_core.c -- pitch bend from tilt: centred until the baseline
 * settles, a straight on-axis press never bends, a held tilt bends one way
 * or the other and saturates at the declared range, small tilts stay in the
 * deadzone, and letting go returns to centre. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "bend_core.h"

static const tiles_bend_config_t CFG = {0.065f, 12.0f};
#define RANGE 2047 /* 12 of the receiver's 48 semitones: 8191 / 4 */

/* Runs `ms` of 2 ms samples at a fixed reading; returns the last bend (or
 * the centre if none came out). */
static uint16_t hold(tiles_bend_t *b, int16_t x, int16_t y, int16_t z, float depth, uint32_t *t, uint32_t ms) {
    uint16_t bend = b->center, out;
    for (uint32_t end = *t + ms; *t < end; *t += 2u) {
        if (tiles_bend_step(b, &CFG, x, y, z, depth, *t, &out)) {
            bend = out;
        }
    }
    return bend;
}

int main(void) {
    tiles_bend_t b;
    uint16_t out;
    uint32_t t;

    /* 1. nothing comes out until the baseline has settled (120 ms) */
    t = 1000u;
    tiles_bend_start(&b, 0, 0, 1000, 0.0f, TILES_BEND_CENTER, t);
    assert(!tiles_bend_step(&b, &CFG, 0, 0, 1000, 0.0f, 1100u, &out));
    assert(tiles_bend_step(&b, &CFG, 0, 0, 1000, 0.0f, 1120u, &out) == false); /* the baseline is taken */
    assert(tiles_bend_step(&b, &CFG, 0, 0, 1000, 0.0f, 1122u, &out) && out == TILES_BEND_CENTER);

    /* 2. a straight press on axis never bends, however deep */
    t = 0u;
    tiles_bend_start(&b, 0, 0, 800, 0.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 800, 0.0f, &t, 200u);
    for (int z = 800; z < 3000; z += 20) {
        assert(hold(&b, 0, 0, (int16_t)z, (float)(z - 800), &t, 4u) == TILES_BEND_CENTER);
    }

    /* 3. a held tilt bends; +X and -X go opposite ways */
    t = 0u;
    tiles_bend_start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    uint16_t plus = hold(&b, 45, 0, 1000, 300.0f, &t, 400u);
    assert(plus < TILES_BEND_CENTER - 300u);
    tiles_bend_start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    uint16_t minus = hold(&b, -45, 0, 1000, 300.0f, &t, 400u);
    assert(minus > TILES_BEND_CENTER + 300u);
    assert(abs((int)plus - (int)TILES_BEND_CENTER) - abs((int)minus - (int)TILES_BEND_CENTER) <= 2);

    /* 4. a big tilt saturates at the declared range, never beyond */
    t = 0u;
    tiles_bend_start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    uint16_t full = hold(&b, 400, 0, 1000, 300.0f, &t, 600u);
    assert(full == TILES_BEND_CENTER - RANGE);

    /* 5. a tilt inside the deadzone (0.015 of |B|) stays centred */
    t = 0u;
    tiles_bend_start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    assert(hold(&b, 15, 0, 1000, 300.0f, &t, 400u) == TILES_BEND_CENTER);

    /* 6. coming back from a tilt returns to centre */
    t = 0u;
    tiles_bend_start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    assert(hold(&b, 60, 0, 1000, 300.0f, &t, 400u) != TILES_BEND_CENTER);
    assert(hold(&b, 0, 0, 1000, 300.0f, &t, 600u) == TILES_BEND_CENTER);

    /* 7. a quarter-tone note bends around its own centre */
    t = 0u;
    tiles_bend_start(&b, 0, 0, 1000, 300.0f, 8277u, t);
    assert(hold(&b, 0, 0, 1000, 300.0f, &t, 300u) == 8277u);
    assert(hold(&b, 400, 0, 1000, 300.0f, &t, 600u) == 8277u - RANGE);

    printf("bend_core: all tests pass\n");
    return 0;
}
