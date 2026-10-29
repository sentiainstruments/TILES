#include "seq_pattern.h"

#include <string.h>

void tiles_seq_pattern_reset(op_seq_pattern_t *p) {
    memset(p, 0, sizeof(*p));
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        p->step_probability_percent[i] = 100u;
        p->step_ratchet_count[i] = 1u;
    }
    p->length = OP_SEQ_NUM_STEPS;
}

static uint32_t mask_of(const bool *flags) {
    uint32_t m = 0u;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if (flags[i]) {
            m |= (uint32_t)1u << i;
        }
    }
    return m;
}

static uint32_t edited_mask(const op_seq_pattern_t *p) {
    uint32_t m = 0u;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if (p->step_probability_percent[i] != 100u || p->step_ratchet_count[i] != 1u) {
            m |= (uint32_t)1u << i;
        }
    }
    return m;
}

static bool is_empty(const op_seq_pattern_t *p) {
    return mask_of(p->step_armed) == 0u && mask_of(p->step_pitch_override) == 0u && edited_mask(p) == 0u &&
           p->length == OP_SEQ_NUM_STEPS && !p->probability_enabled;
}

static void put_u32(uint8_t *o, uint32_t v) {
    o[0] = (uint8_t)(v & 0xFFu);
    o[1] = (uint8_t)((v >> 8) & 0xFFu);
    o[2] = (uint8_t)((v >> 16) & 0xFFu);
    o[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *i) {
    return (uint32_t)i[0] | ((uint32_t)i[1] << 8) | ((uint32_t)i[2] << 16) | ((uint32_t)i[3] << 24);
}

static uint8_t count_notes(const op_seq_pattern_t *p, uint8_t step) {
    uint8_t n = p->step_note_count[step];
    return n > OP_SEQ_MAX_NOTES_PER_STEP ? OP_SEQ_MAX_NOTES_PER_STEP : n;
}

/* Bytes one pattern's record takes. */
static uint16_t record_size(const op_seq_pattern_t *p) {
    uint16_t size = 15u;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if (p->step_pitch_override[i]) {
            size = (uint16_t)(size + 1u + count_notes(p, i));
        }
        if (p->step_probability_percent[i] != 100u || p->step_ratchet_count[i] != 1u) {
            size = (uint16_t)(size + 2u);
        }
    }
    return size;
}

static uint16_t put_record(const op_seq_pattern_t *p, uint8_t index, uint8_t *o) {
    uint32_t override = mask_of(p->step_pitch_override), edited = edited_mask(p);
    uint16_t at = 0u;
    o[at++] = index;
    o[at++] = p->length;
    o[at++] = p->probability_enabled ? 1u : 0u;
    put_u32(&o[at], mask_of(p->step_armed));
    put_u32(&o[at + 4u], override);
    put_u32(&o[at + 8u], edited);
    at = (uint16_t)(at + 12u);
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if ((override >> i) & 1u) {
            uint8_t n = count_notes(p, i);
            o[at++] = n;
            for (uint8_t k = 0; k < n; k++) {
                o[at++] = p->step_notes[i][k];
            }
        }
    }
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if ((edited >> i) & 1u) {
            o[at++] = p->step_probability_percent[i];
            o[at++] = p->step_ratchet_count[i];
        }
    }
    return at;
}

uint16_t tiles_seq_encode(const op_seq_pattern_t pats[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE],
                          const uint8_t playing[OP_SEQ_NUM_LANES], uint8_t viewed_lane, uint8_t *out, uint16_t cap,
                          bool *truncated) {
    *truncated = false;
    if (cap < 1u + OP_SEQ_NUM_LANES) {
        *truncated = true;
        return 0u;
    }
    uint16_t len = 0u;
    out[len++] = viewed_lane;
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        out[len++] = playing[lane];
    }
    /* The playing patterns first, so a full blob keeps what's heard. */
    for (uint8_t pass = 0; pass < 2u; pass++) {
        for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
            for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
                bool is_playing = playing[lane] == alt;
                if ((pass == 0u) != is_playing || is_empty(&pats[lane][alt])) {
                    continue;
                }
                uint16_t need = record_size(&pats[lane][alt]);
                if ((uint32_t)len + need > cap) {
                    *truncated = true;
                    return len;
                }
                len = (uint16_t)(len + put_record(&pats[lane][alt], (uint8_t)(lane * OP_SEQ_ALTS_PER_LANE + alt),
                                                  &out[len]));
            }
        }
    }
    return len;
}

