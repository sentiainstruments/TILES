/*
 * services/physics_core.c -- the physics modes: Ripples' rings land on
 * sixteenths, reach as far as the strike was hard, and their stones drop
 * again every bar until lifted out; Sand plays its piles as a melody on
 * triplets (height = pitch, upside down when flipped), pours faster
 * pressed harder, slides and runs down; Orbits keep whole-beat periods,
 * come off with a second strike, follow a moved sun, stop and carry on on
 * the grid, and never fly off under Pull; the cradle knocks on the beat
 * for ever, comes to rest with Real, and is caught by a touch. Run with
 * test/run.sh.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "physics_core.h"

#define DT (1.0 / 256.0) /* a step, in beats */

static bool on_grid(double t, double per_beat) {
    double x = t * per_beat;
    return fabs(x - floor(x + 0.5)) < 1e-6;
}

/* Within a step after a grid point. */
static bool just_after_grid(double t, double per_beat) {
    double x = t * per_beat;
    return x - floor(x) < DT * per_beat + 1e-9;
}

static int notes_on(const tiles_phys_out_t *out, uint8_t cell) {
    int n = 0;
    for (uint8_t i = 0; i < out->note_count; i++) {
        n += out->note_cell[i] == cell ? 1 : 0;
    }
    return n;
}

static int velocity_on(const tiles_phys_out_t *out, uint8_t cell) {
    for (uint8_t i = 0; i < out->note_count; i++) {
        if (out->note_cell[i] == cell) {
            return out->note_velocity[i];
        }
    }
    return 0;
}

static tiles_ripples_t r;
static tiles_sand_t s;
static tiles_orbits_t o;
static tiles_cradle_t c;

