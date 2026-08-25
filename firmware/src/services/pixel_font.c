#include "pixel_font.h"

#include <stddef.h>

/* bit0 = row1 (top) ... bit3 = row4 (bottom), one byte per column --
 * see the header for the format. Every glyph below is exactly 4
 * columns wide except SPACE. Drawn out as a 4x4 grid in each comment
 * ('#' = lit) so the bit values can be checked by eye. */

/* .##.    #..#    ####    #..# */
static const uint8_t COLS_A[] = {14u, 5u, 5u, 14u};
/* ###.    #.#.    ###.    #.## */
static const uint8_t COLS_B[] = {15u, 5u, 15u, 8u};
/* .###    #...    #...    .### */
static const uint8_t COLS_C[] = {6u, 9u, 9u, 9u};
/* ###.    #..#    #..#    ###. */
static const uint8_t COLS_D[] = {15u, 9u, 9u, 6u};
/* ####    #...    ###.    #### */
static const uint8_t COLS_E[] = {15u, 13u, 13u, 9u};
/* ####    #...    ###.    #... */
static const uint8_t COLS_F[] = {15u, 5u, 5u, 1u};
/* .###    #...    #.##    .### */
static const uint8_t COLS_G[] = {6u, 9u, 13u, 13u};
/* ####    .##.    .##.    #### */
static const uint8_t COLS_I[] = {9u, 15u, 15u, 9u};
/* #...    #...    #...    #### */
static const uint8_t COLS_L[] = {15u, 8u, 8u, 8u};
/* #..#    ##.#    #.##    #..# */
static const uint8_t COLS_N[] = {15u, 2u, 4u, 15u};
/* .###    #...    ...#    ###. */
static const uint8_t COLS_S[] = {10u, 9u, 9u, 5u};
/* ####    .##.    .##.    .##. */
static const uint8_t COLS_T[] = {1u, 15u, 15u, 1u};
/* ....    ....    ####    .... */
static const uint8_t COLS_DASH[] = {4u, 4u, 4u, 4u};
static const uint8_t COLS_SPACE[] = {0u, 0u};

const tiles_glyph_t TILES_GLYPH_A = {COLS_A, 4u};
const tiles_glyph_t TILES_GLYPH_B = {COLS_B, 4u};
const tiles_glyph_t TILES_GLYPH_C = {COLS_C, 4u};
const tiles_glyph_t TILES_GLYPH_D = {COLS_D, 4u};
const tiles_glyph_t TILES_GLYPH_E = {COLS_E, 4u};
const tiles_glyph_t TILES_GLYPH_F = {COLS_F, 4u};
const tiles_glyph_t TILES_GLYPH_G = {COLS_G, 4u};
const tiles_glyph_t TILES_GLYPH_I = {COLS_I, 4u};
const tiles_glyph_t TILES_GLYPH_L = {COLS_L, 4u};
const tiles_glyph_t TILES_GLYPH_N = {COLS_N, 4u};
const tiles_glyph_t TILES_GLYPH_S = {COLS_S, 4u};
const tiles_glyph_t TILES_GLYPH_T = {COLS_T, 4u};
const tiles_glyph_t TILES_GLYPH_DASH = {COLS_DASH, 4u};
const tiles_glyph_t TILES_GLYPH_SPACE = {COLS_SPACE, 2u};

const tiles_glyph_t *tiles_pixel_font_glyph_for_note_letter(char letter) {
    switch (letter) {
    case 'A':
        return &TILES_GLYPH_A;
    case 'B':
        return &TILES_GLYPH_B;
    case 'C':
        return &TILES_GLYPH_C;
    case 'D':
        return &TILES_GLYPH_D;
    case 'E':
        return &TILES_GLYPH_E;
    case 'F':
        return &TILES_GLYPH_F;
    case 'G':
        return &TILES_GLYPH_G;
    default:
        return NULL;
    }
}
