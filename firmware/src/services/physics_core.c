#include "physics_core.h"

#include <math.h>
#include <string.h>

#define PHYS_PI 3.14159265358979323846f
#define PHYS_TWO_PI 6.28318530717958647692f

/* A flash on a cell fades over this many beats. */
#define PHYS_FLASH_BEATS 0.3f

/* ---- shared helpers ---- */

void tiles_phys_out_clear(tiles_phys_out_t *out) {
    out->note_count = 0u;
    out->tap_count = 0u;
}

static float cell_x(uint8_t cell) {
    return (float)(cell % TILES_PHYS_COLS) + 0.5f;
}

static float cell_y(uint8_t cell) {
    return (float)(cell / TILES_PHYS_COLS) + 0.5f;
}

static uint8_t cell_at(float x, float y) {
    if (x < 0.0f || y < 0.0f || x >= (float)TILES_PHYS_COLS || y >= (float)TILES_PHYS_ROWS) {
        return TILES_PHYS_NO_CELL;
    }
    return (uint8_t)((uint8_t)y * TILES_PHYS_COLS + (uint8_t)x);
}

static float cell_dist(uint8_t a, uint8_t b) {
    return hypotf(cell_x(a) - cell_x(b), cell_y(a) - cell_y(b));
}

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* The first point of a grid of `per_beat` divisions at or after t. */
static double grid_at_or_after(double t, float per_beat) {
    return ceil(t * (double)per_beat - 1e-6) / (double)per_beat;
}

static uint8_t velocity_of(float level) {
    float v = clamp01(level) * 127.0f + 0.5f;
    return (uint8_t)(v < 1.0f ? 1.0f : v);
}

static void add_note_at(tiles_phys_out_t *out, uint8_t cell, int8_t degree, float level, float beats) {
    if (out == NULL || out->note_count >= TILES_PHYS_MAX_NOTES) {
        return;
    }
    if (degree < 0 && cell >= TILES_PHYS_CELLS) {
        return;
    }
    out->note_cell[out->note_count] = cell;
    out->note_degree[out->note_count] = degree;
    out->note_velocity[out->note_count] = velocity_of(level);
    out->note_beats[out->note_count] = beats;
    out->note_count++;
}

/* The cell's own note. */
static void add_note(tiles_phys_out_t *out, uint8_t cell, float level, float beats) {
    add_note_at(out, cell, TILES_PHYS_CELL_NOTE, level, beats);
}

static void add_tap(tiles_phys_out_t *out, uint8_t cell, float level) {
    if (out == NULL || cell >= TILES_PHYS_CELLS || out->tap_count >= TILES_PHYS_MAX_TAPS) {
        return;
    }
    for (uint8_t i = 0; i < out->tap_count; i++) {
        if (out->tap_cell[i] == cell) {
            if (velocity_of(level) > out->tap_strength[i]) {
                out->tap_strength[i] = velocity_of(level);
            }
            return;
        }
    }
    out->tap_cell[out->tap_count] = cell;
    out->tap_strength[out->tap_count] = velocity_of(level);
    out->tap_count++;
}

static void flash(double *flash_t, float *flash_amp, uint8_t cell, float amp, double now) {
    if (cell >= TILES_PHYS_CELLS) {
        return;
    }
    float left = flash_amp[cell] * (1.0f - (float)(now - flash_t[cell]) / PHYS_FLASH_BEATS);
    if (amp > left) {
        flash_t[cell] = now;
        flash_amp[cell] = amp;
    }
}

static float flash_level(const double *flash_t, const float *flash_amp, uint8_t cell, double now) {
    float age = (float)(now - flash_t[cell]);
    if (age < 0.0f || age >= PHYS_FLASH_BEATS) {
        return 0.0f;
    }
    return flash_amp[cell] * (1.0f - age / PHYS_FLASH_BEATS);
}

/* Light adds like light (screen blend), so overlaps never clip harshly. */
static void light(tiles_phys_frame_t frame, uint8_t cell, float r, float g, float b, float k) {
    if (cell >= TILES_PHYS_CELLS || k <= 0.0f) {
        return;
    }
    float *f = frame[cell];
    f[0] = 1.0f - (1.0f - f[0]) * (1.0f - clamp01(r * k));
    f[1] = 1.0f - (1.0f - f[1]) * (1.0f - clamp01(g * k));
    f[2] = 1.0f - (1.0f - f[2]) * (1.0f - clamp01(b * k));
}

/* Something small at (x, y): shared between the (up to four) cells around
 * it by distance, constant in power, so a moving thing glides from pad to
 * pad without a halo. */
static void dot(tiles_phys_frame_t frame, float x, float y, float r, float g, float b, float k) {
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        float wx = 1.0f - fabsf(cell_x(cell) - x), wy = 1.0f - fabsf(cell_y(cell) - y);
        if (wx > 0.0f && wy > 0.0f) {
            light(frame, cell, r, g, b, k * sqrtf(wx * wy));
        }
    }
}

static void clear_frame(tiles_phys_frame_t frame) {
    memset(frame, 0, sizeof(tiles_phys_frame_t));
}

static void render_flashes(tiles_phys_frame_t frame, const double *flash_t, const float *flash_amp, double now) {
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        light(frame, cell, 1.0f, 1.0f, 1.0f, flash_level(flash_t, flash_amp, cell, now));
    }
}

static uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x != 0u ? x : 0x9E3779B9u;
    return *s;
}

static float wrap_pi(float a) {
    a = fmodf(a + PHYS_PI, PHYS_TWO_PI);
    if (a < 0.0f) {
        a += PHYS_TWO_PI;
    }
    return a - PHYS_PI;
}

/* ================================ Ripples ================================ */

/* How far a stone's ring reaches, in pads (a soft strike about 1.5, its
 * neighbours; a full one 5, across the grid), and its level at a
 * distance: fading to nothing just past the reach. First guesses. */
#define RIPPLE_REACH_MIN 1.1f
#define RIPPLE_REACH_PER_STRENGTH 5.0f
#define RIPPLE_EDGE 0.5f
#define RIPPLE_MIN_LEVEL 0.05f /* quieter crossings don't play */
#define RIPPLE_HIT_BEATS 0.25f
#define RIPPLE_DROP_BEATS 0.5f
#define RIPPLE_FRONT_WIDTH 0.55f /* pads either side of a front that light */
#define RIPPLE_SPLASH_MIN 0.2f
#define RIPPLE_SPLASH_SHARE 0.55f
#define RIPPLE_SPLASH_AGAIN 0.6f /* chance a landing drop splashes again */
/* The pond at rest, and a stone lying in it. */
#define RIPPLE_SURFACE_LEVEL 0.05f
#define RIPPLE_STONE_LEVEL 0.3f

void tiles_ripples_clear(tiles_ripples_t *r, uint32_t seed, double now) {
    bool splash = r->splash;
    memset(r, 0, sizeof(*r));
    r->splash = splash;
    r->rng = seed != 0u ? seed : 1u;
    r->last = now;
}

static float ring_level(const tiles_ripple_ring_t *ring, float d) {
    if (d > ring->reach) {
        return 0.0f;
    }
    return ring->strength * (1.0f - d / (ring->reach + RIPPLE_EDGE));
}

static void spawn_ring(tiles_ripples_t *r, uint8_t cell, float strength, double now) {
    tiles_ripple_ring_t *slot = &r->ring[0];
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_RINGS; i++) {
        if (!r->ring[i].live) {
            slot = &r->ring[i];
            break;
        }
        if (r->ring[i].t0 < slot->t0) {
            slot = &r->ring[i]; /* full: the oldest goes */
        }
    }
    slot->live = true;
    slot->src = cell;
    slot->x = cell_x(cell);
    slot->y = cell_y(cell);
    slot->strength = clamp01(strength);
    slot->reach = fmaxf(RIPPLE_REACH_MIN, RIPPLE_REACH_PER_STRENGTH * slot->strength);
    slot->t0 = now;
}

