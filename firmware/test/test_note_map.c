/*
 * Host-buildable test for the pad -> MIDI note layout. Verifies it
 * matches the exact examples given when the layout was specified:
 * pad 19 = C (lowest), 20 = C#, 21 = D, 22 = D#, 23 = E, 24 = F, then
 * wrapping to the row above: 13 = F#, 14 = G -- and extrapolates the
 * same bottom-to-top, left-to-right pattern through all 24 pads.
 *
 *   cc -std=c11 -I../src/board -I../src/services test_note_map.c \
 *     ../src/board/pad_config.c ../src/services/note_map.c -o /tmp/test_note_map
 *   /tmp/test_note_map
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

int main(void) {
    CHECK(tiles_note_map_get_scale() == TILES_SCALE_CHROMATIC, "default scale should be chromatic");

    check_explicit_examples();
    check_full_layout();
    check_endpoints();
    check_all_unique();
    check_out_of_range();

    if (g_failures == 0) {
        printf("PASS: note map layout (%u pads checked)\n", (unsigned)TILES_NUM_PADS);
        return 0;
    }

    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
