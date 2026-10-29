/*
 * services/light_effects.c -- the playing light effects: each one lights
 * where it should, fades out, and Off does nothing. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>

#include "light_effects.h"

static float lum(const float c[3]) {
    float m = c[0] > c[1] ? c[0] : c[1];
    return m > c[2] ? m : c[2];
}

static void step_to(uint32_t ms) {
    for (uint32_t t = 0; t <= ms; t += 5u) {
        tiles_fx_step(t);
    }
}

int main(void) {
    float c[3];

    /* 1. Off: nothing, never busy */
    tiles_fx_set(TILES_FX_OFF);
    tiles_fx_hit(15, 100, 1000u);
    assert(!tiles_fx_pad_light(15, 1000u, c) && !tiles_fx_busy(1000u));

    /* 2. Ripple: the ring reaches a pad two away about 0.2 s later, then everything fades */
    tiles_fx_set(TILES_FX_RIPPLE);
    tiles_fx_hit(15, 127, 1000u); /* row 3, column 3 */
    assert(tiles_fx_busy(1000u));
    assert(tiles_fx_pad_light(17, 1222u, c) && lum(c) > 0.3f);  /* two columns over, ring there */
    assert(!tiles_fx_pad_light(17, 1000u, c) || lum(c) < 0.05f); /* not yet at the start */
    assert(!tiles_fx_pad_light(17, 3500u, c) && !tiles_fx_busy(3500u));

    /* 3. Cross: same row and column only */
    tiles_fx_set(TILES_FX_CROSS);
    tiles_fx_hit(15, 127, 1000u);
    assert(tiles_fx_pad_light(13, 1150u, c));  /* same row */
    assert(tiles_fx_pad_light(3, 1150u, c));   /* same column */
    assert(!tiles_fx_pad_light(8, 1150u, c));  /* diagonal: dark */

    /* 4. Afterglow: lit only after release, then gone */
    tiles_fx_set(TILES_FX_AFTERGLOW);
    tiles_fx_hit(1, 127, 1000u);
    assert(!tiles_fx_pad_light(1, 1100u, c)); /* held: the pressed white is the lighting's */
    tiles_fx_release(1, 1200u);
    assert(tiles_fx_pad_light(1, 1250u, c) && lum(c) > 0.5f);
    assert(!tiles_fx_pad_light(1, 3500u, c));

    /* 5. Touch colour: a soft hit is bluish, a hard one magenta, while held */
    tiles_fx_set(TILES_FX_TOUCH_COLOUR);
    tiles_fx_hit(2, 20, 1000u);
    assert(tiles_fx_pressed_colour(2, c) && c[2] > c[0]);           /* blue over red */
    tiles_fx_hit(3, 127, 1000u);
    assert(tiles_fx_pressed_colour(3, c) && c[0] > 0.9f && c[2] > 0.9f && c[1] < 0.1f);
    tiles_fx_release(3, 1100u);
    assert(!tiles_fx_pressed_colour(3, c) && tiles_fx_pad_light(3, 1150u, c));

    /* 6. Warmth: repeated hits glow brighter than one; it cools away */
    tiles_fx_set(TILES_FX_WARMTH);
    tiles_fx_hit(5, 100, 0u);
    tiles_fx_hit(6, 100, 0u);
    tiles_fx_hit(6, 100, 0u);
    tiles_fx_hit(6, 100, 0u);
    float once[3], thrice[3];
    tiles_fx_pad_light(5, 10u, once);
    tiles_fx_pad_light(6, 10u, thrice);
    assert(lum(thrice) > lum(once));
    step_to(20000u);
    assert(!tiles_fx_pad_light(6, 20000u, c) && !tiles_fx_busy(20000u));

    /* 7. Rise: the hit's column, from the bottom up */
    tiles_fx_set(TILES_FX_RISE);
    tiles_fx_hit(9, 127, 1000u); /* column 3 */
    assert(tiles_fx_pad_light(21, 1060u, c));  /* bottom of column 3 */
    assert(!tiles_fx_pad_light(22, 1060u, c)); /* column 4: dark */

    /* 8. Ink spreads to a neighbour over time */
    tiles_fx_set(TILES_FX_INK);
    tiles_fx_hit(15, 127, 0u);
    assert(!tiles_fx_pad_light(16, 0u, c));
    step_to(400u);
    assert(tiles_fx_pad_light(16, 400u, c));

    /* 9. The colour effects take their hue from velocity: soft bluer, hard redder */
    {
        static const struct {
            tiles_fx_t fx;
            uint8_t hit, look;
            uint32_t at_ms;
        } CASES[] = {
            {TILES_FX_PRISM_RIPPLE, 15, 15, 1010u}, /* the ring at the hit */
            {TILES_FX_RISE, 9, 21, 1060u},          /* bottom of the column */
            {TILES_FX_INK, 15, 15, 1000u},
        };
        for (unsigned k = 0; k < sizeof(CASES) / sizeof(CASES[0]); k++) {
            float soft[3], hard[3];
            tiles_fx_set(CASES[k].fx);
            tiles_fx_hit(CASES[k].hit, 20, 1000u);
            assert(tiles_fx_pad_light(CASES[k].look, CASES[k].at_ms, soft));
            tiles_fx_set(CASES[k].fx);
            tiles_fx_hit(CASES[k].hit, 127, 1000u);
            assert(tiles_fx_pad_light(CASES[k].look, CASES[k].at_ms, hard));
            assert(soft[2] > soft[0] && hard[0] > hard[2]);
        }
        /* Heatmap: one hard hit is past violet, one soft hit still blue */
        float soft[3], hard[3];
        tiles_fx_set(TILES_FX_HEATMAP);
        tiles_fx_hit(5, 20, 0u);
        tiles_fx_hit(6, 127, 0u);
        tiles_fx_pad_light(5, 0u, soft);
        tiles_fx_pad_light(6, 0u, hard);
        assert(soft[1] > hard[1] + 0.2f && hard[0] > 0.4f);
    }

    /* 10. Sequencer versions: a ripple stays next to the hit; a playhead
     * hit is fainter than a live press and fades at once (a trail) */
    tiles_fx_set_sequencer(true);
    tiles_fx_set(TILES_FX_RIPPLE);
    tiles_fx_hit(15, 127, 1000u);
    assert(tiles_fx_pad_light(16, 1111u, c));                      /* one pad over */
    assert(!tiles_fx_pad_light(17, 1222u, c) || lum(c) < 0.02f);    /* two: dark */
    float live[3], played[3];
    tiles_fx_set(TILES_FX_HALO);
    tiles_fx_hit(15, 100, 1000u);
    tiles_fx_pad_light(16, 1010u, live);
    tiles_fx_set(TILES_FX_HALO);
    tiles_fx_playback_hit(15, 100, 1000u);
    tiles_fx_pad_light(16, 1010u, played);
    assert(lum(played) > 0.02f && lum(played) < lum(live) * 0.6f);
    tiles_fx_set(TILES_FX_AFTERGLOW);
    tiles_fx_playback_hit(3, 127, 1000u);
    assert(tiles_fx_pad_light(3, 1100u, c) && !tiles_fx_pad_light(3, 3500u, c));
    tiles_fx_set_sequencer(false);
    tiles_fx_set(TILES_FX_RIPPLE);
    tiles_fx_hit(15, 127, 1000u);
    assert(tiles_fx_pad_light(17, 1222u, c) && lum(c) > 0.3f);      /* full again */

    /* 11. Every effect is quiet again a while after a hit, and ignores bad pads */
    for (int fx = 0; fx < TILES_FX_COUNT; fx++) {
        tiles_fx_set((tiles_fx_t)fx);
        tiles_fx_hit(0, 100, 0u);
        tiles_fx_hit(25, 100, 0u);
        assert(!tiles_fx_pad_light(0, 0u, c) && !tiles_fx_pad_light(25, 0u, c));
        tiles_fx_hit(12, 100, 0u);
        tiles_fx_release(12, 100u);
        step_to(30000u);
        assert(!tiles_fx_busy(30000u));
    }

    printf("light_effects: all tests pass\n");
    return 0;
}