static void add_hit(tiles_ripples_t *r, uint8_t cell, double due, float amp) {
    tiles_ripple_hit_t *free_slot = NULL;
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_HITS; i++) {
        tiles_ripple_hit_t *h = &r->hit[i];
        if (h->live && h->cell == cell && fabs(h->due - due) < 1e-6) {
            h->amp += amp; /* two fronts on one sixteenth add up */
            h->n++;
            return;
        }
        if (!h->live && free_slot == NULL) {
            free_slot = h;
        }
    }
    if (free_slot != NULL) {
        free_slot->live = true;
        free_slot->cell = cell;
        free_slot->due = due;
        free_slot->amp = amp;
        free_slot->n = 1u;
    }
}

static void throw_drops(tiles_ripples_t *r, uint8_t from, float strength, uint8_t count, double now) {
    for (uint8_t i = 0; i < count; i++) {
        uint8_t choice[TILES_PHYS_CELLS];
        uint8_t n = 0u;
        for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
            float d = cell_dist(from, cell);
            if (d >= 1.4f && d <= 2.9f) {
                choice[n++] = cell;
            }
        }
        if (n == 0u) {
            return;
        }
        for (uint8_t k = 0; k < TILES_RIPPLES_MAX_DROPS; k++) {
            tiles_ripple_drop_t *d = &r->drop[k];
            if (!d->live) {
                d->live = true;
                d->from = from;
                d->to = choice[rng_next(&r->rng) % n];
                d->strength = strength * RIPPLE_SPLASH_SHARE;
                d->t0 = now;
                d->at = grid_at_or_after(now + 0.5 * (double)(i + 1u), 2.0f);
                break;
            }
        }
    }
}

/* A stone hits the water at `t`: its note, its ring, maybe a splash. */
static void drop_stone(tiles_ripples_t *r, uint8_t cell, float strength, double t, tiles_phys_out_t *out) {
    add_note(out, cell, strength, RIPPLE_DROP_BEATS);
    add_tap(out, cell, strength);
    flash(r->flash_t, r->flash_amp, cell, strength, t);
    spawn_ring(r, cell, strength, t);
    if (r->splash && strength > RIPPLE_SPLASH_MIN) {
        throw_drops(r, cell, strength, 2u, t);
    }
}

static tiles_ripple_stone_t *stone_at(tiles_ripples_t *r, uint8_t cell) {
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        if (r->stone[i].live && r->stone[i].cell == cell) {
            return &r->stone[i];
        }
    }
    return NULL;
}

bool tiles_ripples_has_stone(const tiles_ripples_t *r, uint8_t cell) {
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        if (r->stone[i].live && r->stone[i].cell == cell) {
            return true;
        }
    }
    return false;
}

bool tiles_ripples_strike(tiles_ripples_t *r, uint8_t cell, float strength, double now, tiles_phys_out_t *out) {
    if (cell >= TILES_PHYS_CELLS) {
        return false;
    }
    tiles_ripple_stone_t *stone = stone_at(r, cell);
    if (stone != NULL) {
        stone->live = false; /* lifted out */
        r->held[cell] = false;
        add_tap(out, cell, 0.3f);
        return false;
    }
    strength = clamp01(strength);
    drop_stone(r, cell, strength, now, out);
    stone = &r->stone[0];
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        if (!r->stone[i].live) {
            stone = &r->stone[i];
            break;
        }
        if (r->stone[i].strength < stone->strength) {
            stone = &r->stone[i]; /* full: the smallest goes */
        }
    }
    /* It drops again at this sixteenth of the bar, every bar. */
    double place = fmod(floor(now * 4.0 + 0.5) / 4.0, TILES_RIPPLES_LOOP_BEATS);
    double next = floor(now / TILES_RIPPLES_LOOP_BEATS) * TILES_RIPPLES_LOOP_BEATS + place;
    while (next <= now + 0.05) {
        next += TILES_RIPPLES_LOOP_BEATS;
    }
    *stone = (tiles_ripple_stone_t){true, cell, strength, strength, next};
    r->held[cell] = true;
    return true;
}

void tiles_ripples_set_pressure(tiles_ripples_t *r, uint8_t cell, float pressure) {
    tiles_ripple_stone_t *stone = cell < TILES_PHYS_CELLS && r->held[cell] ? stone_at(r, cell) : NULL;
    if (stone != NULL) {
        stone->strength = fmaxf(stone->base, clamp01(pressure));
    }
}

void tiles_ripples_release(tiles_ripples_t *r, uint8_t cell) {
    if (cell < TILES_PHYS_CELLS) {
        r->held[cell] = false;
    }
}

void tiles_ripples_step(tiles_ripples_t *r, double now, tiles_phys_out_t *out) {
    if (now < r->last) {
        r->last = now;
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        tiles_ripple_stone_t *s = &r->stone[i];
        if (s->live && now >= s->next) {
            drop_stone(r, s->cell, s->strength, s->next, out);
            while (s->next <= now) {
                s->next += TILES_RIPPLES_LOOP_BEATS;
            }
        }
    }
    /* Fronts crossing cells since the last step: each plays on the
     * sixteenth at or after its crossing. */
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_RINGS; i++) {
        tiles_ripple_ring_t *ring = &r->ring[i];
        if (!ring->live) {
            continue;
        }
        float before = (float)(r->last - ring->t0) * TILES_RIPPLES_SPEED;
        float radius = (float)(now - ring->t0) * TILES_RIPPLES_SPEED;
        for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
            if (cell == ring->src) {
                continue;
            }
            float d = hypotf(cell_x(cell) - ring->x, cell_y(cell) - ring->y);
            if (d > before && d <= radius) {
                float level = ring_level(ring, d);
                if (level >= RIPPLE_MIN_LEVEL) {
                    add_hit(r, cell, grid_at_or_after(ring->t0 + (double)(d / TILES_RIPPLES_SPEED), 4.0f), level);
                }
            }
        }
        if (radius > ring->reach + RIPPLE_EDGE + RIPPLE_FRONT_WIDTH) {
            ring->live = false;
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_HITS; i++) {
        tiles_ripple_hit_t *h = &r->hit[i];
        if (h->live && h->due <= now) {
            h->live = false;
            float level = clamp01(h->amp);
            add_note(out, h->cell, level, RIPPLE_HIT_BEATS);
            add_tap(out, h->cell, level);
            flash(r->flash_t, r->flash_amp, h->cell, clamp01(level * (h->n > 1u ? 1.3f : 1.0f)), now);
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_DROPS; i++) {
        tiles_ripple_drop_t *d = &r->drop[i];
        if (d->live && d->at <= now) {
            d->live = false;
            add_note(out, d->to, d->strength, RIPPLE_DROP_BEATS);
            add_tap(out, d->to, d->strength);
            flash(r->flash_t, r->flash_amp, d->to, d->strength, now);
            spawn_ring(r, d->to, d->strength, d->at);
            if (r->splash && d->strength > 0.15f &&
                (float)(rng_next(&r->rng) % 1000u) < RIPPLE_SPLASH_AGAIN * 1000.0f) {
                throw_drops(r, d->to, d->strength, 1u, now);
            }
        }
    }
    r->last = now;
}

