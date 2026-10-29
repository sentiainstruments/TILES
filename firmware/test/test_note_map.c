/*
 * services/note_map.c -- the default (chromatic) pad -> note layout:
 * pad 19 = C (lowest), 20 = C#, ... 24 = F, then the row above:
 * 13 = F#, 14 = G, and so on bottom-to-top, left-to-right through all
 * 24 pads. Run with test/run.sh.
 */

#include <stdio.h>

#include "note_map.h"
#include "pad_config.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (!(cond)) {                                     \
            fprintf(stderr, "FAIL: " __VA_ARGS__);          \
            fprintf(stderr, "\n");                          \
            g_failures++;                                   \
        }                                                    \
    } while (0)

static void check_explicit_examples(void) {
    /* Given directly: pad 19 = C, 20 = C#, 21 = D, 22 = D#, 23 = E,
     * 24 = F, 13 = F#, 14 = G. */
    static const struct {
        uint8_t pad;
        int8_t semitone_from_base;
        const char *name;
    } examples[] = {
        {19, 0, "C"}, {20, 1, "C#"}, {21, 2, "D"}, {22, 3, "D#"}, {23, 4, "E"}, {24, 5, "F"},
        {13, 6, "F#"}, {14, 7, "G"},
    };

    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        uint8_t expected = (uint8_t)(TILES_NOTE_MAP_BASE_NOTE + examples[i].semitone_from_base);
        uint8_t actual = tiles_note_map_get_note(examples[i].pad);
        CHECK(actual == expected, "pad %u (%s): expected note %u, got %u", examples[i].pad, examples[i].name,
              expected, actual);
    }
}

static void check_full_layout(void) {
    /* Bottom-to-top, left-to-right: row 4 (pads 19-24) is degree 0-5,
     * row 3 (13-18) is 6-11, row 2 (7-12) is 12-17, row 1 (1-6) is
     * 18-23. Every pad should be BASE_NOTE + degree. */
    for (uint8_t row = 1; row <= 4; row++) {
        for (uint8_t col = 1; col <= 6; col++) {
            uint8_t pad = (uint8_t)((row - 1) * 6 + col);
            uint8_t musical_row = (uint8_t)(4 - row);
            uint8_t degree = (uint8_t)(musical_row * 6 + (col - 1));
            uint8_t expected = (uint8_t)(TILES_NOTE_MAP_BASE_NOTE + degree);
            uint8_t actual = tiles_note_map_get_note(pad);
            CHECK(actual == expected, "pad %u (row %u col %u): expected note %u (degree %u), got %u", pad, row, col,
                  expected, degree, actual);
        }
    }
}

static void check_endpoints(void) {
    /* pad 19 (bottom-left) is the lowest note; pad 6 (top-right) is
     * the highest -- NOT pad 1, since row 1 walks left-to-right same
     * as every other row (pad 1 = F#, pad 6 = B, per the explicit
     * F#-on-13/G-on-14 pattern continuing up two more rows). */
    CHECK(tiles_note_map_get_note(19) == TILES_NOTE_MAP_BASE_NOTE, "pad 19 should be the base note (lowest)");

    uint8_t lowest = 255u;
    uint8_t highest = 0u;
    for (uint8_t pad = 1; pad <= TILES_NUM_PADS; pad++) {
        uint8_t n = tiles_note_map_get_note(pad);
        if (n < lowest) {
            lowest = n;
        }
        if (n > highest) {
            highest = n;
        }
    }
    CHECK(lowest == TILES_NOTE_MAP_BASE_NOTE, "lowest note across all pads should be the base note, got %u", lowest);
    CHECK(highest == TILES_NOTE_MAP_BASE_NOTE + 23, "highest note should be base+23 (2 octaves - 1 semitone), got %u",
          highest);
}

static void check_all_unique(void) {
    for (uint8_t i = 1; i <= TILES_NUM_PADS; i++) {
        for (uint8_t j = (uint8_t)(i + 1); j <= TILES_NUM_PADS; j++) {
            CHECK(tiles_note_map_get_note(i) != tiles_note_map_get_note(j), "pads %u and %u share the same note", i,
                  j);
        }
    }
}

static void check_out_of_range(void) {
    CHECK(tiles_note_map_get_note(0) == 0, "pad 0 should return 0");
    CHECK(tiles_note_map_get_note(25) == 0, "pad 25 should return 0");
}

/* The third (colour scheme role): the major third (pitch class 4 above C,
 * key offset 0) when the scale has one, else the minor third (3). */
