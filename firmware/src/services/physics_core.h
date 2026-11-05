#pragma once

/* The physics modes' simulations, no hardware (physics.c is the glue);
 * tested in test_physics_core.c. Cells: row * 6 + col, row 0 at the top;
 * a cell's centre is (col + 0.5, row + 0.5), y down. Time is in beats
 * (double). A step fills a tiles_phys_out_t (notes, taps); a render fills
 * one RGB frame (0..1). */

#include <stdbool.h>
#include <stdint.h>

#define TILES_PHYS_ROWS 4u
#define TILES_PHYS_COLS 6u
#define TILES_PHYS_CELLS 24u
#define TILES_PHYS_NO_CELL 0xFFu

#define TILES_PHYS_MAX_NOTES 32u
#define TILES_PHYS_MAX_TAPS 12u
#define TILES_PHYS_CELL_NOTE (-1) /* note_degree: play the cell's own note */

typedef struct {
    uint8_t note_cell[TILES_PHYS_MAX_NOTES];
    int8_t note_degree[TILES_PHYS_MAX_NOTES]; /* >= 0: that scale degree */
    uint8_t note_velocity[TILES_PHYS_MAX_NOTES];
    float note_beats[TILES_PHYS_MAX_NOTES];
    uint8_t note_count;
    uint8_t tap_cell[TILES_PHYS_MAX_TAPS];
    uint8_t tap_strength[TILES_PHYS_MAX_TAPS]; /* 0-127 */
    uint8_t tap_count;
} tiles_phys_out_t;

typedef float tiles_phys_frame_t[TILES_PHYS_CELLS][3];

void tiles_phys_out_clear(tiles_phys_out_t *out);

/* Each mode's colour (menu pad, underglow, the frame's main colour). */
#define TILES_RIPPLES_COLOR_R 0.3f /* water */
#define TILES_RIPPLES_COLOR_G 0.7f
#define TILES_RIPPLES_COLOR_B 1.0f
#define TILES_RIPPLES_STONE_R 0.05f /* a ripple's root: deep blue under the water's light blue */
#define TILES_RIPPLES_STONE_G 0.25f
#define TILES_RIPPLES_STONE_B 1.0f
#define TILES_SAND_COLOR_R 0.8f /* sand: a dark, warm ochre */
#define TILES_SAND_COLOR_G 0.55f
#define TILES_SAND_COLOR_B 0.25f
#define TILES_ORBITS_COLOR_R 0.42f /* night sky */
#define TILES_ORBITS_COLOR_G 0.08f
#define TILES_ORBITS_COLOR_B 1.0f
#define TILES_CRADLE_COLOR_R 0.8f /* steel */
#define TILES_CRADLE_COLOR_G 0.8f
#define TILES_CRADLE_COLOR_B 0.8f

/* ---- Ripples ----------------------------------------------------------------
 * A strike drops a stone: its cell plays and a ring spreads a pad per
 * sixteenth, playing softer further out; harder = bigger ring. Stones drop
 * again every bar; a strike on one lifts it. Splash (square): extra drops. */
#define TILES_RIPPLES_MAX_STONES 8u
#define TILES_RIPPLES_MAX_RINGS 16u
#define TILES_RIPPLES_MAX_HITS 48u
#define TILES_RIPPLES_MAX_DROPS 12u
#define TILES_RIPPLES_SPEED 4.0f   /* pads per beat */
#define TILES_RIPPLES_LOOP_BEATS 4.0 /* a stone drops once a bar */

typedef struct {
    bool live;
    uint8_t cell;
    float base, strength; /* the strike, and with the press on top */
    double next;          /* its next drop */
} tiles_ripple_stone_t;

typedef struct {
    bool live;
    uint8_t src;
    float x, y, strength, reach;
    double t0;
} tiles_ripple_ring_t;

typedef struct {
    bool live;
    uint8_t cell, n;
    float amp;
    double due;
} tiles_ripple_hit_t;