bool tiles_ripples_erase(tiles_ripples_t *r, uint8_t cell) {
    bool any = false;
    tiles_ripple_stone_t *stone = stone_at(r, cell);
    if (stone != NULL) {
        stone->live = false;
        any = true;
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_RINGS; i++) {
        if (r->ring[i].live && r->ring[i].src == cell) {
            r->ring[i].live = false;
            any = true;
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_DROPS; i++) {
        if (r->drop[i].live && (r->drop[i].from == cell || r->drop[i].to == cell)) {
            r->drop[i].live = false;
            any = true;
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_HITS; i++) {
        if (r->hit[i].live && r->hit[i].cell == cell) {
            r->hit[i].live = false;
        }
    }
    return any;
}

uint32_t tiles_ripples_erasable(const tiles_ripples_t *r) {
    uint32_t bits = 0u;
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        if (r->stone[i].live) {
            bits |= 1u << r->stone[i].cell;
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_DROPS; i++) {
        if (r->drop[i].live) {
            bits |= 1u << r->drop[i].to;
        }
    }
    return bits;
}

void tiles_ripples_render(const tiles_ripples_t *r, double now, tiles_phys_frame_t frame) {
    clear_frame(frame);
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        light(frame, cell, TILES_RIPPLES_COLOR_R, TILES_RIPPLES_COLOR_G, TILES_RIPPLES_COLOR_B, RIPPLE_SURFACE_LEVEL);
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        const tiles_ripple_stone_t *s = &r->stone[i];
        if (s->live) {
            light(frame, s->cell, TILES_RIPPLES_COLOR_R, TILES_RIPPLES_COLOR_G, TILES_RIPPLES_COLOR_B,
                  RIPPLE_STONE_LEVEL + 0.3f * s->strength);
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_RINGS; i++) {
        const tiles_ripple_ring_t *ring = &r->ring[i];
        if (!ring->live) {
            continue;
        }
        float radius = (float)(now - ring->t0) * TILES_RIPPLES_SPEED;
        if (radius < 0.0f) {
            continue;
        }
        for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
            float d = hypotf(cell_x(cell) - ring->x, cell_y(cell) - ring->y);
            float w = 1.0f - fabsf(d - radius) / RIPPLE_FRONT_WIDTH;
            float level = d < 0.1f ? ring->strength : ring_level(ring, d);
            if (w > 0.0f && level > 0.0f) {
                light(frame, cell, TILES_RIPPLES_COLOR_R, TILES_RIPPLES_COLOR_G, TILES_RIPPLES_COLOR_B,
                      w * w * fmaxf(level, 0.2f));
            }
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_DROPS; i++) {
        const tiles_ripple_drop_t *d = &r->drop[i];
        if (!d->live || d->at <= d->t0) {
            continue;
        }
        float k = clamp01((float)((now - d->t0) / (d->at - d->t0)));
        float x = cell_x(d->from) + (cell_x(d->to) - cell_x(d->from)) * k;
        float y = cell_y(d->from) + (cell_y(d->to) - cell_y(d->from)) * k - sinf(PHYS_PI * k) * 0.6f;
        dot(frame, x, y, 0.75f, 0.9f, 1.0f, 0.8f);
    }
    render_flashes(frame, r->flash_t, r->flash_amp, now);
}

bool tiles_ripples_active(const tiles_ripples_t *r) {
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_STONES; i++) {
        if (r->stone[i].live) {
            return true;
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_RINGS; i++) {
        if (r->ring[i].live) {
            return true;
        }
    }
    for (uint8_t i = 0; i < TILES_RIPPLES_MAX_DROPS; i++) {
        if (r->drop[i].live) {
            return true;
        }
    }
    return false;
}

/* ================================== Sand ================================= */

#define SAND_GRAVITY 20.0f    /* pads/s^2: top row to floor in about 0.6 s */
#define SAND_SLIDE_S 0.055f   /* an avalanche's grains, one after another */
#define SAND_LEAK_BEATS 8.0   /* the hourglass: a grain off every pile */
#define SAND_PREVIEW_LEVEL 0.35f
#define SAND_PREVIEW_BEATS 0.25f
#define SAND_STEP_LEVEL 0.72f
#define SAND_DOWNBEAT_LEVEL 0.88f /* column 1, on the beat */
#define SAND_MAX_CASCADE 30u
/* Light: the floor where sand will land, the pile, its surface (the note),
 * the playhead's column (white, so it reads apart from the sand). */
#define SAND_FLOOR_LEVEL 0.05f
#define SAND_PILE_LEVEL 0.25f
#define SAND_SURFACE_LEVEL 0.55f
#define SAND_PLAYHEAD_LEVEL 0.12f

void tiles_sand_clear(tiles_sand_t *s, double now) {
    int8_t gdir = s->gdir != 0 ? s->gdir : 1;
    memset(s, 0, sizeof(*s));
    s->gdir = gdir;
    s->playhead = TILES_PHYS_NO_CELL;
    s->step_next = grid_at_or_after(now, (float)TILES_SAND_STEPS_PER_BEAT);
    s->leak_next = floor(now / SAND_LEAK_BEATS) * SAND_LEAK_BEATS + SAND_LEAK_BEATS;
    s->last = now;
}

uint8_t tiles_sand_division(float pressure) {
    if (pressure < 0.25f) {
        return 1u;
    }
    if (pressure < 0.5f) {
        return 2u;
    }
    return pressure < 0.75f ? 4u : 8u;
}

/* How high the sand's surface stands, grains from the bottom edge, minus
 * one: the column's scale degree. Upright, a pile of h is degree h - 1;
 * hanging from the top, its surface sits 12 - h up. */
int8_t tiles_sand_degree(const tiles_sand_t *s, uint8_t col) {
    if (col >= TILES_PHYS_COLS || s->h[col] == 0u) {
        return -1;
    }
    return s->gdir > 0 ? (int8_t)(s->h[col] - 1u) : (int8_t)(TILES_SAND_MAX_HEIGHT - s->h[col]);
}

/* The y of a column's pile surface, in rows (0 = top edge). */
static float pile_top(const tiles_sand_t *s, uint8_t col) {
    float h = (float)s->h[col] / (float)TILES_SAND_PER_CELL;
    return s->gdir > 0 ? (float)TILES_PHYS_ROWS - h : h;
}

/* The cell of a column's grain number `level` (0 = on the floor). */
static uint8_t grain_cell(const tiles_sand_t *s, uint8_t col, uint8_t level) {
    float y = ((float)level + 0.5f) / (float)TILES_SAND_PER_CELL;
    if (s->gdir > 0) {
        y = (float)TILES_PHYS_ROWS - y;
    }
    return cell_at((float)col + 0.5f, y);
}

static uint8_t surface_cell(const tiles_sand_t *s, uint8_t col) {
    return s->h[col] == 0u ? TILES_PHYS_NO_CELL : grain_cell(s, col, (uint8_t)(s->h[col] - 1u));
}

static void spawn_grain(tiles_sand_t *s, float x, float y, bool settles) {
    for (uint8_t i = 0; i < TILES_SAND_MAX_GRAINS; i++) {
        if (!s->grain[i].live) {
            s->grain[i] = (tiles_sand_grain_t){true, settles, x, y, 0.0f};
            return;
        }
    }
}

static void queue_slide(tiles_sand_t *s, uint8_t from, uint8_t to, uint8_t depth, double at) {
    for (uint8_t i = 0; i < TILES_SAND_MAX_SLIDES; i++) {
        if (!s->slide[i].live) {
            s->slide[i] = (tiles_sand_slide_t){true, from, to, depth, at};
            return;
        }
    }
}

static void topple(tiles_sand_t *s, uint8_t col, uint8_t depth, double now, float sec_per_beat) {
    for (int n = (int)col - 1; n <= (int)col + 1; n += 2) {
        if (n < 0 || n >= (int)TILES_PHYS_COLS) {
            continue;
        }
        if (s->h[col] >= s->h[n] + TILES_SAND_PER_CELL) {
            queue_slide(s, col, (uint8_t)n, (uint8_t)(depth + 1u),
                        now + (double)(SAND_SLIDE_S * (float)(depth + 1u) / sec_per_beat));
        }
    }
}

void tiles_sand_strike(tiles_sand_t *s, uint8_t cell, double now) {
    if (cell >= TILES_PHYS_CELLS) {
        return;
    }
    spawn_grain(s, cell_x(cell), cell_y(cell), false);
    s->held[cell] = true;
    s->pressure[cell] = 0.0f;
    s->pour_next[cell] = grid_at_or_after(now + 1e-4, 1.0f);
    flash(s->flash_t, s->flash_amp, cell, 0.5f, now);
}

void tiles_sand_set_pressure(tiles_sand_t *s, uint8_t cell, float pressure) {
    if (cell < TILES_PHYS_CELLS) {
        s->pressure[cell] = clamp01(pressure);
    }
}

void tiles_sand_release(tiles_sand_t *s, uint8_t cell) {
    if (cell < TILES_PHYS_CELLS) {
        s->held[cell] = false;
    }
}

bool tiles_sand_erase(tiles_sand_t *s, uint8_t cell) {
    if (cell >= TILES_PHYS_CELLS) {
        return false;
    }
    uint8_t col = (uint8_t)(cell % TILES_PHYS_COLS);
    bool any = s->h[col] > 0u;
    s->h[col] = 0u;
    for (uint8_t i = 0; i < TILES_SAND_MAX_GRAINS; i++) {
        if (s->grain[i].live && (uint8_t)s->grain[i].x == col) {
            s->grain[i].live = false;
            any = true;
        }
    }
    for (uint8_t i = 0; i < TILES_SAND_MAX_SLIDES; i++) {
        if (s->slide[i].live && (s->slide[i].from == col || s->slide[i].to == col)) {
            s->slide[i].live = false;
        }
    }
    return any;
}

void tiles_sand_flip(tiles_sand_t *s, bool up) {
    int8_t gdir = up ? -1 : 1;
    if (gdir == s->gdir) {
        return;
    }
    /* Every pile lets go where it lies and falls the other way, landing
     * column by column without avalanches, so the melody keeps its notes
     * and only turns over. */
    for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
        for (uint8_t level = 0; level < s->h[col]; level++) {
            float y = ((float)level + 0.5f) / (float)TILES_SAND_PER_CELL;
            spawn_grain(s, (float)col + 0.5f, s->gdir > 0 ? (float)TILES_PHYS_ROWS - y : y, true);
        }
        s->h[col] = 0u;
    }
    for (uint8_t i = 0; i < TILES_SAND_MAX_SLIDES; i++) {
        s->slide[i].live = false;
    }
    s->gdir = gdir;
}

void tiles_sand_step(tiles_sand_t *s, double now, float sec_per_beat, tiles_phys_out_t *out) {
    double dt_beats = now - s->last;
    if (dt_beats < 0.0) {
        dt_beats = 0.0;
    }
    s->last = now;
    float dt = fminf((float)dt_beats * sec_per_beat, 0.05f);

    /* Pouring: a grain on every division, the press setting the division. */
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        if (!s->held[cell]) {
            continue;
        }
        float div = (float)tiles_sand_division(s->pressure[cell]);
        double next = grid_at_or_after(now + 1e-4, div);
        if (s->pour_next[cell] > next) {
            s->pour_next[cell] = next; /* pressed harder: the faster grid */
        }
        if (now >= s->pour_next[cell]) {
            spawn_grain(s, cell_x(cell), cell_y(cell), false);
            add_tap(out, cell, 0.3f);
            s->pour_next[cell] = grid_at_or_after(now + 1e-4, div);
        }
    }

    /* Falling grains; a landing sounds the column's new note, softly. */
    for (uint8_t i = 0; i < TILES_SAND_MAX_GRAINS; i++) {
        tiles_sand_grain_t *g = &s->grain[i];
        if (!g->live) {
            continue;
        }
        g->vy += SAND_GRAVITY * (float)s->gdir * dt;
        g->y += g->vy * dt;
        uint8_t col = (uint8_t)fminf(fmaxf(g->x, 0.0f), (float)TILES_PHYS_COLS - 0.01f);
        float top = pile_top(s, col);
        if (s->gdir > 0 ? g->y < top - 0.05f : g->y > top + 0.05f) {
            continue;
        }
        g->live = false;
        if (s->h[col] >= TILES_SAND_MAX_HEIGHT) {
            topple(s, col, 0u, now, sec_per_beat); /* full: it rolls off */
            continue;
        }
        s->h[col]++;
        uint8_t cell = surface_cell(s, col);
        add_note_at(out, cell, tiles_sand_degree(s, col), SAND_PREVIEW_LEVEL, SAND_PREVIEW_BEATS);
        add_tap(out, cell, 0.4f);
        flash(s->flash_t, s->flash_amp, cell, 0.6f, now);
        if (!g->settles) {
            topple(s, col, 0u, now, sec_per_beat);
        }
    }

    /* Avalanches, a grain at a time, quietly: they reshape the melody. */
    for (uint8_t pass = 0; pass < 4u; pass++) {
        bool moved = false;
        for (uint8_t i = 0; i < TILES_SAND_MAX_SLIDES; i++) {
            tiles_sand_slide_t sl = s->slide[i];
            if (!sl.live || sl.at > now) {
                continue;
            }
            s->slide[i].live = false;
            if (s->h[sl.from] < s->h[sl.to] + 2u || s->h[sl.to] >= TILES_SAND_MAX_HEIGHT) {
                continue;
            }
            s->h[sl.from]--;
            s->h[sl.to]++;
            uint8_t cell = surface_cell(s, sl.to);
            add_tap(out, cell, 0.3f);
            flash(s->flash_t, s->flash_amp, cell, 0.6f, now);
            if (sl.depth < SAND_MAX_CASCADE) {
                topple(s, sl.to, sl.depth, now, sec_per_beat);
                topple(s, sl.from, sl.depth, now, sec_per_beat);
            }
            moved = true;
        }
        if (!moved) {
            break;
        }
    }

    if (now >= s->leak_next) {
        while (s->leak_next <= now) {
            s->leak_next += SAND_LEAK_BEATS;
        }
        for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
            if (s->h[col] > 0u) {
                s->h[col]--;
            }
        }
    }

    /* The playhead: a column a triplet eighth, column 1 on the beat. */
    while (now >= s->step_next) {
        long long k = llround(s->step_next * (double)TILES_SAND_STEPS_PER_BEAT);
        uint8_t col = (uint8_t)(k % (long long)TILES_PHYS_COLS);
        s->playhead = col;
        int8_t degree = tiles_sand_degree(s, col);
        if (degree >= 0) {
            uint8_t cell = surface_cell(s, col);
            float level = col == 0u ? SAND_DOWNBEAT_LEVEL : SAND_STEP_LEVEL;
            add_note_at(out, cell, degree, level, 1.0f / (float)TILES_SAND_STEPS_PER_BEAT);
            add_tap(out, cell, 0.35f);
            flash(s->flash_t, s->flash_amp, cell, 0.9f, s->step_next);
        }
        s->step_next = (double)(k + 1) / (double)TILES_SAND_STEPS_PER_BEAT;
    }
}