static void check_third_in(tiles_scale_mode_t scale, uint8_t want_pc, const char *name) {
    tiles_note_map_set_scale(scale);
    unsigned found = 0u;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool is_third = tiles_note_map_is_third_pad(pad);
        bool want = (tiles_note_map_get_note(pad) % 12u) == want_pc;
        CHECK(is_third == want, "%s: pad %u third=%d, want %d", name, (unsigned)pad, is_third, want);
        found += is_third ? 1u : 0u;
    }
    CHECK(found > 0u, "%s: no third pad lit", name);
}

static void check_thirds(void) {
    check_third_in(TILES_SCALE_CHROMATIC, 4u, "chromatic");
    check_third_in(TILES_SCALE_IONIAN, 4u, "ionian");
    check_third_in(TILES_SCALE_AEOLIAN, 3u, "aeolian");
    check_third_in(TILES_SCALE_PENTATONIC_MINOR, 3u, "pentatonic minor");
    check_third_in(TILES_SCALE_WHOLE_TONE, 4u, "whole tone");
    check_third_in(TILES_SCALE_DIMINISHED, 3u, "diminished");
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);
}

/* The columns layout: chromatic runs on column after column; a 7-note
 * scale starts each column on the root (the top root repeated at the next
 * column's foot); a pentatonic repeats a few more. */
static void check_columns_layout(void) {
    static const uint8_t ORDER[24] = {19, 20, 13, 14, 7, 8, 1, 2, 21, 22, 15, 16, 9, 10, 3, 4, 23, 24, 17, 18, 11, 12, 5, 6};
    tiles_note_map_set_layout(TILES_LAYOUT_COLUMNS);
    CHECK(tiles_note_map_get_layout() == TILES_LAYOUT_COLUMNS, "layout should be columns");
    for (uint8_t i = 0; i < 24u; i++) {
        uint8_t want = (uint8_t)(TILES_NOTE_MAP_BASE_NOTE + i);
        uint8_t got = tiles_note_map_get_note(ORDER[i]);
        CHECK(got == want, "columns chromatic: pad %u expected %u, got %u", ORDER[i], want, got);
    }
    tiles_note_map_set_scale(TILES_SCALE_IONIAN);
    static const uint8_t IONIAN_COL1[8] = {0, 2, 4, 5, 7, 9, 11, 12};
    for (uint8_t i = 0; i < 8u; i++) {
        uint8_t got = tiles_note_map_get_note(ORDER[i]);
        CHECK(got == TILES_NOTE_MAP_BASE_NOTE + IONIAN_COL1[i], "columns ionian: pad %u got %u", ORDER[i], got);
    }
    CHECK(tiles_note_map_get_note(21) == TILES_NOTE_MAP_BASE_NOTE + 12u, "column 2 starts on the octave root");
    CHECK(tiles_note_map_get_note(2) == tiles_note_map_get_note(21), "column 1's top root repeats at column 2's foot");
    CHECK(tiles_note_map_get_note(23) == TILES_NOTE_MAP_BASE_NOTE + 24u, "column 3 starts two octaves up");
    CHECK(tiles_note_map_get_note(6) == TILES_NOTE_MAP_BASE_NOTE + 36u, "three octaves in all");
    static const uint8_t ROOTS[6] = {19, 2, 21, 4, 23, 6};
    for (uint8_t i = 0; i < 6u; i++) {
        CHECK(tiles_note_map_is_root_pad(ROOTS[i]), "columns ionian: pad %u should be a root", ROOTS[i]);
    }
    CHECK(!tiles_note_map_is_root_pad(20), "columns ionian: pad 20 (D) isn't a root");
    tiles_note_map_set_scale(TILES_SCALE_PENTATONIC_MAJOR);
    CHECK(tiles_note_map_get_note(21) == TILES_NOTE_MAP_BASE_NOTE + 12u, "pentatonic: column 2 starts on the root");
    CHECK(tiles_note_map_get_note(2) == TILES_NOTE_MAP_BASE_NOTE + 16u, "pentatonic: column 1 runs past the octave (E')");
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);
    tiles_note_map_set_layout(TILES_LAYOUT_ROWS);
}

/* Fourths, Keys, Fifths, Tonnetz, Quarter tones; and layouts only where
 * enabled (melodic mode). */
