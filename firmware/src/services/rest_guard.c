#include "rest_guard.h"

static int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

void tiles_rest_guard_set(tiles_rest_guard_t *g, int16_t baseline, bool confirmed) {
    g->baseline = baseline;
    g->confirmed = confirmed;
    g->streak = false;
}

void tiles_rest_guard_touched(tiles_rest_guard_t *g) {
    g->streak = false;
}

bool tiles_rest_guard_untouched(tiles_rest_guard_t *g, int16_t z, uint32_t now_ms) {
    if (!g->streak || abs32((int32_t)z - g->last_z) > TILES_REST_NOISE) {
        /* A new streak starts here (the reference slides with every read, so
         * slow drift still counts as steady). */
        g->streak = true;
        g->last_z = z;
        g->steady_since_ms = now_ms;
        return false;
    }
    g->last_z = z;
    uint32_t steady_ms = now_ms - g->steady_since_ms;
    int32_t gap = (int32_t)z - g->baseline;
    if (!g->confirmed) {
        if (steady_ms < TILES_REST_DWELL_MS) {
            return false;
        }
        g->baseline = z;
        g->confirmed = true;
        return true;
    }
    if (abs32(gap) >= TILES_REST_REZERO_DEPTH) {
        if (steady_ms < TILES_REST_REZERO_MS) {
            return false;
        }
        g->baseline = z;
        g->rezeros++;
        return true;
    }
    if (steady_ms < TILES_REST_DWELL_MS) {
        return false;
    }
    /* Nudge, never snap, by at least 1 count so a small gap doesn't stall
     * at zero from integer division. */
    int32_t nudge = gap / TILES_REST_DRIFT_SLEW;
    if (nudge == 0 && gap != 0) {
        nudge = gap > 0 ? 1 : -1;
    }
    g->baseline = (int16_t)(g->baseline + nudge);
    return false;
}

uint16_t tiles_rest_guard_depth(const tiles_rest_guard_t *g, int16_t z) {
    if (!g->confirmed) {
        return 0u;
    }
    int32_t d = abs32((int32_t)z - g->baseline);
    return (uint16_t)(d > 0xFFFF ? 0xFFFF : d);
}