uint32_t tiles_sand_erasable(const tiles_sand_t *s) {
    uint32_t bits = 0u;
    for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
        uint8_t cell = surface_cell(s, col);
        if (cell < TILES_PHYS_CELLS) {
            bits |= 1u << cell;
        }
    }
    return bits;
}

void tiles_sand_render(const tiles_sand_t *s, double now, tiles_phys_frame_t frame) {
    clear_frame(frame);
    uint8_t floor_row = s->gdir > 0 ? (uint8_t)(TILES_PHYS_ROWS - 1u) : 0u;
    for (uint8_t cell = 0; cell < TILES_PHYS_CELLS; cell++) {
        uint8_t col = (uint8_t)(cell % TILES_PHYS_COLS), row = (uint8_t)(cell / TILES_PHYS_COLS);
        uint8_t from_floor = s->gdir > 0 ? (uint8_t)(TILES_PHYS_ROWS - 1u - row) : row;
        float fill = clamp01((float)s->h[col] / (float)TILES_SAND_PER_CELL - (float)from_floor);
        float level = SAND_PILE_LEVEL * fill;
        if (cell == surface_cell(s, col)) {
            level = SAND_SURFACE_LEVEL; /* the grain that sets the note */
        } else if (s->h[col] == 0u && row == floor_row) {
            level = SAND_FLOOR_LEVEL;
        }
        light(frame, cell, TILES_SAND_COLOR_R, TILES_SAND_COLOR_G, TILES_SAND_COLOR_B, level);
        if (col == s->playhead) {
            light(frame, cell, 1.0f, 1.0f, 1.0f, SAND_PLAYHEAD_LEVEL);
        }
        if (s->held[cell]) {
            light(frame, cell, TILES_SAND_COLOR_R, TILES_SAND_COLOR_G, TILES_SAND_COLOR_B,
                  0.3f + 0.4f * s->pressure[cell]);
        }
    }
    for (uint8_t i = 0; i < TILES_SAND_MAX_GRAINS; i++) {
        if (s->grain[i].live) {
            dot(frame, s->grain[i].x, s->grain[i].y, 0.95f, 0.7f, 0.35f, 0.8f);
        }
    }
    render_flashes(frame, s->flash_t, s->flash_amp, now);
}

