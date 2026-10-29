/*
 * services/harmony_core.c -- Harmony mode's chords (a quality plus
 * extensions), fixed voicings and strum notes. Run with
 * test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "harmony_core.h"

static bool chord_is(uint8_t quality, uint8_t ext, const int8_t *want, uint8_t n) {
    int8_t got[TILES_HARMONY_MAX_TONES];
    uint8_t count = tiles_harmony_built_chord(quality, ext, got);
    return count == n && memcmp(got, want, n) == 0;
}

static bool voiced_as(uint8_t root, const int8_t *iv, uint8_t n, uint8_t inversion, bool open,
                      const uint8_t *want) {
    tiles_harmony_voicing_t v;
    tiles_harmony_voice_fixed(root, iv, n, inversion, open, 48, &v);
    return v.count == n && memcmp(v.notes, want, n) == 0;
}

int main(void) {
    /* 1. a quality plus extensions */
    assert(chord_is(0, 0, (const int8_t[]){0, 4, 7}, 3));                               /* major */
    assert(chord_is(0, TILES_HARMONY_EXT_7, (const int8_t[]){0, 4, 7, 10}, 4));         /* 7 */
    assert(chord_is(1, TILES_HARMONY_EXT_7, (const int8_t[]){0, 3, 7, 10}, 4));         /* m7 */
    assert(chord_is(3, TILES_HARMONY_EXT_7, (const int8_t[]){0, 3, 6, 10}, 4));         /* m7b5 */
    assert(chord_is(3, TILES_HARMONY_EXT_6, (const int8_t[]){0, 3, 6, 9}, 4));          /* dim7 */
    assert(chord_is(2, TILES_HARMONY_EXT_7, (const int8_t[]){0, 5, 7, 10}, 4));         /* 7sus4 */
    assert(chord_is(0, TILES_HARMONY_EXT_MAJ7 | TILES_HARMONY_EXT_9, (const int8_t[]){0, 4, 7, 11, 14}, 5));
    assert(chord_is(0, 0xF, (const int8_t[]){0, 4, 7, 9, 10, 11, 14}, 7));              /* all four */
    assert(chord_is(9, 0, (const int8_t[]){0, 4, 7}, 3));                               /* bad quality: major */

    /* 2. voicings: root position, inversions, open */
    const int8_t triad[] = {0, 4, 7}, maj7[] = {0, 4, 7, 11};
    assert(voiced_as(0, triad, 3, 0, false, (const uint8_t[]){48, 52, 55}));
    assert(voiced_as(0, triad, 3, 1, false, (const uint8_t[]){52, 55, 60}));
    assert(voiced_as(0, triad, 3, 2, false, (const uint8_t[]){55, 60, 64}));
    assert(voiced_as(0, triad, 3, 0, true, (const uint8_t[]){48, 55, 64}));   /* root, fifth, tenth */
    assert(voiced_as(0, maj7, 4, 0, true, (const uint8_t[]){43, 48, 52, 59})); /* drop 2 */
    assert(voiced_as(9, triad, 3, 0, false, (const uint8_t[]){57, 61, 64}));  /* A in C3..B3 */

    /* 3. strum: climbs the chord's notes from the bass */
    uint16_t c_major = (1u << 0) | (1u << 4) | (1u << 7);
    assert(tiles_harmony_strum_note(c_major, 36, 0) == 36);  /* C2 */
    assert(tiles_harmony_strum_note(c_major, 36, 1) == 40);  /* E2 */
    assert(tiles_harmony_strum_note(c_major, 36, 2) == 43);  /* G2 */
    assert(tiles_harmony_strum_note(c_major, 36, 3) == 48);  /* C3 */
    assert(tiles_harmony_strum_note(c_major, 36, 11) == 79); /* G5: 12 pads span 4 octaves */
    assert(tiles_harmony_strum_note(c_major, 37, 0) == 40);  /* from between chord notes */
    assert(tiles_harmony_strum_note(0, 36, 0) == 0);
    assert(tiles_harmony_strum_note(c_major, 120, 5) == 0);  /* runs off the top */

    printf("harmony_core: all tests pass\n");
    return 0;
}
