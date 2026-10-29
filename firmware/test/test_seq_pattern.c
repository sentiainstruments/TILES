/*
 * services/seq_pattern.c -- the sequencer's autosave blob: what goes in
 * comes back (steps, pitches, chords, length, chance, repeats, the playing
 * patterns), empty patterns cost nothing, a full blob is cut whole, and
 * damaged data is survived. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "seq_pattern.h"

static op_seq_pattern_t a[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
static op_seq_pattern_t b[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
static uint8_t blob[4096];

static void reset_all(op_seq_pattern_t p[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE]) {
    for (uint8_t l = 0; l < OP_SEQ_NUM_LANES; l++) {
        for (uint8_t k = 0; k < OP_SEQ_ALTS_PER_LANE; k++) {
            tiles_seq_pattern_reset(&p[l][k]);
        }
    }
}

static bool same(const op_seq_pattern_t *x, const op_seq_pattern_t *y) {
    if (x->length != y->length || x->probability_enabled != y->probability_enabled) {
        return false;
    }
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if (x->step_armed[i] != y->step_armed[i] || x->step_pitch_override[i] != y->step_pitch_override[i] ||
            x->step_probability_percent[i] != y->step_probability_percent[i] ||
            x->step_ratchet_count[i] != y->step_ratchet_count[i]) {
            return false;
        }
        if (x->step_pitch_override[i]) {
            if (x->step_note_count[i] != y->step_note_count[i] ||
                memcmp(x->step_notes[i], y->step_notes[i], x->step_note_count[i]) != 0) {
                return false;
            }
        }
    }
    return true;
}

int main(void) {
    bool truncated;
    uint8_t playing[OP_SEQ_NUM_LANES] = {0, 3, 0, 5}, playing2[OP_SEQ_NUM_LANES], viewed = 0;

    /* 1. Empty: just the header */
    reset_all(a);
    uint16_t len = tiles_seq_encode(a, playing, 2, blob, sizeof(blob), &truncated);
    assert(len == 5u && !truncated);
    assert(tiles_seq_decode(b, playing2, &viewed, blob, len, TILES_SEQ_BLOB_VERSION));
    assert(viewed == 2 && playing2[1] == 3 && playing2[3] == 5);

    /* 2. Round trip: steps, single pitches, a chord, a pitch kept on an
     * unarmed step, length 16, chance and repeats */
    reset_all(a);
    op_seq_pattern_t *p = &a[1][3];
    p->length = 16;
    for (uint8_t i = 0; i < 16; i += 2) {
        p->step_armed[i] = true;
        p->step_pitch_override[i] = true;
        p->step_notes[i][0] = (uint8_t)(48 + i);
        p->step_note_count[i] = 1;
    }
    p->step_notes[4][1] = 55;
    p->step_notes[4][2] = 60;
    p->step_note_count[4] = 3;
    p->step_pitch_override[9] = true; /* toggled off, keeps its note */
    p->step_notes[9][0] = 62;
    p->step_note_count[9] = 1;
    p->probability_enabled = true;
    p->step_probability_percent[2] = 40;
    p->step_ratchet_count[6] = 3;
    a[0][5].length = 7; /* not playing, still saved */
    len = tiles_seq_encode(a, playing, 1, blob, sizeof(blob), &truncated);
    assert(!truncated && len < 120u);
    assert(tiles_seq_decode(b, playing2, &viewed, blob, len, TILES_SEQ_BLOB_VERSION));
    for (uint8_t l = 0; l < OP_SEQ_NUM_LANES; l++) {
        for (uint8_t k = 0; k < OP_SEQ_ALTS_PER_LANE; k++) {
            assert(same(&a[l][k], &b[l][k]));
        }
    }
    assert(viewed == 1 && b[1][3].length == 16 && b[0][5].length == 7);

    /* 3. Fingerprint: changes with a pitch, a length, the playing pattern */
    uint32_t f = tiles_seq_fingerprint(a, playing, 1);
    a[1][3].step_notes[0][0]++;
    assert(tiles_seq_fingerprint(a, playing, 1) != f);
    a[1][3].step_notes[0][0]--;
    assert(tiles_seq_fingerprint(a, playing, 1) == f);
    a[2][0].length = 12;
    assert(tiles_seq_fingerprint(a, playing, 1) != f);
    a[2][0].length = OP_SEQ_NUM_STEPS;
    playing[2] = 1;
    assert(tiles_seq_fingerprint(a, playing, 1) != f);
    playing[2] = 0;

    /* 4. Too small a buffer: cut before a whole pattern, the playing one kept */
    len = tiles_seq_encode(a, playing, 1, blob, 50, &truncated);
    assert(truncated && len <= 50u);
    assert(tiles_seq_decode(b, playing2, &viewed, blob, len, TILES_SEQ_BLOB_VERSION));
    assert(same(&a[1][3], &b[1][3]) == (len > 5u));

    /* 5. Damaged or foreign data: clamped or stopped, never past the end */
    len = tiles_seq_encode(a, playing, 1, blob, sizeof(blob), &truncated);
    blob[5] = 200; /* a bad index */
    assert(tiles_seq_decode(b, playing2, &viewed, blob, len, TILES_SEQ_BLOB_VERSION));
    memset(blob, 0xFF, sizeof(blob));
    assert(tiles_seq_decode(b, playing2, &viewed, blob, 300, TILES_SEQ_BLOB_VERSION));
    assert(viewed == 0 && playing2[0] == 0);
    assert(!tiles_seq_decode(b, playing2, &viewed, blob, 3, TILES_SEQ_BLOB_VERSION));
    assert(!tiles_seq_decode(b, playing2, &viewed, blob, 300, 99));

    printf("seq_pattern: all tests pass\n");
    return 0;
}
