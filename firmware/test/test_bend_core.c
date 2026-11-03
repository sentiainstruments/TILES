/*
 * services/bend_core.c -- pitch bend from tilt, in every mode: centred until
 * the baseline settles, a straight on-axis press never bends, a held tilt
 * bends one way or the other and saturates at the declared range, small
 * tilts stay in the deadzone, and letting go returns to centre. Then PATH:
 * learning a pad's press path from strokes, a misaligned magnet's straight
 * press staying centred, and a held tilt keeping its bend under pressure.
 * Run with test/run.sh.
 */

#include <assert.h>
#include <math.h>
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

static tiles_bend_mode_t s_mode;

static void start(tiles_bend_t *b, int16_t x, int16_t y, int16_t z, float depth, uint16_t center, uint32_t t) {
    tiles_bend_start(b, s_mode, NULL, x, y, z, depth, center, t);
}

/* The basics, the same in every mode. */
static void test_basics(void) {
    tiles_bend_t b;
    uint16_t out;
    uint32_t t;

    /* 1. nothing comes out until the baseline has settled (120 ms) */
    t = 1000u;
    start(&b, 0, 0, 1000, 0.0f, TILES_BEND_CENTER, t);
    assert(!tiles_bend_step(&b, &CFG, 0, 0, 1000, 0.0f, 1100u, &out));
    assert(tiles_bend_step(&b, &CFG, 0, 0, 1000, 0.0f, 1120u, &out) == false); /* the baseline is taken */
    assert(tiles_bend_step(&b, &CFG, 0, 0, 1000, 0.0f, 1122u, &out) && out == TILES_BEND_CENTER);

    /* 2. a straight press on axis never bends, however deep */
    t = 0u;
    start(&b, 0, 0, 800, 0.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 800, 0.0f, &t, 200u);
    for (int z = 800; z < 3000; z += 20) {
        assert(hold(&b, 0, 0, (int16_t)z, (float)(z - 800), &t, 4u) == TILES_BEND_CENTER);
    }

    /* 3. a held tilt bends; +X and -X go opposite ways */
    t = 0u;
    start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    uint16_t plus = hold(&b, 45, 0, 1000, 300.0f, &t, 400u);
    assert(plus < TILES_BEND_CENTER - 300u);
    start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    uint16_t minus = hold(&b, -45, 0, 1000, 300.0f, &t, 400u);
    assert(minus > TILES_BEND_CENTER + 300u);
    assert(abs((int)plus - (int)TILES_BEND_CENTER) - abs((int)minus - (int)TILES_BEND_CENTER) <= 2);

    /* 4. a big tilt saturates at the declared range, never beyond */
    t = 0u;
    start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    uint16_t full = hold(&b, 400, 0, 1000, 300.0f, &t, 600u);
    assert(full == TILES_BEND_CENTER - RANGE);

    /* 5. a tilt inside the deadzone (0.015 of |B|) stays centred */
    t = 0u;
    start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    assert(hold(&b, 15, 0, 1000, 300.0f, &t, 400u) == TILES_BEND_CENTER);

    /* 6. coming back from a tilt returns to centre */
    t = 0u;
    start(&b, 0, 0, 1000, 300.0f, TILES_BEND_CENTER, t);
    hold(&b, 0, 0, 1000, 300.0f, &t, 300u);
    assert(hold(&b, 60, 0, 1000, 300.0f, &t, 400u) != TILES_BEND_CENTER);
    assert(hold(&b, 0, 0, 1000, 300.0f, &t, 600u) == TILES_BEND_CENTER);

    /* 7. a quarter-tone note bends around its own centre */
    t = 0u;
    start(&b, 0, 0, 1000, 300.0f, 8277u, t);
    assert(hold(&b, 0, 0, 1000, 300.0f, &t, 300u) == 8277u);
    assert(hold(&b, 400, 0, 1000, 300.0f, &t, 600u) == 8277u - RANGE);

}

/* A misaligned magnet: on a straight press X and Y follow a line in Z. */
#define PATH_A 30.0f
#define PATH_BX 0.12f
#define PATH_BY (-0.05f)
static int16_t path_x(float z) {
    return (int16_t)lrintf(PATH_A + PATH_BX * z);
}
static int16_t path_y(float z) {
    return (int16_t)lrintf(PATH_BY * z);
}

