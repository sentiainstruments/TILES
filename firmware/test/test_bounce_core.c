/*
 * services/bounce_core.c -- Bouncing notes' balls: periods, the drop, the
 * landings and the polyrhythm between heights. Run with test/run.sh.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "bounce_core.h"

int main(void) {
    tiles_bounce_t b;
    tiles_bounce_clear(&b);

    /* 1. periods: every 1, 2, 4, 6 steps for heights 1-4 */
    assert(tiles_bounce_period(0) == 0 && tiles_bounce_period(1) == 1);
    assert(tiles_bounce_period(2) == 2 && tiles_bounce_period(3) == 4 && tiles_bounce_period(4) == 6);

    /* 2. off lanes never land */
    for (int i = 0; i < 20; i++) {
        assert(tiles_bounce_step(&b) == 0u);
    }

    /* 3. a ball dropped from 4 starts at the top, falls, lands, climbs back */
    tiles_bounce_set_height(&b, 0, 4);
    assert(tiles_bounce_ball_height(&b, 0) == 4);
    const uint8_t path[] = {3, 2, 1, 2, 3, 4, 3, 2, 1};
    for (unsigned i = 0; i < sizeof(path); i++) {
        uint16_t landed = tiles_bounce_step(&b);
        assert(tiles_bounce_ball_height(&b, 0) == path[i]);
        assert(((landed & 1u) != 0) == (path[i] == 1)); /* lands exactly on the floor */
    }

    /* 4. height 1 lands every step; off again: silent */
    tiles_bounce_set_height(&b, 0, 1);
    for (int i = 0; i < 5; i++) {
        assert(tiles_bounce_step(&b) & 1u);
        assert(tiles_bounce_ball_height(&b, 0) == 1);
    }
    tiles_bounce_set_height(&b, 0, 0);
    assert(tiles_bounce_step(&b) == 0u && tiles_bounce_ball_height(&b, 0) == 0);

    /* 5. heights 4 and 3 from the same moment: 3 against 2 over 12 steps */
    tiles_bounce_clear(&b);
    tiles_bounce_set_height(&b, 2, 4);
    tiles_bounce_set_height(&b, 11, 3);
    int hits4 = 0, hits3 = 0;
    for (int i = 0; i < 24; i++) {
        uint16_t landed = tiles_bounce_step(&b);
        hits4 += (landed >> 2) & 1u;
        hits3 += (landed >> 11) & 1u;
    }
    assert(hits4 == 4 && hits3 == 6);

    /* 6. drop_all puts every ball back at its top */
    tiles_bounce_step(&b);
    tiles_bounce_drop_all(&b);
    assert(tiles_bounce_ball_height(&b, 2) == 4 && tiles_bounce_ball_height(&b, 11) == 3);
    assert(tiles_bounce_ball_height(&b, 5) == 0);

    /* 7. out of range lanes and heights */
    tiles_bounce_set_height(&b, 12, 3);
    tiles_bounce_set_height(&b, 4, 9);
    assert(b.height[4] == 4);

    /* 8. gravity: thrown from 3 pads, it lands half a bounce later (3
     * eighths), then bounces lower and faster each time, then rests */
    tiles_gravity_t g;
    tiles_gravity_clear(&g);
    tiles_gravity_throw(&g, 0, 3.0f);
    assert(fabsf(tiles_gravity_height(&g, 0) - 3.0f) < 1e-4f); /* at the top */
    float impact[TILES_BOUNCE_LANES];
    float t = 0.0f, last_land = 0.0f, last_gap = 1e9f, last_impact = 1e9f;
    int landings = 0;
    while (t < 200.0f && g.apex[0] > 0.0f) {
        uint16_t landed = tiles_gravity_step(&g, 0.01f, impact);
        t += 0.01f;
        if (landed & 1u) {
            if (landings == 0) {
                assert(fabsf(t - 3.0f) < 0.02f); /* half of 6 eighths */
            } else {
                float gap = t - last_land;
                assert(gap < last_gap);       /* faster */
                last_gap = gap;
            }
            assert(impact[0] < last_impact); /* softer */
            last_impact = impact[0];
            last_land = t;
            landings++;
        }
        assert(tiles_gravity_height(&g, 0) <= 3.0f + 1e-4f);
    }
    assert(g.apex[0] == 0.0f && landings > 5 && landings < 20 && t < 60.0f); /* comes to rest */

    /* 9. at rest: no landings, height 0; a big step lands once and counts */
    assert(tiles_gravity_step(&g, 5.0f, impact) == 0u && tiles_gravity_height(&g, 0) == 0.0f);
    tiles_gravity_throw(&g, 3, 1.0f);
    assert(tiles_gravity_step(&g, 50.0f, impact) & (1u << 3));

    printf("bounce_core: all tests pass\n");
    return 0;
}