typedef struct {
    bool live;
    uint8_t from, to;
    float strength;
    double t0, at;
} tiles_ripple_drop_t;

typedef struct {
    tiles_ripple_stone_t stone[TILES_RIPPLES_MAX_STONES];
    tiles_ripple_ring_t ring[TILES_RIPPLES_MAX_RINGS];
    tiles_ripple_hit_t hit[TILES_RIPPLES_MAX_HITS];
    tiles_ripple_drop_t drop[TILES_RIPPLES_MAX_DROPS];
    bool held[TILES_PHYS_CELLS];
    double flash_t[TILES_PHYS_CELLS];
    float flash_amp[TILES_PHYS_CELLS];
    bool splash;
    uint32_t rng;
    double last;
} tiles_ripples_t;

void tiles_ripples_clear(tiles_ripples_t *r, uint32_t seed, double now);
/* A strike, 0..1: a new stone, or a stone lifted out (false). */
bool tiles_ripples_strike(tiles_ripples_t *r, uint8_t cell, float strength, double now, tiles_phys_out_t *out);
void tiles_ripples_set_pressure(tiles_ripples_t *r, uint8_t cell, float pressure);
void tiles_ripples_release(tiles_ripples_t *r, uint8_t cell);
void tiles_ripples_step(tiles_ripples_t *r, double now, tiles_phys_out_t *out);
/* The stone, rings and drops of a cell (circle + tap); true if any. */
bool tiles_ripples_erase(tiles_ripples_t *r, uint8_t cell);
uint32_t tiles_ripples_erasable(const tiles_ripples_t *r); /* bit = cell */
bool tiles_ripples_has_stone(const tiles_ripples_t *r, uint8_t cell);
void tiles_ripples_render(const tiles_ripples_t *r, double now, tiles_phys_frame_t frame);
bool tiles_ripples_active(const tiles_ripples_t *r);

/* ---- Sand -------------------------------------------------------------------
 * Columns are steps: a triplet-eighth playhead plays each pile at its
 * height (a scale degree per grain). Holding pours, faster when harder;
 * steep piles slide; every two bars each pile loses a grain. Flip (square)
 * turns it upside down. Circle + tap empties a column. */
#define TILES_SAND_PER_CELL 3u
#define TILES_SAND_MAX_HEIGHT 12u
#define TILES_SAND_MAX_GRAINS 80u /* a flip lets every grain go at once */
#define TILES_SAND_MAX_SLIDES 32u
#define TILES_SAND_STEPS_PER_BEAT 3u

typedef struct {
    bool live;
    bool settles; /* from a flip: lands where it falls, no avalanche */
    float x, y, vy;
} tiles_sand_grain_t;

typedef struct {
    bool live;
    uint8_t from, to, depth;
    double at;
} tiles_sand_slide_t;

typedef struct {
    uint8_t h[TILES_PHYS_COLS]; /* grains in each column's pile */
    tiles_sand_grain_t grain[TILES_SAND_MAX_GRAINS];
    tiles_sand_slide_t slide[TILES_SAND_MAX_SLIDES];
    bool held[TILES_PHYS_CELLS];
    float pressure[TILES_PHYS_CELLS];
    double pour_next[TILES_PHYS_CELLS];
    double flash_t[TILES_PHYS_CELLS];
    float flash_amp[TILES_PHYS_CELLS];
    int8_t gdir;      /* +1 down, -1 up (flipped) */
    uint8_t playhead; /* the column last played */
    double step_next;
    double leak_next;
    double last;
} tiles_sand_t;

