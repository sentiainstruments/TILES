/*
 * services/drum_pattern.c -- the drum sequencer core: grid layout, Drum Rack
 * note map, pattern, and the step player against simulated MIDI clock.
 * Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "drum_pattern.h"

/* ---- a recording MIDI output ---- */
typedef struct {
    int on[128], off[128];
    int events;
    uint8_t last_on_note, last_velocity;
    uint32_t rng;
} rec_t;
static void rec_on(uint8_t n, uint8_t v, void *c) { rec_t *r = c; r->on[n]++; r->events++; r->last_on_note = n; r->last_velocity = v; }
static void rec_off(uint8_t n, void *c) { rec_t *r = c; r->off[n]++; r->events++; }
static uint32_t rec_rand(void *c) { rec_t *r = c; r->rng = r->rng * 1103515245u + 12345u; return (r->rng >> 8); }

static tiles_drum_pattern_t pat;
static tiles_drum_player_t pl;
static rec_t rec;
static tiles_drum_output_t out = {rec_on, rec_off, rec_rand, &rec};

static void fresh(void) {
    tiles_drum_pattern_clear(&pat);
    tiles_drum_player_init(&pl);
    memset(&rec, 0, sizeof(rec));
    rec.rng = 7u;
}

/* Feeds pulses from..to (inclusive) one per scan, clock running. */
static void run(uint32_t from, uint32_t to) {
    for (uint32_t p = from; p <= to; p++) tiles_drum_player_advance(&pl, &pat, &out, p, true, false);
}

