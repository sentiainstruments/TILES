#pragma once

/* MPE Member Channel choice (RP-053 3.2), pure and tested natively: a free
 * channel, preferring the one that last carried the same note, else the
 * one idle longest (so tails finish); when full, steal the oldest. */

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