/* Small deterministic noise, +/- amp. */
static unsigned s_seed = 12345u;
static float noise(float amp) {
    s_seed = s_seed * 1103515245u + 12345u;
    return amp * ((float)((s_seed >> 8) & 0xFFFFu) / 32768.0f - 1.0f);
}

/* A strike from rest (Z 1000) to `z_end` in `samples` reads along the path. */
static bool stroke(tiles_press_path_t *p, float z_end, int samples, float wobble) {
    tiles_press_path_stroke_begin(p);
    for (int i = 0; i <= samples; i++) {
        float z = 1000.0f + (z_end - 1000.0f) * (float)i / (float)samples;
        float rock = wobble * sinf(3.0f * (float)i / (float)samples * 3.14159f);
        tiles_press_path_stroke_sample(p, (int16_t)(path_x(z) + noise(40.0f) + rock),
                                       (int16_t)(path_y(z) + noise(40.0f)), (int16_t)(z + noise(20.0f)));
    }
    return tiles_press_path_stroke_end(p);
}

/* Holds a note on the path at depth `z`, with `tilt` (sin of the lean)
 * adding tilt x |B| to X; returns the last bend. */
static uint16_t hold_on_path(tiles_bend_t *b, float z, float tilt, uint32_t *t, uint32_t ms) {
    float mag = sqrtf(z * z);
    return hold(b, (int16_t)lrintf(PATH_A + PATH_BX * z + tilt * mag), path_y(z), (int16_t)lrintf(z), z - 1000.0f, t,
                ms);
}

