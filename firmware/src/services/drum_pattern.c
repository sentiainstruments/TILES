#include "drum_pattern.h"

#include <string.h>

#define GRID_COLS 6u
#define STEP_COLS 4u

/* ---- layout ---- */

uint8_t tiles_drum_pad_for_step(uint8_t step) {
    step = (uint8_t)(step % TILES_DRUM_STEPS);
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
    return note < TILES_DRUM_NOTES && step < TILES_DRUM_STEPS && (p->armed[note] & (1u << step)) != 0u;
}

void tiles_drum_pattern_toggle(tiles_drum_pattern_t *p, uint8_t note, uint8_t step) {
    if (note < TILES_DRUM_NOTES && step < TILES_DRUM_STEPS) {
        p->armed[note] ^= (uint16_t)(1u << step);
    }
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

static uint16_t edited_mask(const tiles_drum_pattern_t *p, uint8_t note) {
    uint16_t mask = 0u;
    for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
        if (p->probability[note][s] != 100u || p->ratchet[note][s] != 1u) {
            mask |= (uint16_t)(1u << s);
        }
    }
    return mask;
}

uint16_t tiles_drum_pattern_encode(const tiles_drum_pattern_t *p, uint8_t *out, uint16_t cap, bool *truncated) {
    uint16_t len = 0u;
    *truncated = false;
    for (uint16_t n = 0; n < TILES_DRUM_NOTES; n++) {
        uint16_t edited = edited_mask(p, (uint8_t)n);
        if (p->armed[n] == 0u && edited == 0u) {
            continue;
        }
        uint16_t need = 5u;
        for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
            need = (uint16_t)(need + ((edited >> s) & 1u) * 2u);
        }
        if ((uint32_t)len + need > cap) {
            *truncated = true;
            break;
        }
        out[len++] = (uint8_t)n;
        out[len++] = (uint8_t)(p->armed[n] & 0xFFu);
        out[len++] = (uint8_t)(p->armed[n] >> 8);
        out[len++] = (uint8_t)(edited & 0xFFu);
        out[len++] = (uint8_t)(edited >> 8);
        for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
            if ((edited >> s) & 1u) {
                out[len++] = p->probability[n][s];
                out[len++] = p->ratchet[n][s];
            }
        }
    }
    return len;
}

void tiles_drum_pattern_decode(tiles_drum_pattern_t *p, const uint8_t *in, uint16_t len) {
    tiles_drum_pattern_clear(p);
    uint16_t pos = 0u;
    while ((uint32_t)pos + 5u <= len) {
        uint8_t note = in[pos];
        uint16_t armed = (uint16_t)(in[pos + 1u] | (in[pos + 2u] << 8));
        uint16_t edited = (uint16_t)(in[pos + 3u] | (in[pos + 4u] << 8));
        uint16_t need = 5u;
        for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
            need = (uint16_t)(need + ((edited >> s) & 1u) * 2u);
        }
        if (note >= TILES_DRUM_NOTES || (uint32_t)pos + need > len) {
            return; /* damaged: keep what came before */
        }
        p->armed[note] = armed;
        uint16_t at = (uint16_t)(pos + 5u);
        for (uint8_t s = 0; s < TILES_DRUM_STEPS; s++) {
            if ((edited >> s) & 1u) {
                tiles_drum_pattern_set_probability(p, note, s, in[at]);
                tiles_drum_pattern_set_ratchet(p, note, s, in[at + 1u]);
                at = (uint16_t)(at + 2u);
            }
        }
        pos = (uint16_t)(pos + need);
    }
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

static void hit(tiles_drum_player_t *pl, const tiles_drum_output_t *out, uint8_t note) {
    if (pl->sounding[note]) {
        out->note_off(note, out->ctx);
    }
    out->note_on(note, TILES_DRUM_STEP_VELOCITY, out->ctx);
    pl->sounding[note] = true;
    pl->hits_done[note]++;
}

/* Ends the last step's hits, then fires every armed note on `step` that
 * wins its probability roll. */
static void enter_step(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat, const tiles_drum_output_t *out,
                       uint8_t step) {
    tiles_drum_player_end_all(pl, out);
    pl->step = step;
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
        tiles_drum_player_end_all(pl, out);
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
        enter_step(pl, pat, out, (uint8_t)((first + steps_in) % TILES_DRUM_STEPS));
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
    enter_step(pl, pat, out, (uint8_t)((pl->step + steps) % TILES_DRUM_STEPS));
}