static void test_ripples(void) {
    tiles_phys_out_t out;
    double t = 10.1;
    /* 1. A light strike: its cell at once, the neighbours on later
     * sixteenths, nothing far away (until its next drop, at 14). */
    tiles_ripples_clear(&r, 7u, t);
    tiles_phys_out_clear(&out);
    assert(tiles_ripples_strike(&r, 14u, 0.3f, t, &out)); /* row 2, col 2 */
    assert(out.note_count == 1u && out.note_cell[0] == 14u && out.note_degree[0] < 0);
    tiles_ripples_release(&r, 14u);
    int near = 0, far = 0;
    for (int i = 0; i < 900; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_ripples_step(&r, t, &out);
        for (uint8_t k = 0; k < out.note_count; k++) {
            uint8_t cell = out.note_cell[k];
            float dx = (float)(cell % 6) - 2.0f, dy = (float)(cell / 6) - 2.0f;
            if (sqrtf(dx * dx + dy * dy) <= 1.5f) {
                near++;
            } else {
                far++;
            }
        }
    }
    assert(near >= 4 && far == 0);

    /* 2. A hard strike reaches across the grid, every hit on a sixteenth,
     * softer further out. */
    tiles_ripples_clear(&r, 7u, t);
    tiles_phys_out_clear(&out);
    tiles_ripples_strike(&r, 0u, 1.0f, t, &out);
    tiles_ripples_release(&r, 0u);
    int v_near = 0, v_far = 0, hits = 0;
    for (int i = 0; i < 700; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_ripples_step(&r, t, &out);
        if (out.note_count > 0u) {
            hits += out.note_count;
            assert(just_after_grid(t, 4.0));
        }
        if (notes_on(&out, 1u)) {
            v_near = velocity_on(&out, 1u);
        }
        if (notes_on(&out, 23u)) {
            v_far = velocity_on(&out, 23u);
        }
    }
    assert(hits >= 15);
    assert(v_near > 0 && v_near > v_far);

    /* 3. The stone stays: it drops again every bar at its sixteenth, and
     * pressing while it's held makes it bigger. */
    tiles_ripples_clear(&r, 7u, 20.0);
    t = 21.26; /* the sixteenth 21.25: drops again at 25.25, 29.25, ... */
    tiles_phys_out_clear(&out);
    tiles_ripples_strike(&r, 9u, 0.4f, t, &out);
    tiles_ripples_set_pressure(&r, 9u, 0.9f);
    assert(fabsf(r.stone[0].strength - 0.9f) < 1e-6f);
    tiles_ripples_release(&r, 9u);
    tiles_ripples_set_pressure(&r, 9u, 0.1f); /* let go: no change */
    assert(fabsf(r.stone[0].strength - 0.9f) < 1e-6f);
    int drops = 0;
    double first = -1.0;
    for (int i = 0; i < 12 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_ripples_step(&r, t, &out);
        if (notes_on(&out, 9u) && velocity_on(&out, 9u) > 100) {
            if (first < 0.0) {
                first = t;
            }
            assert(fabs(fmod(t - 21.25, 4.0)) < 2.0 * DT);
            drops++;
        }
    }
    assert(drops == 3 && fabs(first - 25.25) < 2.0 * DT);

    /* 4. A strike on the stone lifts it out: no more drops. */
    tiles_phys_out_clear(&out);
    assert(tiles_ripples_has_stone(&r, 9u));
    assert(!tiles_ripples_strike(&r, 9u, 1.0f, t, &out));
    assert(out.note_count == 0u && !tiles_ripples_has_stone(&r, 9u));
    for (int i = 0; i < 8 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_ripples_step(&r, t, &out);
        assert(notes_on(&out, 9u) == 0);
    }
    assert(!tiles_ripples_active(&r));

    /* 5. Circle + tap takes a stone too. */
    tiles_phys_out_clear(&out);
    tiles_ripples_strike(&r, 3u, 0.5f, t, &out);
    tiles_ripples_release(&r, 3u);
    assert((tiles_ripples_erasable(&r) >> 3) & 1u);
    assert(tiles_ripples_erase(&r, 3u) && !tiles_ripples_has_stone(&r, 3u));

    /* 6. Splash: drops land on eighths, a pad or two away. */
    tiles_ripples_clear(&r, 99u, 30.0);
    r.splash = true;
    t = 30.03;
    tiles_phys_out_clear(&out);
    tiles_ripples_strike(&r, 8u, 1.0f, t, &out);
    int n = 0;
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_DROPS; i++) {
        if (r.drop[i].live) {
            n++;
            assert(on_grid(r.drop[i].at, 2.0) && r.drop[i].at > t);
            float dx = (float)(r.drop[i].to % 6) - 2.0f, dy = (float)(r.drop[i].to / 6) - 1.0f;
            float d = sqrtf(dx * dx + dy * dy);
            assert(d >= 1.4f && d <= 2.9f);
        }
    }
    assert(n == 2);
    r.splash = false;
}

/* Runs Sand for `beats`, collecting the playhead's notes (degree >= 0,
 * loud) by column. */
static void run_sand(double *t, double beats, int played[6], int degree[6], double when[6]) {
    tiles_phys_out_t out;
    for (int i = 0; i < (int)(beats * 256.0); i++) {
        *t += DT;
        tiles_phys_out_clear(&out);
        tiles_sand_step(&s, *t, 0.5f, &out);
        for (uint8_t k = 0; k < out.note_count; k++) {
            if (out.note_degree[k] >= 0 && out.note_velocity[k] > 80 && played != NULL) {
                uint8_t col = (uint8_t)(out.note_cell[k] % 6u);
                played[col]++;
                degree[col] = out.note_degree[k];
                when[col] = *t;
            }
        }
    }
}