bool tiles_seq_decode(op_seq_pattern_t pats[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE], uint8_t playing[OP_SEQ_NUM_LANES],
                      uint8_t *viewed_lane, const uint8_t *in, uint16_t len, uint16_t version) {
    if (version != TILES_SEQ_BLOB_VERSION || len < 1u + OP_SEQ_NUM_LANES) {
        return false;
    }
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            tiles_seq_pattern_reset(&pats[lane][alt]);
        }
    }
    uint16_t pos = 0u;
    *viewed_lane = in[pos] < OP_SEQ_NUM_LANES ? in[pos] : 0u;
    pos++;
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++, pos++) {
        playing[lane] = in[pos] < OP_SEQ_ALTS_PER_LANE ? in[pos] : 0u;
    }
    while ((uint32_t)pos + 15u <= len) {
        uint8_t index = in[pos];
        if (index >= OP_SEQ_NUM_LANES * OP_SEQ_ALTS_PER_LANE) {
            return true; /* damaged: keep what came before */
        }
        op_seq_pattern_t p;
        tiles_seq_pattern_reset(&p);
        uint8_t length = in[pos + 1u];
        p.length = length < 1u ? 1u : (length > OP_SEQ_NUM_STEPS ? OP_SEQ_NUM_STEPS : length);
        p.probability_enabled = (in[pos + 2u] & 1u) != 0u;
        uint32_t armed = get_u32(&in[pos + 3u]), override = get_u32(&in[pos + 7u]), edited = get_u32(&in[pos + 11u]);
        uint16_t at = (uint16_t)(pos + 15u);
        for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
            p.step_armed[i] = (armed >> i) & 1u;
            if ((override >> i) & 1u) {
                if (at >= len) {
                    return true;
                }
                uint8_t n = in[at++];
                if (n > OP_SEQ_MAX_NOTES_PER_STEP || (uint32_t)at + n > len) {
                    return true;
                }
                p.step_pitch_override[i] = true;
                p.step_note_count[i] = n;
                for (uint8_t k = 0; k < n; k++) {
                    p.step_notes[i][k] = (uint8_t)(in[at++] & 0x7Fu);
                }
            }
        }
        for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
            if ((edited >> i) & 1u) {
                if ((uint32_t)at + 2u > len) {
                    return true;
                }
                uint8_t chance = in[at], repeats = in[at + 1u];
                p.step_probability_percent[i] = chance > 100u ? 100u : chance;
                p.step_ratchet_count[i] =
                    repeats < 1u ? 1u : (repeats > OP_SEQ_MAX_RATCHET ? OP_SEQ_MAX_RATCHET : repeats);
                at = (uint16_t)(at + 2u);
            }
        }
        pats[index / OP_SEQ_ALTS_PER_LANE][index % OP_SEQ_ALTS_PER_LANE] = p;
        pos = at;
    }
    return true;
}

uint32_t tiles_seq_fingerprint(const op_seq_pattern_t pats[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE],
                               const uint8_t playing[OP_SEQ_NUM_LANES], uint8_t viewed_lane) {
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)pats;
    for (uint32_t i = 0; i < sizeof(op_seq_pattern_t) * OP_SEQ_NUM_LANES * OP_SEQ_ALTS_PER_LANE; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        h = (h ^ playing[lane]) * 16777619u;
    }
    return (h ^ viewed_lane) * 16777619u;
}
