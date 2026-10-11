#include "drum_pattern.h"

#include <string.h>

#define GRID_COLS 6u
#define STEP_COLS 4u

/* ---- layout ---- */

uint8_t tiles_drum_pad_for_step(uint8_t step) {
    step = (uint8_t)(step % TILES_DRUM_PAGE_STEPS);
    return (uint8_t)((step / STEP_COLS) * GRID_COLS + step % STEP_COLS + 1u);
}

bool tiles_drum_step_for_pad(uint8_t pad, uint8_t *out_step) {
    if (pad < 1u || pad > 24u) {
        return false;
    }
    uint8_t idx = (uint8_t)(pad - 1u), row = (uint8_t)(idx / GRID_COLS), col = (uint8_t)(idx % GRID_COLS);
    if (col >= STEP_COLS) {
        return false;
    }
    *out_step = (uint8_t)(row * STEP_COLS + col);
    return true;
}

uint8_t tiles_drum_pad_for_voice(uint8_t voice) {
    voice = (uint8_t)(voice % TILES_DRUM_VOICES);
    return (uint8_t)((voice / 2u) * GRID_COLS + STEP_COLS + voice % 2u + 1u);
}

bool tiles_drum_voice_for_pad(uint8_t pad, uint8_t *out_voice) {
    if (pad < 1u || pad > 24u) {
        return false;
    }
    uint8_t idx = (uint8_t)(pad - 1u), row = (uint8_t)(idx / GRID_COLS), col = (uint8_t)(idx % GRID_COLS);
    if (col < STEP_COLS) {
        return false;
    }
    *out_voice = (uint8_t)(row * 2u + (col - STEP_COLS));
    return true;
}

int8_t tiles_drum_clamp_bank(int bank) {
    if (bank < TILES_DRUM_BANK_MIN) {
        return (int8_t)TILES_DRUM_BANK_MIN;
    }
    if (bank > TILES_DRUM_BANK_MAX) {
        return (int8_t)TILES_DRUM_BANK_MAX;
    }
    return (int8_t)bank;
}

uint8_t tiles_drum_note(int8_t bank, uint8_t voice) {
    int note = (int)TILES_DRUM_FIRST_NOTE + 8 * (int)tiles_drum_clamp_bank(bank) + (int)(voice % TILES_DRUM_VOICES);
    return (uint8_t)note; /* 4-123 for every bank in range */
}

/* ---- pattern ---- */

void tiles_drum_pattern_clear_note(tiles_drum_pattern_t *p, uint8_t note) {
    note = (uint8_t)(note % TILES_DRUM_NOTES);
    p->armed[note] = 0u;
    for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
        p->probability[note][s] = 100u;
        p->ratchet[note][s] = 1u;
    }
}

void tiles_drum_pattern_clear(tiles_drum_pattern_t *p) {
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        tiles_drum_pattern_clear_note(p, (uint8_t)n);
    }
}

bool tiles_drum_pattern_is_armed(const tiles_drum_pattern_t *p, uint8_t note, uint8_t step) {
    return note < TILES_DRUM_NOTES && step < TILES_DRUM_STEPS && (p->armed[note] & (1ul << step)) != 0u;
}

void tiles_drum_pattern_toggle(tiles_drum_pattern_t *p, uint8_t note, uint8_t step) {
    if (note < TILES_DRUM_NOTES && step < TILES_DRUM_STEPS) {
        p->armed[note] ^= (uint32_t)(1ul << step);
    }
}

uint8_t tiles_drum_pattern_length(const tiles_drum_pattern_t *p) {
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        if ((p->armed[n] >> TILES_DRUM_PAGE_STEPS) != 0u) {
            return (uint8_t)TILES_DRUM_STEPS;
        }
    }
    return (uint8_t)TILES_DRUM_PAGE_STEPS;
}

bool tiles_drum_pattern_note_has_steps(const tiles_drum_pattern_t *p, uint8_t note) {
    return note < TILES_DRUM_NOTES && p->armed[note] != 0u;
}

void tiles_drum_pattern_set_probability(tiles_drum_pattern_t *p, uint8_t note, uint8_t step, uint8_t percent) {
    if (note < TILES_DRUM_NOTES && step < TILES_DRUM_STEPS) {
        p->probability[note][step] = percent > 100u ? 100u : percent;
    }
}

void tiles_drum_pattern_set_ratchet(tiles_drum_pattern_t *p, uint8_t note, uint8_t step, uint8_t hits) {
    if (note < TILES_DRUM_NOTES && step < TILES_DRUM_STEPS) {
        p->ratchet[note][step] = hits < 1u ? 1u : (hits > TILES_DRUM_MAX_RATCHET ? TILES_DRUM_MAX_RATCHET : hits);
    }
}

/* ---- saving ---- */