int main(void) {
    /* 1. layout: steps on columns 1-4, drums on 5-6 */
    assert(tiles_drum_pad_for_step(0) == 1 && tiles_drum_pad_for_step(3) == 4 && tiles_drum_pad_for_step(4) == 7);
    assert(tiles_drum_pad_for_step(15) == 22);
    static const uint8_t VOICE_PADS[8] = {5, 6, 11, 12, 17, 18, 23, 24};
    for (uint8_t v = 0; v < 8; v++) {
        uint8_t back;
        assert(tiles_drum_pad_for_voice(v) == VOICE_PADS[v]);
        assert(tiles_drum_voice_for_pad(VOICE_PADS[v], &back) && back == v);
        assert(!tiles_drum_step_for_pad(VOICE_PADS[v], &back));
    }
    for (uint8_t s = 0; s < 16; s++) {
        uint8_t back, v;
        assert(tiles_drum_step_for_pad(tiles_drum_pad_for_step(s), &back) && back == s);
        assert(!tiles_drum_voice_for_pad(tiles_drum_pad_for_step(s), &v));
    }
    uint8_t dummy;
    assert(!tiles_drum_step_for_pad(0, &dummy) && !tiles_drum_voice_for_pad(25, &dummy));

    /* 2. notes: C1 = 36 on pad 5, C#1 on pad 6, D1 on pad 11 ... G1 on pad 24;
     * the next bank continues at G#1 */
    assert(tiles_drum_note(0, 0) == 36 && tiles_drum_note(0, 1) == 37 && tiles_drum_note(0, 2) == 38);
    assert(tiles_drum_note(0, 3) == 39 && tiles_drum_note(0, 7) == 43);
    assert(tiles_drum_note(1, 0) == 44 && tiles_drum_note(-1, 0) == 28);
    assert(tiles_drum_note(TILES_DRUM_BANK_MIN, 0) == 4 && tiles_drum_note(TILES_DRUM_BANK_MAX, 7) == 123);
    assert(tiles_drum_clamp_bank(99) == TILES_DRUM_BANK_MAX && tiles_drum_clamp_bank(-99) == TILES_DRUM_BANK_MIN);

    /* 3. pattern edits */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    assert(tiles_drum_pattern_is_armed(&pat, 36, 0) && tiles_drum_pattern_note_has_steps(&pat, 36));
    tiles_drum_pattern_toggle(&pat, 36, 0);
    assert(!tiles_drum_pattern_note_has_steps(&pat, 36));
    tiles_drum_pattern_set_probability(&pat, 36, 2, 250);
    tiles_drum_pattern_set_ratchet(&pat, 36, 2, 9);
    assert(pat.probability[36][2] == 100 && pat.ratchet[36][2] == TILES_DRUM_MAX_RATCHET);
    tiles_drum_pattern_set_ratchet(&pat, 36, 2, 0);
    assert(pat.ratchet[36][2] == 1);

    /* 4. four on the floor + offbeat hats: a clock Start plays step 1, then
     * one step per 6 pulses */
    fresh();
    for (uint8_t s = 0; s < 16; s += 4) tiles_drum_pattern_toggle(&pat, 36, s);   /* kick 1, 5, 9, 13 */
    for (uint8_t s = 2; s < 16; s += 4) tiles_drum_pattern_toggle(&pat, 42, s);   /* hat on the offbeats */
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);                     /* Start */
    assert(rec.on[36] == 1 && rec.on[42] == 0 && rec.last_velocity == TILES_DRUM_STEP_VELOCITY && pl.step == 0);
    run(1, 5);
    assert(rec.on[36] == 1 && rec.off[36] == 0);                                   /* still step 1 */
    run(6, 6);
    assert(pl.step == 1 && rec.off[36] == 1);                                      /* step 2 ends the kick */
    run(7, 95);                                                                    /* to the end of bar 1 */
    assert(rec.on[36] == 4 && rec.on[42] == 4 && pl.step == 15);
    run(96, 96);                                                                   /* wraps to step 1 */
    assert(pl.step == 0 && rec.on[36] == 5);

    /* 5. pulses skipped between scans: jumps, doesn't replay */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    tiles_drum_pattern_toggle(&pat, 38, 3);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    tiles_drum_player_advance(&pl, &pat, &out, 19, true, false);                   /* 3 steps at once */
    assert(pl.step == 3 && rec.on[38] == 1 && rec.on[36] == 1);

    /* 6. ratchet: 4 hits within one step, at pulses 0, 1, 2, 3 */
    fresh();
    tiles_drum_pattern_toggle(&pat, 40, 0);
    tiles_drum_pattern_set_ratchet(&pat, 40, 0, 4);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    assert(rec.on[40] == 1);
    run(1, 1); assert(rec.on[40] == 2 && rec.off[40] == 1);                       /* each repeat retriggers */
    run(2, 5); assert(rec.on[40] == 4);
    run(6, 6); assert(rec.on[40] == 4 && rec.off[40] == 4);
    /* 2 hits: at 0 and 3 */
    fresh();
    tiles_drum_pattern_toggle(&pat, 40, 0);
    tiles_drum_pattern_set_ratchet(&pat, 40, 0, 2);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    run(1, 2); assert(rec.on[40] == 1);
    run(3, 3); assert(rec.on[40] == 2);

    /* 7. probability: 0% never plays, 100% always, 50% sometimes */
    fresh();
    for (uint8_t s = 0; s < 16; s++) {
        tiles_drum_pattern_toggle(&pat, 50, s); tiles_drum_pattern_set_probability(&pat, 50, s, 0);
        tiles_drum_pattern_toggle(&pat, 51, s);
        tiles_drum_pattern_toggle(&pat, 52, s); tiles_drum_pattern_set_probability(&pat, 52, s, 50);
    }
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    run(1, 6 * 160);
    assert(rec.on[50] == 0 && rec.on[51] == 161 && rec.on[52] > 40 && rec.on[52] < 121);

    /* 8. a manual start waits for the nearest beat */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    tiles_drum_pattern_toggle(&pat, 36, 1);
    tiles_drum_player_start(&pl, true);
    run(13, 23);                                    /* second half of a beat: wait */
    assert(rec.on[36] == 0 && pl.pending_start);
    run(24, 24);                                    /* next beat: step 1 */
    assert(rec.on[36] == 1 && pl.step == 0 && pl.step_started_pulse == 24);
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    tiles_drum_pattern_toggle(&pat, 37, 1);
    tiles_drum_player_start(&pl, true);
    run(31, 31);                                    /* first half (phase 7): now, on that beat's grid */
    assert(pl.step == 1 && rec.on[37] == 1 && rec.on[36] == 0 && pl.step_started_pulse == 30);
    run(32, 36);
    assert(pl.step == 2);

    /* 9. pause resumes where it stopped; rewind goes back to step 1 */
    fresh();
    for (uint8_t s = 0; s < 16; s++) tiles_drum_pattern_toggle(&pat, 36, s);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    run(1, 30);                                     /* on step 6 */
    assert(pl.step == 5);
    tiles_drum_player_pause(&pl);
    run(31, 31);
    assert(rec.off[36] == rec.on[36]);              /* silenced */
    tiles_drum_player_start(&pl, false);
    run(32, 32);                                    /* phase 8: resumes now, one step into the beat */
    assert(pl.step == 6 && pl.step_started_pulse == 30);
    tiles_drum_player_pause(&pl);
    tiles_drum_player_rewind(&pl);
    assert(pl.step == 0);

    /* 10. the shared clock stops: silent but still running, resumes with it */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    tiles_drum_player_advance(&pl, &pat, &out, 1, false, false);
    assert(rec.off[36] == 1 && pl.running);

    /* 11. stopped: a Start from elsewhere doesn't play it */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    assert(rec.events == 0);

    printf("drum_pattern: all tests pass\n");
    return 0;
}