bool tiles_sand_active(const tiles_sand_t *s) {
    for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
        if (s->h[col] > 0u) {
            return true;
        }
    }
    for (uint8_t i = 0; i < TILES_SAND_MAX_GRAINS; i++) {
        if (s->grain[i].live) {
            return true;
        }
    }
    return false;
}

/* ================================= Orbits ================================ */

/* Periods a planet may take, in beats (Kepler's, rounded to the nearest of
 * these by ratio). */
static const float ORBIT_PERIODS[] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f};
#define ORBIT_PERIOD_COUNT (sizeof(ORBIT_PERIODS) / sizeof(ORBIT_PERIODS[0]))
#define ORBIT_NOTE_BEATS 0.5f
/* Pull: how hard planets pull on each other, the spring back to their own
 * orbit, and the damping. First guesses: enough to bunch and swap them,
 * and the radius is held within 1.5 pads of the orbit, so none flies off
 * (the lab's free gravity threw 3 of 5 out within 16 beats). */
#define ORBIT_PULL 0.8f
#define ORBIT_SPRING 4.0f
#define ORBIT_DAMP 1.2f
#define ORBIT_SOFTEN 0.3f
#define ORBIT_PASS_CHORD 0.9f /* planets closer than this play together */
#define ORBIT_GLIDE 4.0f      /* the radius's way back after Pull, per beat */
/* Light: a planet's own cell (where it plays), the planet, the sun. */
#define ORBIT_HOME_LEVEL 0.35f
#define ORBIT_PLANET_LEVEL 1.0f
#define ORBIT_SUN_LEVEL 0.9f
#define ORBIT_CORONA_LEVEL 0.12f

static const float ORBIT_HUES[6][3] = {
    {0.7f, 0.85f, 1.0f}, /* ice */
    {0.2f, 0.9f, 1.0f},  /* cyan */
    {1.0f, 0.35f, 0.8f}, /* pink */
    {0.4f, 1.0f, 0.7f},  /* mint */
    {0.75f, 0.5f, 1.0f}, /* lavender */
    {1.0f, 0.5f, 0.3f},  /* coral */
};
#define ORBIT_SUN_R 1.0f
#define ORBIT_SUN_G 0.72f
#define ORBIT_SUN_B 0.3f

void tiles_orbits_clear(tiles_orbits_t *o, double now) {
    bool pull = o->pull, running = o->running;
    double offset = o->offset, paused_t = o->paused_t, paused_at = o->paused_at, resume_at = o->resume_at;
    memset(o, 0, sizeof(*o));
    o->pull = pull;
    o->running = running;
    o->offset = offset;
    o->paused_t = paused_t;
    o->paused_at = paused_at;
    o->resume_at = resume_at;
    o->sun[0].live = true;
    o->sun[0].cell = TILES_ORBITS_HOME_SUN;
    o->last_t = tiles_orbits_time(o, now);
}

double tiles_orbits_time(const tiles_orbits_t *o, double now) {
    if (!o->running || now < o->resume_at) {
        return o->paused_t;
    }
    return now - o->offset;
}

float tiles_orbits_period(float radius) {
    float kepler = powf(radius, 1.5f);
    float best = ORBIT_PERIODS[0];
    for (uint8_t i = 1; i < ORBIT_PERIOD_COUNT; i++) {
        if (fabsf(logf(ORBIT_PERIODS[i] / kepler)) < fabsf(logf(best / kepler))) {
            best = ORBIT_PERIODS[i];
        }
    }
    return best;
}

bool tiles_orbits_is_sun(const tiles_orbits_t *o, uint8_t cell) {
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_SUNS; i++) {
        if (o->sun[i].live && o->sun[i].cell == cell) {
            return true;
        }
    }
    return false;
}

uint8_t tiles_orbits_sun_count(const tiles_orbits_t *o) {
    uint8_t n = 0u;
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_SUNS; i++) {
        n = (uint8_t)(n + (o->sun[i].live ? 1u : 0u));
    }
    return n;
}

int8_t tiles_orbits_planet_at(const tiles_orbits_t *o, uint8_t cell) {
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        if (o->planet[i].live && o->planet[i].home == cell) {
            return (int8_t)i;
        }
    }
    return -1;
}

/* A planet's angle at orbit time t on its clockwork orbit. */
static float clock_angle(const tiles_orbit_planet_t *p, double t) {
    double turns = (t - p->t_ref) / (double)p->period;
    return p->home_angle + PHYS_TWO_PI * (float)(turns - floor(turns));
}

static void planet_xy_at(const tiles_orbits_t *o, const tiles_orbit_planet_t *p, double t, float *x, float *y) {
    uint8_t sun = o->sun[p->sun].cell;
    float a = o->pull ? p->theta : clock_angle(p, t);
    float r = o->pull ? p->r : p->r0 + p->r_off;
    *x = cell_x(sun) + r * cosf(a);
    *y = cell_y(sun) + r * sinf(a);
}

void tiles_orbits_planet_xy(const tiles_orbits_t *o, uint8_t i, double now, float *x, float *y) {
    planet_xy_at(o, &o->planet[i], tiles_orbits_time(o, now), x, y);
}

static void start_free(tiles_orbit_planet_t *p, double t) {
    p->theta = clock_angle(p, t);
    p->omega = PHYS_TWO_PI / p->period;
    p->r = p->r0 + p->r_off;
    p->vr = 0.0f;
    p->prev_rel = wrap_pi(p->theta - p->home_angle);
}

/* A planet's orbit around its sun from its own cell. */
static void fit_orbit(tiles_orbits_t *o, tiles_orbit_planet_t *p) {
    uint8_t sun = o->sun[p->sun].cell;
    p->r0 = cell_dist(sun, p->home);
    p->home_angle = atan2f(cell_y(p->home) - cell_y(sun), cell_x(p->home) - cell_x(sun));
    p->period = tiles_orbits_period(p->r0);
}

static int8_t nearest_sun(const tiles_orbits_t *o, uint8_t cell) {
    int8_t best = -1;
    float best_d = 1e9f;
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_SUNS; i++) {
        if (o->sun[i].live) {
            float d = cell_dist(o->sun[i].cell, cell);
            if (d < best_d) {
                best_d = d;
                best = (int8_t)i;
            }
        }
    }
    return best;
}

void tiles_orbits_launch(tiles_orbits_t *o, uint8_t cell, uint8_t velocity, double struck_at) {
    if (cell >= TILES_PHYS_CELLS || tiles_orbits_is_sun(o, cell) || tiles_orbits_planet_at(o, cell) >= 0) {
        return;
    }
    if (tiles_orbits_sun_count(o) == 0u) {
        o->sun[0].live = true;
        o->sun[0].cell = TILES_ORBITS_HOME_SUN;
        if (cell == TILES_ORBITS_HOME_SUN) {
            return;
        }
    }
    tiles_orbit_planet_t *slot = NULL;
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        if (!o->planet[i].live) {
            slot = &o->planet[i];
            break;
        }
    }
    if (slot == NULL) {
        slot = &o->planet[0]; /* full: the oldest goes */
        for (uint8_t i = 1; i < TILES_ORBITS_MAX_PLANETS; i++) {
            if (o->planet[i].t_ref < slot->t_ref) {
                slot = &o->planet[i];
            }
        }
    }
    double t = tiles_orbits_time(o, struck_at);
    memset(slot, 0, sizeof(*slot));
    slot->live = true;
    slot->home = cell;
    slot->sun = (uint8_t)nearest_sun(o, cell);
    slot->velocity = velocity;
    slot->hue = (uint8_t)(o->next_hue++ % 6u);
    fit_orbit(o, slot);
    slot->t_ref = floor(t * 4.0 + 0.5) / 4.0; /* home on the nearest sixteenth */
    slot->next_due = slot->t_ref + (double)slot->period;
    if (o->pull) {
        start_free(slot, t);
    }
}