static uint32_t edited_mask(const tiles_drum_pattern_t *p, uint8_t note) {
    uint32_t mask = 0u;
    for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
        if (p->probability[note][s] != 100u || p->ratchet[note][s] != 1u) {
            mask |= (uint32_t)(1ul << s);
        }
    }
    return mask;
}

static uint8_t bits(uint32_t v) {
    uint8_t n = 0u;
    for (; v != 0u; v &= v - 1u) {
        n++;
    }
    return n;
}

static void put_u32(uint8_t *o, uint32_t v) {
    o[0] = (uint8_t)(v & 0xFFu);
    o[1] = (uint8_t)((v >> 8) & 0xFFu);
    o[2] = (uint8_t)((v >> 16) & 0xFFu);
    o[3] = (uint8_t)(v >> 24);
}

static uint32_t get_mask(const uint8_t *i, uint8_t size) {
    uint32_t v = (uint32_t)i[0] | ((uint32_t)i[1] << 8);
    if (size == 4u) {
        v |= ((uint32_t)i[2] << 16) | ((uint32_t)i[3] << 24);
    }
    return v;
}

uint16_t tiles_drum_pattern_encode(const tiles_drum_pattern_t *p, uint8_t *out, uint16_t cap, bool *truncated) {
    uint16_t len = 0u;
    *truncated = false;
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        uint32_t edited = edited_mask(p, (uint8_t)n);
        if (p->armed[n] == 0u && edited == 0u) {
            continue;
        }
        uint16_t need = (uint16_t)(9u + 2u * bits(edited));
        if ((uint32_t)len + need > cap) {
            *truncated = true;
            break;
        }
        out[len] = (uint8_t)n;
        put_u32(&out[len + 1u], p->armed[n]);
        put_u32(&out[len + 5u], edited);
        len = (uint16_t)(len + 9u);
        for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
            if ((edited >> s) & 1u) {
                out[len++] = p->probability[n][s];
                out[len++] = p->ratchet[n][s];
            }
        }
    }
    return len;
}

bool tiles_drum_pattern_decode(tiles_drum_pattern_t *p, const uint8_t *in, uint16_t len, uint16_t version) {
    tiles_drum_pattern_clear(p);
    uint8_t mask_size;
    if (version == 1u) {
        mask_size = 2u; /* 0.2.8: 16 steps */
    } else if (version == TILES_DRUM_BLOB_VERSION) {
        mask_size = 4u;
    } else {
        return false;
    }
    uint8_t head = (uint8_t)(1u + 2u * mask_size);
    uint16_t pos = 0u;
    while ((uint32_t)pos + head <= len) {
        uint8_t note = in[pos];
        uint32_t armed = get_mask(&in[pos + 1u], mask_size);
        uint32_t edited = get_mask(&in[pos + 1u + mask_size], mask_size);
        uint16_t need = (uint16_t)(head + 2u * bits(edited));
        if (note >= TILES_DRUM_NOTES || (uint32_t)pos + need > len) {
            return true; /* damaged: keep what came before */
        }
        p->armed[note] = armed;
        uint16_t at = (uint16_t)(pos + head);
        for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
            if ((edited >> s) & 1u) {
                tiles_drum_pattern_set_probability(p, note, s, in[at]);
                tiles_drum_pattern_set_ratchet(p, note, s, in[at + 1u]);
                at = (uint16_t)(at + 2u);
            }
        }
        pos = (uint16_t)(pos + need);
    }
    return true;
}

/* ---- player ---- */

void tiles_drum_player_init(tiles_drum_player_t *pl) {
    memset(pl, 0, sizeof(*pl));
}

void tiles_drum_player_start(tiles_drum_player_t *pl, bool restart) {
    pl->running = true;
    pl->pending_start = true;
    pl->pending_restart = restart;
}

void tiles_drum_player_pause(tiles_drum_player_t *pl) {
    pl->running = false;
    pl->pending_start = false;
}

void tiles_drum_player_rewind(tiles_drum_player_t *pl) {
    pl->step = 0u;
    pl->pending_start = false;
}

void tiles_drum_player_end_all(tiles_drum_player_t *pl, const tiles_drum_output_t *out) {
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        if (pl->sounding[n]) {
            out->note_off((uint8_t)n, out->ctx);
            pl->sounding[n] = false;
        }
        pl->hits_total[n] = 0u;
    }
}

bool tiles_drum_player_repeats_pending(const tiles_drum_player_t *pl) {
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        if (pl->hits_total[n] > pl->hits_done[n]) {
            return true;
        }
    }
    return false;
}

static void hit_at(tiles_drum_player_t *pl, const tiles_drum_output_t *out, uint8_t note, uint8_t velocity) {
    if (pl->sounding[note]) {
        out->note_off(note, out->ctx);
    }
    out->note_on(note, velocity, out->ctx);
    pl->sounding[note] = true;
}

static void hit(tiles_drum_player_t *pl, const tiles_drum_output_t *out, uint8_t note) {
    hit_at(pl, out, note, TILES_DRUM_STEP_VELOCITY);
    pl->hits_done[note]++;
}

