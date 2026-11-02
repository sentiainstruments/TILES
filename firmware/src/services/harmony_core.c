#include "harmony_core.h"

uint8_t tiles_harmony_built_chord(uint8_t quality, uint8_t ext_mask, int8_t *intervals_out) {
    static const int8_t TRIADS[TILES_HARMONY_NUM_QUALITIES][2] = {
        {4, 7}, /* major */
        {3, 7}, /* minor */
        {5, 7}, /* sus4 */
        {3, 6}, /* diminished (with 6: dim7) */
    };
    if (quality >= TILES_HARMONY_NUM_QUALITIES) {
        quality = 0u;
    }
    uint8_t n = 0u;
    intervals_out[n++] = 0;
    intervals_out[n++] = TRIADS[quality][0];
    intervals_out[n++] = TRIADS[quality][1];
    /* Already ascending: 9, 10, 11, 14 all sit above the triad. */
    if (ext_mask & TILES_HARMONY_EXT_6) {
        intervals_out[n++] = 9;
    }
    if (ext_mask & TILES_HARMONY_EXT_7) {
        intervals_out[n++] = 10;
    }
    if (ext_mask & TILES_HARMONY_EXT_MAJ7) {
        intervals_out[n++] = 11;
    }
    if (ext_mask & TILES_HARMONY_EXT_9) {
        intervals_out[n++] = 14;
    }
    return n;
}

void tiles_harmony_voice_fixed(uint8_t root_pc, const int8_t *intervals, uint8_t count, uint8_t inversion,
                               bool open, uint8_t low, tiles_harmony_voicing_t *out) {
    out->count = 0u;
    if (count == 0u || count > TILES_HARMONY_MAX_TONES) {
        return;
    }
    root_pc = (uint8_t)(root_pc % 12u);
    int base = (int)low + (((int)root_pc - (int)low) % 12 + 12) % 12; /* the root's note in low..low+11 */
    int notes[TILES_HARMONY_MAX_TONES];
    for (uint8_t i = 0; i < count; i++) {
        notes[i] = base + intervals[i] + (i < inversion && i < count - 1u ? 12 : 0);
    }
    for (uint8_t pass = 0; pass < 2u; pass++) {
        for (uint8_t i = 1; i < count; i++) {
            for (uint8_t j = i; j > 0 && notes[j] < notes[j - 1u]; j--) {
                int tmp = notes[j];
                notes[j] = notes[j - 1u];
                notes[j - 1u] = tmp;
            }
        }
        if (pass == 0u && open && count == 3u) {
            notes[1] += 12; /* a triad opens to root, fifth, tenth */
        } else if (pass == 0u && open && count > 3u) {
            notes[count - 2u] -= 12; /* drop 2 */
        } else {
            break;
        }
    }
    for (uint8_t i = 0; i < count; i++) {
        if (notes[i] < 0 || notes[i] > 127) {
            return;
        }
        out->notes[i] = (uint8_t)notes[i];
    }
    out->count = count;
}

uint8_t tiles_harmony_strum_note(uint16_t pc_mask, uint8_t from, uint8_t step) {
    if ((pc_mask & 0x0FFFu) == 0u) {
        return 0u;
    }
    for (unsigned note = from; note < 128u; note++) {
        if ((pc_mask >> (note % 12u)) & 1u) {
            if (step == 0u) {
                return (uint8_t)note;
            }
            step--;
        }
    }
    return 0u;
}