void tiles_sand_clear(tiles_sand_t *s, double now);
/* Pour divisions per beat for a pressure 0..1: 1, 2, 4 or 8. */
uint8_t tiles_sand_division(float pressure);
/* The scale degree a column plays (its surface's height), or -1 if empty. */
int8_t tiles_sand_degree(const tiles_sand_t *s, uint8_t col);
void tiles_sand_strike(tiles_sand_t *s, uint8_t cell, double now);
void tiles_sand_set_pressure(tiles_sand_t *s, uint8_t cell, float pressure);
void tiles_sand_release(tiles_sand_t *s, uint8_t cell);
void tiles_sand_step(tiles_sand_t *s, double now, float sec_per_beat, tiles_phys_out_t *out);
void tiles_sand_flip(tiles_sand_t *s, bool up);
/* Empties the cell's column. */
bool tiles_sand_erase(tiles_sand_t *s, uint8_t cell);
uint32_t tiles_sand_erasable(const tiles_sand_t *s);
void tiles_sand_render(const tiles_sand_t *s, double now, tiles_phys_frame_t frame);
bool tiles_sand_active(const tiles_sand_t *s);

/* ---- Orbits -----------------------------------------------------------------
 * A strike launches a planet round the sun, playing its cell's note each
 * time round; periods follow Kepler, rounded to half beats (polyrhythms).
 * A strike on a planet removes it; holding moves the sun. Pull (square):
 * planets attract each other. "-" freezes, "+" resumes on the beat. */
#define TILES_ORBITS_MAX_SUNS 2u
#define TILES_ORBITS_MAX_PLANETS 8u
#define TILES_ORBITS_HOME_SUN 14u /* row 2, column 3: near the middle */

typedef struct {
    bool live;
    uint8_t cell;
} tiles_orbit_sun_t;

typedef struct {
    bool live;
    uint8_t home, sun, velocity, hue;
    float r0, home_angle, period; /* period in beats */
    double t_ref;                 /* clockwork: at home at t_ref + n * period */
    double next_due;
    float theta, omega, r, vr; /* Pull: the free state */
    float prev_rel;            /* Pull: last angle from home, for passing it */
    float r_off;               /* clockwork: radius still gliding back */
} tiles_orbit_planet_t;

typedef struct {
    tiles_orbit_sun_t sun[TILES_ORBITS_MAX_SUNS];
    tiles_orbit_planet_t planet[TILES_ORBITS_MAX_PLANETS];
    float pair_d[TILES_ORBITS_MAX_PLANETS][TILES_ORBITS_MAX_PLANETS]; /* Pull: last distance */
    int8_t pair_dir[TILES_ORBITS_MAX_PLANETS][TILES_ORBITS_MAX_PLANETS];
    bool pull;
    bool running;
    double offset;    /* orbit time = now - offset while running */
    double paused_t;  /* orbit time while stopped */
    double paused_at; /* the beat clock when it stopped */
    double resume_at; /* "+": carries on here (a whole number of beats on) */
    double last_t;
    double flash_t[TILES_PHYS_CELLS];
    float flash_amp[TILES_PHYS_CELLS];
    uint8_t next_hue;
} tiles_orbits_t;

/* Every planet gone, the sun back in the middle; the transport and Pull
 * stay as they are (zero the struct and set running for a first start). */
void tiles_orbits_clear(tiles_orbits_t *o, double now);
/* Orbit time: the beat clock less the stops. */
double tiles_orbits_time(const tiles_orbits_t *o, double now);
/* Period in beats for a radius in pads (Kepler, rounded). */
float tiles_orbits_period(float radius);
bool tiles_orbits_is_sun(const tiles_orbits_t *o, uint8_t cell);
uint8_t tiles_orbits_sun_count(const tiles_orbits_t *o);
/* The planet that plays this cell, or -1. */
int8_t tiles_orbits_planet_at(const tiles_orbits_t *o, uint8_t cell);
/* A tap's release: a planet from `cell`, struck at `struck_at`. */
void tiles_orbits_launch(tiles_orbits_t *o, uint8_t cell, uint8_t velocity, double struck_at);
/* Takes the planet that plays this cell away; true if there was one. */
bool tiles_orbits_remove_planet(tiles_orbits_t *o, uint8_t cell);
/* The sun to this cell (one sun): each planet takes its new distance and
 * period, its next pass on the next sixteenth. A planet on that very cell
 * is swallowed. */