static void check_more_layouts(void) {
    uint8_t B = TILES_NOTE_MAP_BASE_NOTE;
    tiles_note_map_set_layout(TILES_LAYOUT_FOURTHS);
    CHECK(tiles_note_map_get_note(19) == B && tiles_note_map_get_note(13) == B + 5u, "fourths: a row up is +5");
    tiles_note_map_set_scale(TILES_SCALE_IONIAN);
    CHECK(tiles_note_map_get_note(13) == B + 5u, "fourths in key: a row up is 3 steps (F)");
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);

    tiles_note_map_set_layout(TILES_LAYOUT_KEYS);
    CHECK(tiles_note_map_get_note(19) == B && tiles_note_map_get_note(20) == B + 2u, "keys: white row C D");
    CHECK(tiles_note_map_get_note(13) == B + 1u, "keys: C# above C");
    CHECK(!tiles_note_map_pad_has_note(15), "keys: no key above E");
    CHECK(tiles_note_map_pad_has_note(16) && tiles_note_map_get_note(16) == B + 6u, "keys: F# above F");
    CHECK(tiles_note_map_get_note(7) == B + 11u && tiles_note_map_get_note(8) == B + 12u, "keys: upper pair starts B C");
    CHECK(!tiles_note_map_pad_has_note(1), "keys: no key above B");
    CHECK(tiles_note_map_is_root_pad(19) && tiles_note_map_is_root_pad(8), "keys: both Cs are roots");
    tiles_note_map_set_scale(TILES_SCALE_IONIAN);
    CHECK(!tiles_note_map_is_natural_pad(13), "keys in C major: C# dark");
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);

    tiles_note_map_set_layout(TILES_LAYOUT_FIFTHS);
    CHECK(tiles_note_map_get_note(19) == B - 12u && tiles_note_map_get_note(13) == B - 5u, "fifths: an octave down, a row up is +7");

    tiles_note_map_set_layout(TILES_LAYOUT_TONNETZ);
    uint8_t c = tiles_note_map_get_note(19), g = tiles_note_map_get_note(20), e = tiles_note_map_get_note(13);
    CHECK((g - c) % 12u == 7u && (e - c) % 12u == 4u, "tonnetz: right a fifth, up a major third (C, G, E: a C major L)");

    tiles_note_map_set_layout(TILES_LAYOUT_QUARTER_TONES);
    CHECK(tiles_note_map_get_note(19) == B && tiles_note_map_get_pad_cents(19) == 0u, "quarter tones: pad 19 C");
    CHECK(tiles_note_map_get_note(20) == B && tiles_note_map_get_pad_cents(20) == 50u, "quarter tones: pad 20 C + 50 cents");
    CHECK(tiles_note_map_get_note(21) == B + 1u && tiles_note_map_get_pad_cents(21) == 0u, "quarter tones: pad 21 C#");
    CHECK(!tiles_note_map_is_root_pad(20), "quarter tones: a quarter tone is no root");

    tiles_note_map_set_layout_enabled(false);
    CHECK(tiles_note_map_get_note(14) == B + 7u && tiles_note_map_get_pad_cents(20) == 0u, "disabled: Rows (pad 14 G)");
    tiles_note_map_set_layout_enabled(true);
    tiles_note_map_set_layout(TILES_LAYOUT_ROWS);
}

/* Chord mode's strip has its own octave; the melody grid keeps the main one. */
static void check_chord_octave(void) {
    tiles_note_map_set_chord_mode(true);
    uint8_t chord[TILES_NOTE_MAP_CHORD_NUM_NOTES], chord_up[TILES_NOTE_MAP_CHORD_NUM_NOTES];
    tiles_note_map_get_chord_notes(19, chord);
    uint8_t melody = tiles_note_map_get_note(21);
    tiles_note_map_set_chord_octave_shift(1);
    tiles_note_map_get_chord_notes(19, chord_up);
    CHECK(chord_up[0] == chord[0] + 12u, "chord octave +1 moves the chord up an octave");
    CHECK(tiles_note_map_get_note(21) == melody, "chord octave leaves the melody alone");
    tiles_note_map_set_chord_octave_shift(0);
    tiles_note_map_set_octave_shift(1);
    tiles_note_map_get_chord_notes(19, chord_up);
    CHECK(chord_up[0] == chord[0], "the melody octave leaves the chords alone");
    CHECK(tiles_note_map_get_note(21) == melody + 12u, "the melody octave moves the melody");
    tiles_note_map_set_octave_shift(0);
    tiles_note_map_set_chord_mode(false);
}

int main(void) {
    CHECK(tiles_note_map_get_scale() == TILES_SCALE_CHROMATIC, "default scale should be chromatic");

    check_explicit_examples();
    check_full_layout();
    check_endpoints();
    check_all_unique();
    check_out_of_range();
    check_thirds();
    check_columns_layout();
    check_more_layouts();
    check_chord_octave();

    if (g_failures == 0) {
        printf("PASS: note map layout (%u pads checked)\n", (unsigned)TILES_NUM_PADS);
        return 0;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
