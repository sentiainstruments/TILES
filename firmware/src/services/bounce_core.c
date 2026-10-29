#include "bounce_core.h"

#include <math.h>
#include <string.h>

void tiles_bounce_clear(tiles_bounce_t *b) {
    memset(b, 0, sizeof(*b));
}

uint8_t tiles_bounce_period(uint8_t height) {
    if (height == 0u) {
        return 0u;
    }
    return height == 1u ? 1u : (uint8_t)(2u * (height - 1u));
}

/* Phase 0 is on the floor; the ball climbs to its height at phase h-1 and
 * falls back, landing at phase 2(h-1) = 0. Starting at phase h-1 is the
 * top, about to fall. */
void tiles_bounce_set_height(tiles_bounce_t *b, uint8_t lane, uint8_t height) {
    if (lane >= TILES_BOUNCE_LANES) {
        return;
    }
    if (height > TILES_BOUNCE_MAX_HEIGHT) {
        height = TILES_BOUNCE_MAX_HEIGHT;
    }
    b->height[lane] = height;
    b->phase[lane] = height > 1u ? (uint8_t)(height - 1u) : 0u;
}

void tiles_bounce_drop_all(tiles_bounce_t *b) {
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        tiles_bounce_set_height(b, lane, b->height[lane]);
    }
}

uint8_t tiles_bounce_ball_height(const tiles_bounce_t *b, uint8_t lane) {
    if (lane >= TILES_BOUNCE_LANES || b->height[lane] == 0u) {
        return 0u;
    }
    uint8_t h = b->height[lane];
    uint8_t p = b->phase[lane];
    if (h == 1u) {
        return 1u;
    }
    return p <= h - 1u ? (uint8_t)(1u + p) : (uint8_t)(1u + 2u * (h - 1u) - p);
}

uint16_t tiles_bounce_step(tiles_bounce_t *b) {
    uint16_t landed = 0u;
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        uint8_t period = tiles_bounce_period(b->height[lane]);
        if (period == 0u) {
            continue;
        }
        b->phase[lane] = (uint8_t)((b->phase[lane] + 1u) % period);
        if (b->phase[lane] == 0u) {
            landed |= (uint16_t)(1u << lane);
        }
    }
    return landed;
}

void tiles_gravity_clear(tiles_gravity_t *g) {
    memset(g, 0, sizeof(*g));
}

float tiles_gravity_period(float apex) {
    return apex > 0.0f ? TILES_GRAVITY_PERIOD_K * sqrtf(apex) : 0.0f;
}

void tiles_gravity_throw(tiles_gravity_t *g, uint8_t lane, float apex) {
    if (lane >= TILES_BOUNCE_LANES) {
        return;
    }
    if (apex < TILES_GRAVITY_REST_APEX) {
        apex = TILES_GRAVITY_REST_APEX;
    }
    g->apex[lane] = apex;
    g->t[lane] = 0.5f * tiles_gravity_period(apex);
}

uint16_t tiles_gravity_step(tiles_gravity_t *g, float dt, float impact[TILES_BOUNCE_LANES]) {
    uint16_t landed = 0u;
    for (uint8_t lane = 0; lane < TILES_BOUNCE_LANES; lane++) {
        if (g->apex[lane] <= 0.0f) {
            continue;
        }
        g->t[lane] += dt;
        /* Bounded: a long dt lands it a few times, the last one counts. */
        for (uint8_t guard = 0; guard < 64u && g->apex[lane] > 0.0f; guard++) {
            float period = tiles_gravity_period(g->apex[lane]);
            if (g->t[lane] < period) {
                break;
            }
            g->t[lane] -= period;
            landed |= (uint16_t)(1u << lane);
            impact[lane] = g->apex[lane];
            g->apex[lane] *= TILES_GRAVITY_RESTITUTION;
            if (g->apex[lane] < TILES_GRAVITY_REST_APEX) {
                g->apex[lane] = 0.0f; /* at rest on the floor */
                g->t[lane] = 0.0f;
            }
        }
    }
    return landed;
}

float tiles_gravity_height(const tiles_gravity_t *g, uint8_t lane) {
    if (lane >= TILES_BOUNCE_LANES || g->apex[lane] <= 0.0f) {
        return 0.0f;
    }
    float period = tiles_gravity_period(g->apex[lane]);
    float u = g->t[lane] / period;
    if (u > 1.0f) {
        u = 1.0f;
    }
    return g->apex[lane] * 4.0f * u * (1.0f - u);
}