void tiles_drum_player_jump(tiles_drum_player_t *pl, uint8_t step, uint32_t pulse_count) {
    pl->step = (uint8_t)(step % TILES_DRUM_STEPS);
    pl->step_started_pulse = pulse_count - pulse_count % TILES_DRUM_CLOCKS_PER_STEP;
    pl->pending_start = false;
}

void tiles_drum_player_fire_step(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat,
                                 const tiles_drum_output_t *out, uint8_t step, uint8_t velocity) {
    tiles_drum_player_end_all(pl, out);
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        if (tiles_drum_pattern_is_armed(pat, (uint8_t)n, step)) {
            hit_at(pl, out, (uint8_t)n, velocity);
        }
    }
}

/* Ends the last step's hits, then fires every armed note on `step` that
 * wins its probability roll. */
static void enter_step(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat, const tiles_drum_output_t *out,
                       uint8_t step) {
    pl->step = step;
    tiles_drum_player_end_all(pl, out);
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        pl->hits_done[n] = 0u;
        if (!tiles_drum_pattern_is_armed(pat, (uint8_t)n, step)) {
            continue;
        }
        if (out->random(out->ctx) % 100u >= pat->probability[n][step]) {
            continue; /* skipped this time */
        }
        uint8_t total = pat->ratchet[n][step];
        pl->hits_total[n] = total < 1u ? 1u : total;
        hit(pl, out, (uint8_t)n);
    }
}

void tiles_drum_player_advance(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat, const tiles_drum_output_t *out,
                               uint32_t pulse_count, bool clock_running, bool start_edge) {
    if (!pl->running) {
        if (!pl->muted) {
            tiles_drum_player_end_all(pl, out);
        }
        return;
    }
    if (pl->muted) {
        /* A step roll holds the playhead: keep the step grid moving under it,
         * the step and the notes untouched. */
        if (start_edge) {
            pl->step_started_pulse = pulse_count;
        } else if (clock_running && pulse_count - pl->step_started_pulse >= TILES_DRUM_CLOCKS_PER_STEP) {
            pl->step_started_pulse +=
                (pulse_count - pl->step_started_pulse) / TILES_DRUM_CLOCKS_PER_STEP * TILES_DRUM_CLOCKS_PER_STEP;
        }
        pl->pending_start = false;
        return;
    }
    if (start_edge) {
        /* A clock Start (or the first tap-tempo start): step 1 now. */
        pl->pending_start = false;
        pl->step_started_pulse = pulse_count;
        enter_step(pl, pat, out, 0u);
        return;
    }
    if (!clock_running) {
        /* The shared tempo stopped: silent, but still "running", so it
         * resumes with the clock. */
        tiles_drum_player_end_all(pl, out);
        return;
    }
    if (pl->pending_start) {
        /* Nearest beat: in the first half of a beat, start now on the grid of
         * the beat just passed (the first hit can be up to half a beat late);
         * past halfway, wait for the next beat. A scan that skips pulses still
         * lands in the first half after the boundary. */
        uint32_t phase = pulse_count % TILES_DRUM_CLOCKS_PER_BEAT;
        if (phase * 2u >= TILES_DRUM_CLOCKS_PER_BEAT) {
            return;
        }
        pl->pending_start = false;
        uint8_t first = pl->pending_restart ? 0u : pl->step;
        uint32_t steps_in = phase / TILES_DRUM_CLOCKS_PER_STEP;
        pl->step_started_pulse = pulse_count - phase + steps_in * TILES_DRUM_CLOCKS_PER_STEP;
        enter_step(pl, pat, out, (uint8_t)((first + steps_in) % tiles_drum_pattern_length(pat)));
        return;
    }

    /* Ratchet repeats due within the current step, before the step check so a
     * repeat right at the edge isn't lost. */
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        uint8_t total = pl->hits_total[n];
        if (total < 2u || pl->hits_done[n] >= total) {
            continue;
        }
        uint32_t interval = TILES_DRUM_CLOCKS_PER_STEP / total;
        if (interval < 1u) {
            interval = 1u;
        }
        if (pulse_count - pl->step_started_pulse >= interval * pl->hits_done[n]) {
            hit(pl, out, (uint8_t)n);
        }
    }

    uint32_t elapsed = pulse_count - pl->step_started_pulse;
    if (elapsed < TILES_DRUM_CLOCKS_PER_STEP) {
        return;
    }
    /* Several steps' worth of pulses between scans: jump, don't replay. */
    uint32_t steps = elapsed / TILES_DRUM_CLOCKS_PER_STEP;
    pl->step_started_pulse += steps * TILES_DRUM_CLOCKS_PER_STEP;
    enter_step(pl, pat, out, (uint8_t)((pl->step + steps) % tiles_drum_pattern_length(pat)));
}