bool tiles_orbits_remove_planet(tiles_orbits_t *o, uint8_t cell) {
    int8_t i = tiles_orbits_planet_at(o, cell);
    if (i < 0) {
        return false;
    }
    o->planet[i].live = false;
    return true;
}

void tiles_orbits_move_sun(tiles_orbits_t *o, uint8_t cell, double now) {
    if (cell >= TILES_PHYS_CELLS) {
        return;
    }
    double t = tiles_orbits_time(o, now);
    o->sun[0].live = true;
    o->sun[0].cell = cell;
    for (uint8_t i = 1; i < TILES_ORBITS_MAX_SUNS; i++) {
        o->sun[i].live = false;
    }
    double next = grid_at_or_after(t + 1e-6, 4.0f);
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        tiles_orbit_planet_t *p = &o->planet[i];
        if (!p->live) {
            continue;
        }
        if (p->home == cell) {
            p->live = false; /* the sun landed on it */
            continue;
        }
        p->sun = 0u;
        fit_orbit(o, p);
        p->r_off = 0.0f;
        p->next_due = next;
        p->t_ref = next - (double)p->period;
        if (o->pull) {
            start_free(p, t);
        }
    }
}

void tiles_orbits_set_pull(tiles_orbits_t *o, bool on, double now) {
    if (on == o->pull) {
        return;
    }
    double t = tiles_orbits_time(o, now);
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        tiles_orbit_planet_t *p = &o->planet[i];
        if (!p->live) {
            continue;
        }
        if (on) {
            start_free(p, t);
        } else {
            /* Back into time: the next pass on the nearest sixteenth, from
             * where it is, and the radius gliding home. */
            float rem = fmodf(p->home_angle - p->theta, PHYS_TWO_PI);
            if (rem < 0.0f) {
                rem += PHYS_TWO_PI;
            }
            double due = floor((t + (double)(rem / PHYS_TWO_PI * p->period)) * 4.0 + 0.5) / 4.0;
            double first = ceil(t * 4.0 - 1e-6) / 4.0;
            p->next_due = due > first ? due : first;
            p->t_ref = p->next_due - (double)p->period;
            p->r_off = p->r - p->r0;
        }
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        for (uint8_t j = 0; j < TILES_ORBITS_MAX_PLANETS; j++) {
            o->pair_d[i][j] = 1e9f;
            o->pair_dir[i][j] = 0;
        }
    }
    o->pull = on;
}

void tiles_orbits_stop(tiles_orbits_t *o, double now) {
    if (!o->running) {
        return;
    }
    o->paused_t = tiles_orbits_time(o, now);
    o->paused_at = now;
    o->running = false;
}

void tiles_orbits_play(tiles_orbits_t *o, double now) {
    if (o->running) {
        return;
    }
    /* Carry on a whole number of beats after the stop, so the orbits keep
     * the beat grid they were on. */
    double wait = ceil(now - o->paused_at - 1e-6);
    o->resume_at = o->paused_at + (wait > 0.0 ? wait : 0.0);
    o->offset = o->resume_at - o->paused_t;
    o->running = true;
}

bool tiles_orbits_is_running(const tiles_orbits_t *o) {
    return o->running;
}

static void orbit_note(tiles_orbits_t *o, const tiles_orbit_planet_t *p, double now, tiles_phys_out_t *out) {
    add_note(out, p->home, (float)p->velocity / 127.0f, ORBIT_NOTE_BEATS);
    add_tap(out, p->home, (float)p->velocity / 127.0f);
    flash(o->flash_t, o->flash_amp, p->home, 0.9f, now);
}

static void pull_step(tiles_orbits_t *o, float dt) {
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        tiles_orbit_planet_t *p = &o->planet[i];
        if (!p->live) {
            continue;
        }
        float px = p->r * cosf(p->theta), py = p->r * sinf(p->theta);
        float fx = 0.0f, fy = 0.0f;
        for (uint8_t j = 0; j < TILES_ORBITS_MAX_PLANETS; j++) {
            const tiles_orbit_planet_t *q = &o->planet[j];
            if (j == i || !q->live || q->sun != p->sun) {
                continue;
            }
            float dx = q->r * cosf(q->theta) - px, dy = q->r * sinf(q->theta) - py;
            float d2 = dx * dx + dy * dy + ORBIT_SOFTEN;
            float inv = ORBIT_PULL / (d2 * sqrtf(d2));
            fx += dx * inv;
            fy += dy * inv;
        }
        float c = cosf(p->theta), s = sinf(p->theta);
        float radial = fx * c + fy * s, tangential = -fx * s + fy * c;
        float w0 = PHYS_TWO_PI / p->period;
        p->vr += (radial - ORBIT_SPRING * (p->r - p->r0) - ORBIT_DAMP * p->vr) * dt;
        p->omega += (tangential / fmaxf(p->r, 0.5f) - ORBIT_DAMP * (p->omega - w0)) * dt;
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        tiles_orbit_planet_t *p = &o->planet[i];
        if (p->live) {
            p->theta = wrap_pi(p->theta + p->omega * dt);
            p->r = fminf(fmaxf(p->r + p->vr * dt, 0.5f), p->r0 + 1.5f);
        }
    }
}

void tiles_orbits_step(tiles_orbits_t *o, double now, tiles_phys_out_t *out) {
    double t = tiles_orbits_time(o, now);
    double dt = t - o->last_t;
    if (dt < 0.0) {
        dt = 0.0;
    }
    o->last_t = t;
    if (!o->pull) {
        for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
            tiles_orbit_planet_t *p = &o->planet[i];
            if (!p->live) {
                continue;
            }
            p->r_off *= expf(-ORBIT_GLIDE * (float)dt);
            if (t >= p->next_due) {
                orbit_note(o, p, now, out);
                while (p->next_due <= t) {
                    p->next_due += (double)p->period;
                }
            }
        }
        return;
    }
    uint8_t substeps = (uint8_t)fminf(ceilf((float)dt / 0.02f), 50.0f);
    for (uint8_t k = 0; k < substeps; k++) {
        pull_step(o, (float)dt / (float)substeps);
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        tiles_orbit_planet_t *p = &o->planet[i];
        if (!p->live || dt <= 0.0) {
            continue;
        }
        float rel = wrap_pi(p->theta - p->home_angle);
        if (p->prev_rel < 0.0f && rel >= 0.0f && rel - p->prev_rel < PHYS_PI) {
            orbit_note(o, p, now, out); /* back round past its cell */
        }
        p->prev_rel = rel;
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        for (uint8_t j = (uint8_t)(i + 1u); j < TILES_ORBITS_MAX_PLANETS; j++) {
            const tiles_orbit_planet_t *a = &o->planet[i], *b = &o->planet[j];
            if (!a->live || !b->live || dt <= 0.0) {
                continue;
            }
            float ax, ay, bx, by;
            planet_xy_at(o, a, t, &ax, &ay);
            planet_xy_at(o, b, t, &bx, &by);
            float d = hypotf(ax - bx, ay - by);
            int8_t dir = d > o->pair_d[i][j] ? 1 : (d < o->pair_d[i][j] ? -1 : 0);
            if (o->pair_dir[i][j] < 0 && dir > 0 && d < ORBIT_PASS_CHORD) {
                /* A close pass: both play, felt where they meet. */
                add_note(out, a->home, (float)a->velocity / 127.0f, ORBIT_NOTE_BEATS);
                add_note(out, b->home, (float)b->velocity / 127.0f, ORBIT_NOTE_BEATS);
                uint8_t mid = cell_at((ax + bx) * 0.5f, (ay + by) * 0.5f);
                add_tap(out, mid, 0.6f);
                flash(o->flash_t, o->flash_amp, mid, 1.0f, now);
            }
            o->pair_d[i][j] = d;
            if (dir != 0) {
                o->pair_dir[i][j] = dir;
            }
        }
    }
}