void tiles_orbits_move_sun(tiles_orbits_t *o, uint8_t cell, double now);
void tiles_orbits_set_pull(tiles_orbits_t *o, bool on, double now);
void tiles_orbits_stop(tiles_orbits_t *o, double now);
void tiles_orbits_play(tiles_orbits_t *o, double now);
bool tiles_orbits_is_running(const tiles_orbits_t *o);
void tiles_orbits_step(tiles_orbits_t *o, double now, tiles_phys_out_t *out);
/* Circle + tap: a planet's cell takes the planet away. */
bool tiles_orbits_erase(tiles_orbits_t *o, uint8_t cell);
uint32_t tiles_orbits_erasable(const tiles_orbits_t *o);
void tiles_orbits_render(const tiles_orbits_t *o, double now, tiles_phys_frame_t frame);
bool tiles_orbits_active(const tiles_orbits_t *o);
/* Where a planet is drawn now (for tests). */
void tiles_orbits_planet_xy(const tiles_orbits_t *o, uint8_t i, double now, float *x, float *y);

/* ---- Newton's cradle --------------------------------------------------------
 * Holding above the bottom row lifts 1-3 balls from that end (higher =
 * louder); released, they knock on the next eighth, then once a beat,
 * forever. Pendulum wave (square): each ball on its own string (column 1
 * eight swings per two bars ... column 6 thirteen), meeting every second
 * bar; a strike starts or stops a ball. */
#define TILES_CRADLE_MAX_SWINGS 4u
#define TILES_CRADLE_WAVE_BEATS 8.0 /* two bars: the wave lines up again */
#define TILES_CRADLE_WAVE_FIRST 8u  /* column 1's passes in that time */

typedef struct {
    bool live;
    bool falling; /* let go, before the first knock */
    int8_t side;  /* the end it is lifted at: -1 left, +1 right */
    uint8_t k;    /* balls */
    float h;      /* height, in rows */
    float dur;    /* this half-swing, beats */
    double t0;
} tiles_cradle_swing_t;

typedef struct {
    tiles_cradle_swing_t swing[TILES_CRADLE_MAX_SWINGS];
    uint8_t lift_cell[2]; /* held lifts, per side (left, right); NO_CELL if none */
    bool wave;                       /* Pendulum wave (square) */
    bool wave_on[TILES_PHYS_COLS];   /* that ball swinging in it */
    bool wave_held[TILES_PHYS_COLS]; /* lifted by a finger: still, silent */
    float wave_amp[TILES_PHYS_COLS]; /* rows: how high it swings, how loud */
    double wave_last;
    double flash_t[TILES_PHYS_CELLS];
    float flash_amp[TILES_PHYS_CELLS];
} tiles_cradle_t;

void tiles_cradle_clear(tiles_cradle_t *c);
/* Pendulum wave on (the swinging balls carry on as pendulums, in step
 * with the two-bar cycle) or off (back to the cradle, at rest). */
void tiles_cradle_set_wave(tiles_cradle_t *c, bool on, double now);
/* Beats between a column's passes in the wave. */
double tiles_cradle_wave_interval(uint8_t col);
/* A strike: on the bottom row, catches a swinging ball's swing or taps a
 * resting ball; above it, lifts. */
void tiles_cradle_strike(tiles_cradle_t *c, uint8_t cell, uint8_t velocity, double now, tiles_phys_out_t *out);
void tiles_cradle_release(tiles_cradle_t *c, uint8_t cell, double now);
void tiles_cradle_step(tiles_cradle_t *c, double now, tiles_phys_out_t *out);
bool tiles_cradle_erase(tiles_cradle_t *c, uint8_t cell);
uint32_t tiles_cradle_erasable(const tiles_cradle_t *c);
void tiles_cradle_render(const tiles_cradle_t *c, double now, tiles_phys_frame_t frame);
bool tiles_cradle_active(const tiles_cradle_t *c);
