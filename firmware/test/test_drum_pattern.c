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

    /* 12. saving: sparse, round-trips, clamps, survives damage */
    fresh();
    uint8_t blob[4076];
    bool truncated = true;
    assert(tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated) == 0 && !truncated);
    for (uint8_t s = 0; s < 16; s += 4) tiles_drum_pattern_toggle(&pat, 36, s);
    assert(tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated) == 9);   /* a plain beat: 9 bytes */
    tiles_drum_pattern_toggle(&pat, 42, 2);
    tiles_drum_pattern_set_probability(&pat, 42, 2, 40);
    tiles_drum_pattern_set_ratchet(&pat, 42, 9, 3);                                  /* edited but not armed: kept */
    tiles_drum_pattern_toggle(&pat, 120, 15);
    uint16_t len = tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated);
    assert(len == 9 + (9 + 4) + 9 && !truncated);
    tiles_drum_pattern_t back;
    assert(tiles_drum_pattern_decode(&back, blob, len, TILES_DRUM_BLOB_VERSION));
    assert(memcmp(&back, &pat, sizeof(pat)) == 0);
    /* truncation: stops before a note that doesn't fit, never mid-record */
    len = tiles_drum_pattern_encode(&pat, blob, 20, &truncated);
    assert(truncated && len == 9);
    /* damage: a record cut short keeps what came before */
    len = tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated);
    tiles_drum_pattern_decode(&back, blob, (uint16_t)(len - 1), TILES_DRUM_BLOB_VERSION);
    assert(back.armed[36] == pat.armed[36] && back.armed[42] == pat.armed[42] && back.armed[120] == 0);
    /* out-of-range values clamp */
    uint8_t bad[] = {50, 0x01, 0, 0, 0, 0x01, 0, 0, 0, 250, 9};
    tiles_drum_pattern_decode(&back, bad, sizeof(bad), TILES_DRUM_BLOB_VERSION);
    assert(back.armed[50] == 1 && back.probability[50][0] == 100 && back.ratchet[50][0] == TILES_DRUM_MAX_RATCHET);
    /* every reachable note with every step edited doesn't fit; the cut is clean */
    for (uint16_t n = 4; n <= 123; n++)
        for (uint8_t s = 0; s < 16; s++) { tiles_drum_pattern_toggle(&pat, (uint8_t)n, s); tiles_drum_pattern_set_ratchet(&pat, (uint8_t)n, s, 2); }
    len = tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated);
    assert(truncated && len <= sizeof(blob) && len % 41 == 0);
    /* firmware 0.2.8's version 1 (16 steps, u16 masks) still loads */
    uint8_t v1[] = {36, 0x11, 0x11, 0x04, 0x00, 60, 2,   42, 0x44, 0x44, 0x00, 0x00};
    assert(tiles_drum_pattern_decode(&back, v1, sizeof(v1), 1));
    assert(back.armed[36] == 0x1111 && back.probability[36][2] == 60 && back.ratchet[36][2] == 2 && back.armed[42] == 0x4444);
    assert(tiles_drum_pattern_length(&back) == 16);
    assert(!tiles_drum_pattern_decode(&back, v1, sizeof(v1), 99));

    /* 13. repeats pending within a step */
    fresh();
    tiles_drum_pattern_toggle(&pat, 40, 0);
    tiles_drum_pattern_set_ratchet(&pat, 40, 0, 2);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    assert(tiles_drum_player_repeats_pending(&pl));
    run(1, 3);
    assert(!tiles_drum_player_repeats_pending(&pl));

    /* 14. two pages: anything on steps 17-32 makes it a 32-step pattern */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 0);
    assert(tiles_drum_pattern_length(&pat) == 16);
    tiles_drum_pattern_toggle(&pat, 38, 20);                                      /* step 21 */
    assert(tiles_drum_pattern_length(&pat) == 32);
    assert(tiles_drum_pad_for_step(20) == tiles_drum_pad_for_step(4));           /* same pad, page 2 */
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    run(1, 6 * 20);                                                              /* to step 21 */
    assert(pl.step == 20 && rec.on[38] == 1);
    run(6 * 20 + 1, 6 * 32);                                                     /* wraps after step 32 */
    assert(pl.step == 0 && rec.on[36] == 2);
    tiles_drum_pattern_toggle(&pat, 38, 20);                                      /* page 2 empty again */
    assert(tiles_drum_pattern_length(&pat) == 16);
    len = tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated);
    tiles_drum_pattern_toggle(&pat, 40, 31);
    len = tiles_drum_pattern_encode(&pat, blob, sizeof(blob), &truncated);
    assert(tiles_drum_pattern_decode(&back, blob, len, TILES_DRUM_BLOB_VERSION) && back.armed[40] == 0x80000000u);

    /* 15. step roll, slip style: fire_step sounds every drum on the step
     * (chance ignored); while muted the pattern runs on silently and leaves
     * the roll's notes alone; on release it plays on from where it got to */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 9);
    tiles_drum_pattern_toggle(&pat, 42, 9);
    tiles_drum_pattern_set_probability(&pat, 42, 9, 0);
    tiles_drum_pattern_toggle(&pat, 38, 10);
    tiles_drum_pattern_toggle(&pat, 40, 12);
    tiles_drum_player_start(&pl, true);
    tiles_drum_player_advance(&pl, &pat, &out, 0, true, true);
    run(1, 20);                                      /* on step 4 */
    pl.muted = true;
    tiles_drum_player_fire_step(&pl, &pat, &out, 9, 77);
    assert(rec.on[36] == 1 && rec.on[42] == 1 && rec.last_velocity == 77);
    run(21, 66);                                     /* the pattern passes step 11 silently */
    assert(pl.step == 11 && rec.on[38] == 0 && rec.off[36] == 0);
    tiles_drum_player_fire_step(&pl, &pat, &out, 9, 77);
    assert(rec.on[36] == 2 && rec.off[36] == 1);    /* a retrigger ends the last hit */
    pl.muted = false;
    run(67, 72);                                     /* released: on from step 13, where it got to */
    assert(pl.step == 12 && rec.on[40] == 1 && rec.on[38] == 0);
    /* stopped, the idle player doesn't cut the roll */
    fresh();
    tiles_drum_pattern_toggle(&pat, 36, 3);
    pl.muted = true;
    tiles_drum_player_fire_step(&pl, &pat, &out, 3, 100);
    tiles_drum_player_advance(&pl, &pat, &out, 105, false, false);
    assert(rec.on[36] == 1 && rec.off[36] == 0);

    printf("drum_pattern: all tests pass\n");
    return 0;
}