bool tiles_orbits_erase(tiles_orbits_t *o, uint8_t cell) {
    return tiles_orbits_remove_planet(o, cell);
}

uint32_t tiles_orbits_erasable(const tiles_orbits_t *o) {
    uint32_t bits = 0u;
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        if (o->planet[i].live) {
            bits |= 1u << o->planet[i].home;
        }
    }
    return bits;
}

void tiles_orbits_render(const tiles_orbits_t *o, double now, tiles_phys_frame_t frame) {
    clear_frame(frame);
    double t = tiles_orbits_time(o, now);
    for (uint8_t s = 0; s < TILES_ORBITS_MAX_SUNS; s++) {
        if (!o->sun[s].live) {
            continue;
        }
        uint8_t cell = o->sun[s].cell;
        light(frame, cell, ORBIT_SUN_R, ORBIT_SUN_G, ORBIT_SUN_B, ORBIT_SUN_LEVEL);
        for (uint8_t n = 0; n < TILES_PHYS_CELLS; n++) {
            if (fabsf(cell_dist(n, cell) - 1.0f) < 0.01f) {
                light(frame, n, ORBIT_SUN_R, ORBIT_SUN_G, ORBIT_SUN_B, ORBIT_CORONA_LEVEL);
            }
        }
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        const tiles_orbit_planet_t *p = &o->planet[i];
        if (p->live) {
            const float *c = ORBIT_HUES[p->hue % 6u];
            light(frame, p->home, c[0], c[1], c[2], ORBIT_HOME_LEVEL); /* where it plays */
        }
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        const tiles_orbit_planet_t *p = &o->planet[i];
        if (p->live) {
            float x, y;
            planet_xy_at(o, p, t, &x, &y);
            const float *c = ORBIT_HUES[p->hue % 6u];
            dot(frame, x, y, c[0], c[1], c[2], ORBIT_PLANET_LEVEL);
        }
    }
    render_flashes(frame, o->flash_t, o->flash_amp, now);
}

bool tiles_orbits_active(const tiles_orbits_t *o) {
    if (!o->running) {
        return false;
    }
    for (uint8_t i = 0; i < TILES_ORBITS_MAX_PLANETS; i++) {
        if (o->planet[i].live) {
            return true;
        }
    }
    return false;
}

/* ============================ Newton's cradle ============================= */

#define CRADLE_REST 0.15f       /* rows: below this it hangs still */
#define CRADLE_WAVE_AMP 2.0f    /* rows: a wave ball's swing unless lifted */
#define CRADLE_WAVE_LEVEL 0.85f /* a wave pass, relative to a knock */
#define CRADLE_WAVE_FLASH 0.5f  /* a pass's flash, relative to its level */
#define CRADLE_FIRST_MIN 0.2f   /* beats from letting go to the first knock, at least */
#define CRADLE_HALF_SWING 1.0f  /* beats from knock to knock */
#define CRADLE_NOTE_BEATS 0.5f
#define CRADLE_FOLLOW_LEVEL 0.6f /* the second and third balls out */
#define CRADLE_BOTTOM ((uint8_t)(TILES_PHYS_ROWS - 1u))

void tiles_cradle_clear(tiles_cradle_t *c) {
    bool wave = c->wave;
    double wave_last = c->wave_last;
    memset(c, 0, sizeof(*c));
    c->wave = wave;
    c->wave_last = wave_last;
    c->lift_cell[0] = c->lift_cell[1] = TILES_PHYS_NO_CELL;
}

double tiles_cradle_wave_interval(uint8_t col) {
    return TILES_CRADLE_WAVE_BEATS / (double)(TILES_CRADLE_WAVE_FIRST + col);
}

static bool swing_uses_col(const tiles_cradle_swing_t *sw, uint8_t col);

void tiles_cradle_set_wave(tiles_cradle_t *c, bool on, double now) {
    if (on == c->wave) {
        return;
    }
    /* The balls moving now keep moving, each on its own string, as high
     * as they swung. */
    float moving[TILES_PHYS_COLS] = {0};
    for (uint8_t i = 0; on && i < TILES_CRADLE_MAX_SWINGS; i++) {
        const tiles_cradle_swing_t *sw = &c->swing[i];
        for (uint8_t col = 0; sw->live && col < TILES_PHYS_COLS; col++) {
            if (swing_uses_col(sw, col)) {
                moving[col] = fmaxf(moving[col], sw->h);
            }
        }
    }
    tiles_cradle_clear(c); /* the swings and lifts stop either way */
    c->wave = on;
    c->wave_last = now;
    for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
        c->wave_on[col] = moving[col] > 0.0f;
        c->wave_amp[col] = moving[col] > 0.0f ? moving[col] : CRADLE_WAVE_AMP;
    }
}

static float level_for_height(float h) {
    return fminf(fmaxf(h / 3.0f, CRADLE_REST), 1.0f);
}

/* A wave ball's height now: rising and settling back smoothly between
 * passes (a sharp bounce at six balls' speeds strobes). */
static float wave_height(const tiles_cradle_t *c, uint8_t col, double now) {
    if (c->wave_held[col]) {
        return c->wave_amp[col];
    }
    if (!c->wave_on[col]) {
        return 0.0f;
    }
    double u = now / tiles_cradle_wave_interval(col);
    return c->wave_amp[col] * 0.5f * (1.0f - cosf(PHYS_TWO_PI * (float)(u - floor(u))));
}

/* Ball i (0 = the end one) of the k lifted at `side`. */
static uint8_t cradle_col(int8_t side, uint8_t i) {
    return side < 0 ? i : (uint8_t)(TILES_PHYS_COLS - 1u - i);
}

static void lift_of(uint8_t cell, int8_t *side, uint8_t *k, float *h) {
    uint8_t col = (uint8_t)(cell % TILES_PHYS_COLS), row = (uint8_t)(cell / TILES_PHYS_COLS);
    *side = col < TILES_PHYS_COLS / 2u ? -1 : 1;
    *k = col < TILES_PHYS_COLS / 2u ? (uint8_t)(col + 1u) : (uint8_t)(TILES_PHYS_COLS - col);
    *h = (float)(CRADLE_BOTTOM - row);
}

/* Columns a swing moves: its k balls at either end. */
static bool swing_uses_col(const tiles_cradle_swing_t *sw, uint8_t col) {
    for (uint8_t i = 0; i < sw->k; i++) {
        if (cradle_col(-1, i) == col || cradle_col(1, i) == col) {
            return true;
        }
    }
    return false;
}

static bool catch_col(tiles_cradle_t *c, uint8_t col) {
    bool any = false;
    for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
        if (c->swing[i].live && swing_uses_col(&c->swing[i], col)) {
            c->swing[i].live = false;
            any = true;
        }
    }
    return any;
}