static void test_sand(void) {
    tiles_phys_out_t out;
    assert(tiles_sand_division(0.0f) == 1u && tiles_sand_division(0.3f) == 2u);
    assert(tiles_sand_division(0.6f) == 4u && tiles_sand_division(1.0f) == 8u);

    /* 1. A tap drops one grain: it lands on the floor, sounding the
     * column's first note (degree 0), softly. */
    double t = 4.0;
    memset(&s, 0, sizeof(s));
    tiles_sand_clear(&s, t);
    tiles_sand_strike(&s, 2u, t);
    tiles_sand_release(&s, 2u);
    int previews = 0;
    while (t < 6.0) { /* about 1.2 beats to fall at 120 BPM */
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_sand_step(&s, t, 0.5f, &out);
        for (uint8_t k = 0; k < out.note_count; k++) {
            if (out.note_cell[k] == 20u && out.note_degree[k] == 0 && out.note_velocity[k] < 60) {
                previews++;
            }
        }
    }
    assert(previews == 1 && s.h[2] == 1u && tiles_sand_degree(&s, 2u) == 0);

    /* 2. The playhead: a column a triplet eighth, column 1 on the beat;
     * each pile at its height, empty columns silent. */
    tiles_sand_clear(&s, 8.0);
    s.h[0] = 1u;
    s.h[2] = 3u;
    s.h[3] = 2u; /* within 2 of its neighbours: nothing slides */
    s.h[5] = 1u;
    t = 8.0;
    int played[6] = {0}, degree[6] = {0};
    double when[6] = {0};
    run_sand(&t, 1.99, played, degree, when); /* one loop: 8.0 .. 9.67 */
    assert(played[0] == 1 && played[1] == 0 && played[2] == 1 && played[3] == 1 && played[4] == 0);
    assert(played[5] == 1);
    assert(degree[0] == 0 && degree[2] == 2 && degree[3] == 1 && degree[5] == 0);
    assert(just_after_grid(when[0], 1.0) && fabs(when[0] - 8.0) < 2.0 * DT); /* on the beat */
    assert(just_after_grid(when[2], 3.0) && fabs(when[2] - 8.0 - 2.0 / 3.0) < 2.0 * DT);

    /* 3. Avalanches smooth the melody: no step more than 2 grains from its
     * neighbour once they settle. */
    tiles_sand_clear(&s, t);
    s.h[2] = 10u;
    tiles_sand_strike(&s, 2u, t); /* one more grain sets it going */
    tiles_sand_release(&s, 2u);
    run_sand(&t, 1.5, NULL, NULL, NULL);
    for (int col = 0; col + 1 < 6; col++) {
        int d = (int)s.h[col] - (int)s.h[col + 1];
        assert(d <= 2 && d >= -2);
    }
    assert(s.h[2] < 11u && s.h[1] > 0u && s.h[3] > 0u);

    /* 4. The hourglass: every 8 beats each pile loses a grain. */
    unsigned before = 0, after = 0;
    for (int col = 0; col < 6; col++) {
        before += s.h[col];
    }
    double next_leak = s.leak_next;
    while (t < next_leak + 0.1) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_sand_step(&s, t, 0.5f, &out);
    }
    unsigned piles = 0;
    for (int col = 0; col < 6; col++) {
        after += s.h[col];
        piles += s.h[col] > 0u ? 1u : 0u;
    }
    assert(before - after >= piles && before - after <= 6u);

    /* 5. Flip: the sand hangs from the top and the melody turns over. */
    tiles_sand_clear(&s, t);
    s.h[0] = 1u;  /* low */
    s.h[1] = 3u;  /* within 2 of its neighbours: nothing slides */
    s.h[2] = 5u;
    s.h[3] = 7u;
    s.h[4] = 9u;
    s.h[5] = 11u; /* high */
    assert(tiles_sand_degree(&s, 0u) == 0 && tiles_sand_degree(&s, 5u) == 10);
    tiles_sand_flip(&s, true);
    run_sand(&t, 1.5, NULL, NULL, NULL);
    assert(s.gdir == -1 && s.h[0] == 1u && s.h[5] == 11u);
    assert(tiles_sand_degree(&s, 0u) == 11 && tiles_sand_degree(&s, 5u) == 1);

    /* 6. Circle + tap empties a column. */
    assert(tiles_sand_erase(&s, 3u) && s.h[3] == 0u && tiles_sand_degree(&s, 3u) == -1);
}

