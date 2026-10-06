#pragma once

/* MPE Member Channel choice: which channel a new note gets, and which note
 * gives up its channel when all are busy. Pure functions over a slot
 * table, tested natively (firmware/test/test_mpe_alloc.c);
 * services/expression.c owns the table and does the sending.
 *
 * Follows MMA RP-053 v1.0 section 3.2: a new note goes on a free channel,
 * preferring the one that last carried the SAME note (so a re-struck note
 * retriggers its own voice instead of stacking a copy), otherwise the one
 * idle longest (so release tails and pedal-held notes finish before their
 * channel is reused). Lowest-free-first, the old rule, kept landing new
 * notes on channels still ringing, cutting tails or bending them.
 *
 * TILES never puts two notes on one channel, so "fewest active notes" is
 * simply "free". */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool in_use;
    /* 1..24 for a played note; 0 for a harmonic pluck, which is never stolen
     * (it ends on its own timer). Valid only while in_use. */
    uint8_t owner_pad;
    /* The note on this channel now, or last, once freed (for the same-note
     * rule). */
    uint8_t note;
    /* When this channel was claimed; the oldest claim is the steal victim. */
    uint32_t claim_seq;
    /* When last freed; 0 = never used, which counts as idle longest. */
    uint32_t release_seq;
} tiles_mpe_slot_t;

/* Picks a FREE eligible slot for `note`: the one whose last note was the
 * same (most recently freed if several), else the one freed longest ago,
 * lowest index on a tie. -1 if none is free. */
int tiles_mpe_alloc_pick_free(const tiles_mpe_slot_t *slots, const bool *eligible, uint8_t count, uint8_t note);

/* Picks the slot to STEAL when none is free: among eligible, in-use slots
 * owned by a played note (owner_pad != 0), a note held only by the pedal
 * (pedal_held[i], no finger) before one still being played, then the
 * oldest claim. -1 if nothing is stealable. `pedal_held` may be NULL. */
int tiles_mpe_alloc_pick_steal(const tiles_mpe_slot_t *slots, const bool *eligible, const bool *pedal_held,
                               uint8_t count);
