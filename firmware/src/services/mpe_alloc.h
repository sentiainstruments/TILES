#pragma once

/*
 * MPE Member Channel choice -- which channel a new note goes on, and which
 * note gives up its channel when every one is busy. Pure functions over a
 * slot table (no MIDI I/O, no Pico SDK), tested natively in
 * firmware/test/test_mpe_alloc.c; services/expression.c owns the table and
 * does the actual claiming/sending.
 *
 * Real feedback: "what else does it look like we need to fix for
 * standarization and compatibility" -> "do all". This used to hand a new
 * note the LOWEST free channel. The MPE specification (MMA RP-053 v1.0,
 * section 3.2) asks for something else: put a new note on a channel with the
 * fewest active notes, preferably the one that just ended the SAME note
 * number, otherwise the one whose last note ended longest ago. Lowest-first
 * kept reusing channel 2 for every new note, so each new note landed on the
 * channel whose previous note was still in its release tail (or still being
 * held by the synth's sustain pedal): the synth either cut that tail short
 * or, where it lets a channel stack voices, applied the new note's pitch
 * bend and pressure to the old one too. Longest-idle-first gives every
 * released note the most time to finish before its channel is reused, and
 * the same-note rule means a re-struck note retriggers its own voice instead
 * of stacking a second copy on another channel ("stacking and chorusing
 * identical notes" -- the spec's own warning).
 *
 * TILES never puts two notes on one Member Channel, so "fewest active notes"
 * is simply "free".
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool in_use;
    /* 1..24 for a played note; 0 for a harmonic pluck (services/expression.c's
     * harmonics), which is never stolen -- it ends on its own timer. Valid
     * only while in_use. */
    uint8_t owner_pad;
    /* The note this channel carries now, or carried last once freed -- what
     * the same-note rule compares against. */
    uint8_t note;
    /* When this channel was claimed; the oldest claim is the steal victim. */
    uint32_t claim_seq;
    /* When this channel was last freed; 0 = never used since boot, which
     * counts as the longest idle of all. */
    uint32_t release_seq;
} tiles_mpe_slot_t;

/* Picks a FREE slot for `note` among slots[0..count) where eligible[i] is
 * true: first a free slot whose last note was this same note (the most
 * recently freed one if several), otherwise the free slot freed longest ago,
 * lowest index on a tie. Returns the index, or -1 if no eligible slot is
 * free. */
int tiles_mpe_alloc_pick_free(const tiles_mpe_slot_t *slots, const bool *eligible, uint8_t count, uint8_t note);

/* Picks the slot to STEAL when tiles_mpe_alloc_pick_free() found nothing:
 * among eligible, in-use slots owned by a played note (owner_pad != 0), a
 * note TILES is only holding for the pedal (pedal_held[i] -- no finger on
 * it) before any note still being played, then the oldest claim. Returns the
 * index, or -1 if nothing is stealable. `pedal_held` may be NULL (no slot is
 * pedal-held). */
int tiles_mpe_alloc_pick_steal(const tiles_mpe_slot_t *slots, const bool *eligible, const bool *pedal_held,
                               uint8_t count);