static void test_orbits(void) {
    tiles_phys_out_t out;
    assert(tiles_orbits_period(1.0f) == 1.0f);
    assert(tiles_orbits_period(sqrtf(2.0f)) == 1.5f);
    assert(tiles_orbits_period(2.0f) == 3.0f);
    assert(tiles_orbits_period(sqrtf(5.0f)) == 3.0f);
    assert(tiles_orbits_period(3.0f) == 6.0f);
    assert(tiles_orbits_period(4.0f) == 8.0f);

    memset(&o, 0, sizeof(o));
    o.running = true;
    double t = 3.37;
    tiles_orbits_clear(&o, t);
    /* 1. The sun is there from the start; a tap is a planet. */
    assert(tiles_orbits_sun_count(&o) == 1u && tiles_orbits_is_sun(&o, TILES_ORBITS_HOME_SUN));
    tiles_orbits_launch(&o, 15u, 100u, t); /* a pad from the sun: every beat */
    tiles_orbits_launch(&o, 2u, 90u, t);   /* two rows up: every 3 */
    tiles_orbits_launch(&o, 14u, 90u, t);  /* the sun's own cell: nothing */
    assert(o.planet[0].period == 1.0f && o.planet[1].period == 3.0f && !o.planet[2].live);
    assert(tiles_orbits_planet_at(&o, 15u) == 0 && tiles_orbits_planet_at(&o, 2u) == 1);
    double prev15 = -1.0, prev2 = -1.0;
    int n15 = 0, n2 = 0;
    for (int i = 0; i < 12 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_orbits_step(&o, t, &out);
        if (notes_on(&out, 15u)) {
            if (prev15 >= 0.0) {
                assert(fabs(t - prev15 - 1.0) < 2.0 * DT);
            }
            assert(just_after_grid(t, 4.0));
            prev15 = t;
            n15++;
        }
        if (notes_on(&out, 2u)) {
            if (prev2 >= 0.0) {
                assert(fabs(t - prev2 - 3.0) < 2.0 * DT);
            }
            prev2 = t;
            n2++;
        }
        float x, y;
        tiles_orbits_planet_xy(&o, 0u, t, &x, &y);
        assert(fabsf(hypotf(x - 2.5f, y - 2.5f) - 1.0f) < 1e-3f);
    }
    assert(n15 == 12 && n2 == 4);

    /* 2. Stop: frozen and silent; play: on from a whole beat later. */
    tiles_orbits_stop(&o, t);
    float x0, y0, x1, y1;
    tiles_orbits_planet_xy(&o, 0u, t, &x0, &y0);
    for (int i = 0; i < 300; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_orbits_step(&o, t, &out);
        assert(out.note_count == 0u);
    }
    tiles_orbits_planet_xy(&o, 0u, t, &x1, &y1);
    assert(x0 == x1 && y0 == y1);
    tiles_orbits_play(&o, t);
    assert(fmod(o.offset, 1.0) < 1e-9 || fmod(o.offset, 1.0) > 1.0 - 1e-9);
    prev15 = -1.0;
    for (int i = 0; i < 6 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_orbits_step(&o, t, &out);
        if (notes_on(&out, 15u)) {
            assert(just_after_grid(t, 4.0));
            if (prev15 >= 0.0) {
                assert(fabs(t - prev15 - 1.0) < 2.0 * DT);
            }
            prev15 = t;
        }
    }
    assert(prev15 > 0.0);

    /* 3. Moving the sun: every orbit takes its new distance, in time. */
    tiles_orbits_move_sun(&o, 8u, t); /* row 1, col 2: 15 is now a diagonal away */
    assert(o.planet[0].period == 1.5f && tiles_orbits_is_sun(&o, 8u) && tiles_orbits_sun_count(&o) == 1u);
    prev15 = -1.0;
    for (int i = 0; i < 6 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_orbits_step(&o, t, &out);
        if (notes_on(&out, 15u)) {
            assert(just_after_grid(t, 4.0));
            if (prev15 >= 0.0) {
                assert(fabs(t - prev15 - 1.5) < 2.0 * DT);
            }
            prev15 = t;
        }
    }
    assert(prev15 > 0.0);
    tiles_orbits_move_sun(&o, 2u, t); /* onto a planet: swallowed */
    assert(tiles_orbits_planet_at(&o, 2u) < 0);

    /* 4. Pull: a crowd of planets stays bound for 64 beats; off again,
     * every one plays on the sixteenths at its own period. */
    tiles_orbits_move_sun(&o, TILES_ORBITS_HOME_SUN, t);
    for (uint8_t cell = 0; cell < 24u; cell += 3u) {
        tiles_orbits_launch(&o, cell, 100u, t);
    }
    tiles_orbits_set_pull(&o, true, t);
    for (int i = 0; i < 64 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_orbits_step(&o, t, &out);
        for (uint8_t k = 0; k < TILES_ORBITS_MAX_PLANETS; k++) {
            if (o.planet[k].live) {
                assert(o.planet[k].r >= 0.5f - 1e-4f && o.planet[k].r <= o.planet[k].r0 + 1.5f + 1e-4f);
            }
        }
    }
    tiles_orbits_set_pull(&o, false, t);
    double last[TILES_ORBITS_MAX_PLANETS];
    for (int k = 0; k < (int)TILES_ORBITS_MAX_PLANETS; k++) {
        last[k] = -1.0;
    }
    for (int i = 0; i < 16 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_orbits_step(&o, t, &out);
        for (uint8_t k = 0; k < TILES_ORBITS_MAX_PLANETS; k++) {
            if (o.planet[k].live && notes_on(&out, o.planet[k].home)) {
                assert(just_after_grid(t, 4.0));
                if (last[k] >= 0.0) {
                    assert(fabs(t - last[k] - (double)o.planet[k].period) < 2.0 * DT);
                }
                last[k] = t;
            }
        }
    }

    /* 5. A planet comes off by its cell; clearing keeps the sun. */
    int8_t i = tiles_orbits_planet_at(&o, 15u);
    assert(i >= 0 && ((tiles_orbits_erasable(&o) >> 15) & 1u));
    assert(tiles_orbits_remove_planet(&o, 15u) && tiles_orbits_planet_at(&o, 15u) < 0);
    assert(!tiles_orbits_remove_planet(&o, 15u));
    tiles_orbits_clear(&o, t);
    assert(!tiles_orbits_active(&o) && tiles_orbits_is_sun(&o, TILES_ORBITS_HOME_SUN));
}

