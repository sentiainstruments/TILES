#include "strike_settle.h"

void tiles_strike_settle_begin(tiles_strike_settle_t *st, float depth, uint32_t now_ms) {
    st->crossed_ms = now_ms;
    st->rise_depth = depth;
    st->rise_ms = now_ms;
    st->samples_since_rise = 0u;
}

void tiles_strike_settle_sample(tiles_strike_settle_t *st, float depth, uint32_t sample_ms) {
    if (depth >= st->rise_depth + TILES_STRIKE_SETTLE_RISE) {
        st->rise_depth = depth;
        st->rise_ms = sample_ms;
        st->samples_since_rise = 0u;
    } else if (st->samples_since_rise < 255u) {
        st->samples_since_rise++;
    }
}

bool tiles_strike_settle_ready(const tiles_strike_settle_t *st, float peak_depth, float full_depth,
                               uint32_t now_ms) {
    if (now_ms - st->crossed_ms >= TILES_STRIKE_FOLLOWTHROUGH_MS) {
        return true;
    }
    if (peak_depth >= full_depth) {
        return true;
    }
    return st->samples_since_rise >= TILES_STRIKE_SETTLE_SAMPLES && now_ms - st->rise_ms >= TILES_STRIKE_SETTLE_MS;
}
