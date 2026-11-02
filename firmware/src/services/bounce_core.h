#pragma once

/* Bouncing notes' physics, no hardware (bounce.c is the glue); tested in
 * test_bounce_core.c. A ball from height h lands every 2(h-1) steps, so
 * lanes at different heights make polyrhythms. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_BOUNCE_LANES 12u
#define TILES_BOUNCE_MAX_HEIGHT 4u

typedef struct {
    uint8_t height[TILES_BOUNCE_LANES]; /* 0 = off, 1-4 */
    uint8_t phase[TILES_BOUNCE_LANES];  /* steps since the last landing */
} tiles_bounce_t;

void tiles_bounce_clear(tiles_bounce_t *b);

/* Steps between landings for height h (0 for off). */
uint8_t tiles_bounce_period(uint8_t height);

/* Drops lane's ball from `height` (0 = off): it starts at the top, falling,
 * so it lands height-1 steps later. */
void tiles_bounce_set_height(tiles_bounce_t *b, uint8_t lane, uint8_t height);

/* Every ball back to the top of its height, falling (a rewind). */
void tiles_bounce_drop_all(tiles_bounce_t *b);

/* Where lane's ball is: 1 = the floor .. its height; 0 if off. */
uint8_t tiles_bounce_ball_height(const tiles_bounce_t *b, uint8_t lane);

/* One step: every ball moves; returns the lanes that land (bit n = lane n). */
uint16_t tiles_bounce_step(tiles_bounce_t *b);

/* ---- Real gravity (square) --------------------------------------------------
 * A ball thrown to an apex keeps RESTITUTION of its height each landing:
 * lower, faster, softer until it rests. Time in eighths (the tempo scales
 * it), off the clock's grid; a bounce takes PERIOD_K * sqrt(apex). */
#define TILES_GRAVITY_RESTITUTION 0.7f
#define TILES_GRAVITY_REST_APEX 0.06f
#define TILES_GRAVITY_PERIOD_K 3.4641016f /* 6 / sqrt(3) */

typedef struct {
    float apex[TILES_BOUNCE_LANES]; /* 0 = at rest */
    float t[TILES_BOUNCE_LANES];    /* eighths into the current bounce */
} tiles_gravity_t;

void tiles_gravity_clear(tiles_gravity_t *g);
float tiles_gravity_period(float apex);
/* Thrown to `apex`: starts at the top, falling (lands half a period later). */
void tiles_gravity_throw(tiles_gravity_t *g, uint8_t lane, float apex);
/* Moves every ball `dt` eighths on; returns the lanes that landed (bit n),
 * with each one's apex before the landing (how hard it hit) in impact[]. */
uint16_t tiles_gravity_step(tiles_gravity_t *g, float dt, float impact[TILES_BOUNCE_LANES]);
/* Where lane's ball is, in pads above the floor (0 at rest or landing). */
float tiles_gravity_height(const tiles_gravity_t *g, uint8_t lane);