void tiles_cradle_strike(tiles_cradle_t *c, uint8_t cell, uint8_t velocity, double now, tiles_phys_out_t *out) {
    if (cell >= TILES_PHYS_CELLS) {
        return;
    }
    uint8_t col = (uint8_t)(cell % TILES_PHYS_COLS);
    if (c->wave) {
        if (cell / TILES_PHYS_COLS != CRADLE_BOTTOM) {
            c->wave_held[col] = true; /* lifted: still until let go */
            c->wave_on[col] = true;
            c->wave_amp[col] = (float)(CRADLE_BOTTOM - cell / TILES_PHYS_COLS);
            add_tap(out, cell, 0.3f);
        } else if (c->wave_on[col]) {
            c->wave_on[col] = false; /* stopped */
            add_tap(out, cell, 0.5f);
        } else {
            c->wave_on[col] = true; /* swinging again, in step */
            add_note(out, cell, (float)velocity / 127.0f, CRADLE_NOTE_BEATS);
            add_tap(out, cell, (float)velocity / 127.0f);
            flash(c->flash_t, c->flash_amp, cell, 1.0f, now);
        }
        return;
    }
    if (cell / TILES_PHYS_COLS == CRADLE_BOTTOM) {
        if (catch_col(c, col)) {
            add_tap(out, cell, 0.5f); /* a swinging ball: caught */
            return;
        }
        add_note(out, cell, (float)velocity / 127.0f, CRADLE_NOTE_BEATS); /* a resting one: tapped */
        add_tap(out, cell, (float)velocity / 127.0f);
        flash(c->flash_t, c->flash_amp, cell, 1.0f, now);
        return;
    }
    int8_t side;
    uint8_t k;
    float h;
    lift_of(cell, &side, &k, &h);
    /* Grabbing the balls at that end catches what swings there. */
    for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
        if (c->swing[i].live && c->swing[i].side == side) {
            c->swing[i].live = false;
        }
    }
    c->lift_cell[side < 0 ? 0 : 1] = cell;
    add_tap(out, cell, 0.3f);
}

void tiles_cradle_release(tiles_cradle_t *c, uint8_t cell, double now) {
    if (c->wave) {
        if (cell < TILES_PHYS_CELLS) {
            c->wave_held[cell % TILES_PHYS_COLS] = false;
        }
        return;
    }
    for (uint8_t s = 0; s < 2u; s++) {
        if (c->lift_cell[s] != cell) {
            continue;
        }
        c->lift_cell[s] = TILES_PHYS_NO_CELL;
        int8_t side;
        uint8_t k;
        float h;
        lift_of(cell, &side, &k, &h);
        for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
            tiles_cradle_swing_t *sw = &c->swing[i];
            if (!sw->live) {
                sw->live = true;
                sw->falling = true;
                sw->side = side;
                sw->k = k;
                sw->h = h;
                sw->t0 = now;
                sw->dur = (float)(grid_at_or_after(now + CRADLE_FIRST_MIN, 2.0f) - now);
                break;
            }
        }
    }
}

static void knock(tiles_cradle_t *c, tiles_cradle_swing_t *sw, tiles_phys_out_t *out) {
    double at = sw->t0 + (double)sw->dur;
    float level = level_for_height(sw->h);
    for (uint8_t i = 0; i < sw->k; i++) {
        uint8_t cell = (uint8_t)(CRADLE_BOTTOM * TILES_PHYS_COLS + cradle_col((int8_t)-sw->side, i));
        float l = i == 0u ? level : level * CRADLE_FOLLOW_LEVEL;
        add_note(out, cell, l, CRADLE_NOTE_BEATS);
        add_tap(out, cell, l);
        flash(c->flash_t, c->flash_amp, cell, l, at);
    }
    sw->side = (int8_t)-sw->side;
    sw->falling = false;
    sw->t0 = at;
    sw->dur = CRADLE_HALF_SWING;
}

void tiles_cradle_step(tiles_cradle_t *c, double now, tiles_phys_out_t *out) {
    if (c->wave) {
        for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
            if (!c->wave_on[col] || c->wave_held[col]) {
                continue;
            }
            double interval = tiles_cradle_wave_interval(col);
            double pass = floor(now / interval);
            if (pass > floor(c->wave_last / interval)) {
                uint8_t cell = (uint8_t)(CRADLE_BOTTOM * TILES_PHYS_COLS + col);
                float level = level_for_height(c->wave_amp[col]) * CRADLE_WAVE_LEVEL;
                add_note(out, cell, level, fminf((float)interval * 0.9f, CRADLE_NOTE_BEATS));
                add_tap(out, cell, level);
                flash(c->flash_t, c->flash_amp, cell, level * CRADLE_WAVE_FLASH, pass * interval);
            }
        }
        c->wave_last = now;
        return;
    }
    for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
        tiles_cradle_swing_t *sw = &c->swing[i];
        while (sw->live && now - sw->t0 >= (double)sw->dur) {
            knock(c, sw, out);
        }
    }
}

bool tiles_cradle_erase(tiles_cradle_t *c, uint8_t cell) {
    uint8_t col = (uint8_t)(cell % TILES_PHYS_COLS);
    if (c->wave) {
        bool was = c->wave_on[col];
        c->wave_on[col] = false;
        c->wave_held[col] = false;
        return was;
    }
    return catch_col(c, col);
}

uint32_t tiles_cradle_erasable(const tiles_cradle_t *c) {
    uint32_t bits = 0u;
    if (c->wave) {
        for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
            if (c->wave_on[col]) {
                bits |= 1u << (CRADLE_BOTTOM * TILES_PHYS_COLS + col);
            }
        }
        return bits;
    }
    for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
        const tiles_cradle_swing_t *sw = &c->swing[i];
        if (!sw->live) {
            continue;
        }
        for (uint8_t b = 0; b < sw->k; b++) {
            bits |= 1u << (CRADLE_BOTTOM * TILES_PHYS_COLS + cradle_col(-1, b));
            bits |= 1u << (CRADLE_BOTTOM * TILES_PHYS_COLS + cradle_col(1, b));
        }
    }
    return bits;
}

/* How high a swing's balls are now, in rows. */
static float swing_height(const tiles_cradle_swing_t *sw, double now) {
    float e = fminf(fmaxf((float)(now - sw->t0) / sw->dur, 0.0f), 1.0f);
    return sw->falling ? sw->h * cosf(PHYS_PI * 0.5f * e) : sw->h * sinf(PHYS_PI * e);
}

void tiles_cradle_render(const tiles_cradle_t *c, double now, tiles_phys_frame_t frame) {
    clear_frame(frame);
    float lifted[TILES_PHYS_COLS] = {0};
    for (uint8_t col = 0; c->wave && col < TILES_PHYS_COLS; col++) {
        lifted[col] = wave_height(c, col, now);
    }
    for (uint8_t s = 0; s < 2u; s++) {
        if (c->lift_cell[s] == TILES_PHYS_NO_CELL) {
            continue;
        }
        int8_t side;
        uint8_t k;
        float h;
        lift_of(c->lift_cell[s], &side, &k, &h);
        for (uint8_t i = 0; i < k; i++) {
            lifted[cradle_col(side, i)] = fmaxf(lifted[cradle_col(side, i)], h);
        }
    }
    for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
        const tiles_cradle_swing_t *sw = &c->swing[i];
        if (!sw->live) {
            continue;
        }
        float h = swing_height(sw, now);
        for (uint8_t b = 0; b < sw->k; b++) {
            uint8_t col = cradle_col(sw->side, b);
            lifted[col] = fmaxf(lifted[col], h);
        }
    }
    for (uint8_t col = 0; col < TILES_PHYS_COLS; col++) {
        float h = lifted[col];
        if (h < 0.08f) {
            light(frame, (uint8_t)(CRADLE_BOTTOM * TILES_PHYS_COLS + col), TILES_CRADLE_COLOR_R, TILES_CRADLE_COLOR_G,
                  TILES_CRADLE_COLOR_B, 0.3f); /* a ball at rest */
            continue;
        }
        dot(frame, (float)col + 0.5f, (float)TILES_PHYS_ROWS - 0.5f - h, 0.9f, 0.92f, 1.0f, 0.95f);
    }
    render_flashes(frame, c->flash_t, c->flash_amp, now);
}

bool tiles_cradle_active(const tiles_cradle_t *c) {
    for (uint8_t col = 0; c->wave && col < TILES_PHYS_COLS; col++) {
        if (c->wave_on[col]) {
            return true;
        }
    }
    for (uint8_t i = 0; i < TILES_CRADLE_MAX_SWINGS; i++) {
        if (c->swing[i].live) {
            return true;
        }
    }
    return c->lift_cell[0] != TILES_PHYS_NO_CELL || c->lift_cell[1] != TILES_PHYS_NO_CELL;
}
