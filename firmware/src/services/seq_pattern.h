#pragma once

/* The sequencer's pattern and its autosave blob (tested natively). Blob,
 * little-endian, sparse:
 *   u8 viewed lane, u8 playing pattern per lane (4), then per pattern:
 *   u8 index, u8 length, u8 flags, u32 armed, u32 has-pitch, u32 edited,
 *   per has-pitch step: u8 count + notes; per edited step: chance, repeats.
 * Encoding stops before a pattern that won't fit (`truncated`); decoding
 * resets, clamps and stops at a damaged record. */

#include <stdbool.h>
#include <stdint.h>

#define OP_SEQ_NUM_STEPS 24u /* one step per pad */
#define OP_SEQ_NUM_LANES 4u
#define OP_SEQ_ALTS_PER_LANE 6u
/* Up to 4 hits per step, each at least one clock pulse apart. */
#define OP_SEQ_MAX_RATCHET 4u
/* Up to 4 notes per step, enough to capture a chord (bass, root, fifth,
 * third; OP_CHORD_NUM_VOICES). */
#define OP_SEQ_MAX_NOTES_PER_STEP 4u

/* step_note_count[i] == 0 means no note, even if armed (tap-to-arm arms an
 * empty step until a pitch is assigned); op_mode.c's seq_fire_note()
 * treats it as silent. */
typedef struct {
    bool step_armed[OP_SEQ_NUM_STEPS];
    bool step_pitch_override[OP_SEQ_NUM_STEPS];
    /* The step's notes, used only if step_pitch_override[i]. */
    uint8_t step_notes[OP_SEQ_NUM_STEPS][OP_SEQ_MAX_NOTES_PER_STEP];
    uint8_t step_note_count[OP_SEQ_NUM_STEPS];
    uint8_t step_probability_percent[OP_SEQ_NUM_STEPS]; /* 0-100, default 100; used only if probability_enabled */
    uint8_t step_ratchet_count[OP_SEQ_NUM_STEPS];    /* 1..OP_SEQ_MAX_RATCHET, default 1 (none) */
    /* Per-pattern probability switch: while false, every step fires as if at
     * 100%. Turned on when any step's probability is edited
     * (handle_edit_mode()); nothing turns it back off. */
    bool probability_enabled;
    uint8_t length; /* 1..24 active steps */
    /* No channel (that's a lane property) and no scale: patterns follow the
     * one global scale, applied when a note plays, without changing stored
     * notes. */
} op_seq_pattern_t;

/* Empty: nothing armed, no pitches, chance 100, no repeats, 24 steps. */
void tiles_seq_pattern_reset(op_seq_pattern_t *p);

#define TILES_SEQ_BLOB_VERSION 1u

uint16_t tiles_seq_encode(const op_seq_pattern_t pats[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE],
                          const uint8_t playing[OP_SEQ_NUM_LANES], uint8_t viewed_lane, uint8_t *out, uint16_t cap,
                          bool *truncated);

/* False for an unknown version or a blob too short for its header
 * (nothing changed then). */
bool tiles_seq_decode(op_seq_pattern_t pats[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE], uint8_t playing[OP_SEQ_NUM_LANES],
                      uint8_t *viewed_lane, const uint8_t *in, uint16_t len, uint16_t version);

/* A fingerprint of every pattern, the playing ones and the viewed lane
 * (FNV-1a): op_mode.c compares it to notice any change, whatever made it. */
uint32_t tiles_seq_fingerprint(const op_seq_pattern_t pats[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE],
                               const uint8_t playing[OP_SEQ_NUM_LANES], uint8_t viewed_lane);
