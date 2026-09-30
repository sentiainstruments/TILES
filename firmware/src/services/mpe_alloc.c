#include "mpe_alloc.h"

#include <stddef.h>

int tiles_mpe_alloc_pick_free(const tiles_mpe_slot_t *slots, const bool *eligible, uint8_t count, uint8_t note) {
    int same_note = -1;
    int longest_idle = -1;
    for (uint8_t i = 0; i < count; i++) {
        if (!eligible[i] || slots[i].in_use) {
            continue;
        }
        /* A never-used slot has no last note; its zeroed note field must not match
         * note 0. */
        if (slots[i].release_seq != 0u && slots[i].note == note &&
            (same_note < 0 || slots[i].release_seq > slots[same_note].release_seq)) {
            same_note = (int)i;
        }
        if (longest_idle < 0 || slots[i].release_seq < slots[longest_idle].release_seq) {
            longest_idle = (int)i;
        }
    }
    return same_note >= 0 ? same_note : longest_idle;
}

int tiles_mpe_alloc_pick_steal(const tiles_mpe_slot_t *slots, const bool *eligible, const bool *pedal_held,
                               uint8_t count) {
    int best = -1;
    for (uint8_t i = 0; i < count; i++) {
        if (!eligible[i] || !slots[i].in_use || slots[i].owner_pad == 0u) {
            continue;
        }
        if (best < 0) {
            best = (int)i;
            continue;
        }
        bool i_held = pedal_held != NULL && pedal_held[i];
        bool best_held = pedal_held != NULL && pedal_held[best];
        if (i_held != best_held ? i_held : slots[i].claim_seq < slots[best].claim_seq) {
            best = (int)i;
        }
    }
    return best;
}