static void test_cradle(void) {
    tiles_phys_out_t out;
    memset(&c, 0, sizeof(c));
    tiles_cradle_clear(&c);
    double t = 7.3;
    /* 1. Two balls lifted two rows from the left end (row 1, col 1): they
     * knock once a beat for ever. */
    tiles_phys_out_clear(&out);
    tiles_cradle_strike(&c, 7u, 100u, t, &out);
    assert(out.note_count == 0u && tiles_cradle_active(&c));
    t += 0.5;
    tiles_cradle_release(&c, 7u, t); /* let go at 7.8: first knock at 8.0 */
    double knocks[64];
    int n = 0, last_velocity = -1;
    for (int i = 0; i < 48 * 256 && n < 48; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_cradle_step(&c, t, &out);
        if (out.note_count > 0u) {
            /* the balls on the other end fly out: columns 6 and 5 */
            assert(out.note_count == 2u);
            bool right = notes_on(&out, 23u) && notes_on(&out, 22u);
            bool left = notes_on(&out, 18u) && notes_on(&out, 19u);
            assert(right != left);
            assert((n % 2 == 0) == right);
            int v = out.note_velocity[0];
            assert(last_velocity < 0 || v == last_velocity); /* no energy lost */
            last_velocity = v;
            knocks[n++] = t;
        }
    }
    assert(fabs(knocks[0] - 8.0) < 2.0 * DT);
    for (int i = 1; i < n; i++) {
        assert(fabs(knocks[i] - knocks[i - 1] - 1.0) < 2.0 * DT);
    }
    assert(n == 48 && tiles_cradle_active(&c));

    /* 2. A touch on a swinging ball catches it, no note. */
    tiles_phys_out_clear(&out);
    tiles_cradle_strike(&c, 19u, 100u, t, &out); /* column 2: one of its balls */
    assert(out.note_count == 0u && !tiles_cradle_active(&c));
    /* A resting ball is tapped. */
    tiles_phys_out_clear(&out);
    tiles_cradle_strike(&c, 21u, 90u, t, &out);
    assert(out.note_count == 1u && out.note_cell[0] == 21u && out.note_velocity[0] == 90u);

    /* 3. Pendulum wave: column c passes the bottom 8 + c times in two bars,
     * every column together on the first beat of every second bar. */
    t = 15.9;
    tiles_cradle_set_wave(&c, true, t); /* nothing was swinging: all still */
    assert(c.wave && !tiles_cradle_active(&c));
    for (uint8_t col = 0; col < 6u; col++) {
        tiles_phys_out_clear(&out);
        tiles_cradle_strike(&c, (uint8_t)(18u + col), 100u, t, &out); /* each one set going */
        assert(c.wave_on[col] && out.note_count == 1u);
    }
    int passes[6] = {0};
    int together = 0;
    for (int i = 0; i < (int)(8.0 / DT); i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_cradle_step(&c, t, &out);
        for (uint8_t k = 0; k < out.note_count; k++) {
            passes[out.note_cell[k] - 18u]++;
        }
        if (out.note_count == 6u) {
            assert(just_after_grid(t, 1.0 / 8.0)); /* a multiple of 8 beats */
            together++;
        }
    }
    for (int col = 0; col < 6; col++) {
        assert(passes[col] == 8 + col);
        assert(fabs(tiles_cradle_wave_interval((uint8_t)col) - 8.0 / (8.0 + col)) < 1e-12);
    }
    assert(together == 1);
    /* A touch stops a ball, another sets it going again. */
    tiles_phys_out_clear(&out);
    tiles_cradle_strike(&c, 23u, 100u, t, &out);
    assert(out.note_count == 0u && !c.wave_on[5]);
    tiles_cradle_strike(&c, 23u, 100u, t, &out);
    assert(out.note_count == 1u && c.wave_on[5]);
    /* Held up top: silent until let go, then louder than the rest. */
    tiles_cradle_strike(&c, 0u, 100u, t, &out); /* column 1, three rows up */
    int held_notes = 0;
    for (int i = 0; i < 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_cradle_step(&c, t, &out);
        held_notes += notes_on(&out, 18u);
    }
    assert(held_notes == 0 && c.wave_amp[0] == 3.0f);
    tiles_cradle_release(&c, 0u, t);
    int loud = 0, other = 0;
    for (int i = 0; i < 2 * 256; i++) {
        t += DT;
        tiles_phys_out_clear(&out);
        tiles_cradle_step(&c, t, &out);
        if (notes_on(&out, 18u)) {
            loud = velocity_on(&out, 18u);
        }
        if (notes_on(&out, 19u)) {
            other = velocity_on(&out, 19u);
        }
    }
    assert(loud > other && other > 0);
    /* Off again: the cradle, at rest. */
    tiles_cradle_set_wave(&c, false, t);
    assert(!c.wave && !tiles_cradle_active(&c));
    /* On while a ball swings: that swing's balls carry on as pendulums,
     * the rest hang still. */
    tiles_phys_out_clear(&out);
    tiles_cradle_strike(&c, 6u, 100u, t, &out); /* one ball, two rows up */
    tiles_cradle_release(&c, 6u, t);
    tiles_cradle_set_wave(&c, true, t + 0.1);
    assert(c.wave_on[0] && c.wave_on[5] && !c.wave_on[1] && !c.wave_on[2]);
    assert(c.wave_amp[0] == 2.0f);
    tiles_cradle_set_wave(&c, false, t + 0.2);

    /* 4. Circle + tap catches too. */
    tiles_cradle_strike(&c, 11u, 100u, t, &out); /* one ball, right end */
    tiles_cradle_release(&c, 11u, t);
    assert(tiles_cradle_erasable(&c) & (1u << 23));
    assert(tiles_cradle_erase(&c, 23u) && !tiles_cradle_active(&c));
}

int main(void) {
    test_ripples();
    test_sand();
    test_orbits();
    test_cradle();
    printf("physics_core: all tests pass\n");
    return 0;
}