static void test_path(void) {
    tiles_press_path_t p;
    tiles_bend_t b;
    uint32_t t;

    /* 8. strokes along the path teach its slope; bad strokes are refused */
    tiles_press_path_reset(&p);
    assert(!stroke(&p, 1100.0f, 8, 0.0f));  /* too shallow (Z range 100) */
    assert(!stroke(&p, 2500.0f, 2, 0.0f));  /* too few samples */
    assert(!stroke(&p, 2500.0f, 12, 400.0f)); /* rocking: far off a line */
    assert(p.strokes == 0u);
    for (int i = 0; i < 16; i++) {
        assert(stroke(&p, 1800.0f + 50.0f * (float)i, 10, 0.0f));
    }
    assert(p.strokes == 16u);
    assert(fabsf(p.bx - PATH_BX) < 0.01f && fabsf(p.by - PATH_BY) < 0.01f);
    /* a few tilted strikes barely move a learned slope */
    tiles_press_path_t tilted = p;
    tiles_press_path_stroke_begin(&tilted);
    for (int i = 0; i <= 10; i++) {
        float z = 1000.0f + 150.0f * (float)i;
        tiles_press_path_stroke_sample(&tilted, (int16_t)(path_x(z) + 0.3f * (z - 1000.0f)), path_y(z), (int16_t)z);
    }
    assert(tiles_press_path_stroke_end(&tilted));
    assert(fabsf(tilted.bx - PATH_BX) < 0.03f);

    /* 9. straight presses on the misaligned magnet. A slow swell to 2.5x the
     * rest field and a fast release, then rocking while pressing deeper:
     * PATH stays centred, and comes back to it. */
    {
        const int mode = TILES_BEND_MODE_PATH;
        uint16_t worst = 0u;
        t = 0u;
        tiles_bend_start(&b, (tiles_bend_mode_t)mode, &p, path_x(1300.0f), path_y(1300.0f), 1300, 300.0f,
                         TILES_BEND_CENTER, t);
        hold_on_path(&b, 1300.0f, 0.0f, &t, 200u);
        for (float z = 1300.0f; z < 2500.0f; z += 1.0f) { /* slow: 2.4 s */
            uint16_t out = hold_on_path(&b, z, 0.0f, &t, 2u);
            uint16_t off = (uint16_t)abs((int)out - (int)TILES_BEND_CENTER);
            worst = off > worst ? off : worst;
        }
        for (float z = 2500.0f; z > 1300.0f; z -= 60.0f) { /* fast: 40 ms back */
            uint16_t out = hold_on_path(&b, z, 0.0f, &t, 2u);
            uint16_t off = (uint16_t)abs((int)out - (int)TILES_BEND_CENTER);
            worst = off > worst ? off : worst;
        }
        assert(worst == 0u);

        t = 0u;
        tiles_bend_start(&b, (tiles_bend_mode_t)mode, &p, path_x(1300.0f), path_y(1300.0f), 1300, 300.0f,
                         TILES_BEND_CENTER, t);
        hold_on_path(&b, 1300.0f, 0.0f, &t, 200u);
        for (float z = 1300.0f; z < 2500.0f; z += 20.0f) { /* rocks while pressing deeper */
            hold_on_path(&b, z, 0.06f, &t, 2u);
        }
        assert(hold_on_path(&b, 2500.0f, 0.0f, &t, 400u) == TILES_BEND_CENTER); /* then straight */
    }

    /* 10. a held tilt, then the player presses harder: PATH keeps the bend
     * within the report's 20% (what's left is the learned slope's error);
     * LEGACY's frozen |B| adds the press to it (+70% here), the Alpha 6.6
     * problem */
    for (int mode = TILES_BEND_MODE_LEGACY; mode <= TILES_BEND_MODE_PATH; mode += 2) {
        t = 0u;
        tiles_bend_start(&b, (tiles_bend_mode_t)mode, &p, path_x(1300.0f), path_y(1300.0f), 1300, 300.0f,
                         TILES_BEND_CENTER, t);
        hold_on_path(&b, 1300.0f, 0.0f, &t, 200u);
        uint16_t held = hold_on_path(&b, 1300.0f, 0.05f, &t, 400u);
        int held_off = abs((int)held - (int)TILES_BEND_CENTER);
        assert(held_off > 300);
        for (float z = 1300.0f; z < 2400.0f; z += 4.0f) {
            hold_on_path(&b, z, 0.05f, &t, 2u);
        }
        uint16_t pressed = hold_on_path(&b, 2400.0f, 0.05f, &t, 300u);
        int change = (int)pressed - (int)held;
        if (mode == TILES_BEND_MODE_PATH) {
            assert(abs(change) * 5 <= held_off);
        } else {
            assert(abs(change) * 5 > held_off * 2);
        }
    }

    /* 11. too few strokes: the note falls back to its own direction, which
     * still holds a constant-direction field */
    tiles_press_path_t fresh;
    tiles_press_path_reset(&fresh);
    t = 0u;
    tiles_bend_start(&b, TILES_BEND_MODE_PATH, &fresh, 120, -60, 1000, 300.0f, TILES_BEND_CENTER, t);
    assert(!b.path_learned);
    hold(&b, 120, -60, 1000, 300.0f, &t, 200u);
    for (int z = 1000; z < 2500; z += 10) {
        assert(hold(&b, (int16_t)(z * 12 / 100), (int16_t)(-z * 6 / 100), (int16_t)z, (float)(z - 1000), &t, 2u) ==
               TILES_BEND_CENTER);
    }
}

/* 12. vibrato energy reads the same in raw units in every mode, so its
 * thresholds carry over */
static void test_wiggle_scale(void) {
    float energy[TILES_BEND_MODE_COUNT];
    for (int mode = 0; mode < TILES_BEND_MODE_COUNT; mode++) {
        tiles_bend_t b;
        uint32_t t = 0u;
        tiles_bend_start(&b, (tiles_bend_mode_t)mode, NULL, 0, 0, 4000, 300.0f, TILES_BEND_CENTER, t);
        uint16_t out;
        for (int i = 0; i < 1000; i++, t += 2u) {
            int16_t x = (int16_t)lrintf(150.0f * sinf(2.0f * 3.14159f * 6.0f * (float)t / 1000.0f));
            tiles_bend_step(&b, &CFG, x, 0, 4000, 300.0f, t, &out);
        }
        energy[mode] = b.wiggle_energy;
    }
    assert(energy[0] > 8.0f);
    for (int mode = 1; mode < TILES_BEND_MODE_COUNT; mode++) {
        assert(fabsf(energy[mode] - energy[0]) < 0.1f * energy[0]);
    }
}

int main(void) {
    for (int mode = 0; mode < TILES_BEND_MODE_COUNT; mode++) {
        s_mode = (tiles_bend_mode_t)mode;
        test_basics();
    }
    test_path();
    test_wiggle_scale();
    printf("bend_core: all tests pass\n");
    return 0;
}
