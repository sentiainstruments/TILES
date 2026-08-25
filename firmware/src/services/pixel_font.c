#include "pixel_font.h"

#include <stddef.h>

/* bit0 = row1 (top) ... bit3 = row4 (bottom), one byte per column --
 * see the header for the format. */
static const uint8_t COLS_A[] = {14u, 5u, 14u};
static const uint8_t COLS_B[] = {15u, 5u, 10u};
static const uint8_t COLS_C[] = {15u, 9u, 9u};
static const uint8_t COLS_D[] = {15u, 9u, 6u};
static const uint8_t COLS_E[] = {15u, 13u, 9u};
static const uint8_t COLS_F[] = {15u, 5u, 1u};
static const uint8_t COLS_G[] = {15u, 9u, 13u};
static const uint8_t COLS_I[] = {15u};
static const uint8_t COLS_L[] = {15u, 8u, 8u};
static const uint8_t COLS_N[] = {15u, 6u, 15u};
static const uint8_t COLS_S[] = {11u, 9u, 13u};
static const uint8_t COLS_T[] = {1u, 15u, 1u};
static const uint8_t COLS_DASH[] = {4u, 4u, 4u};
static const uint8_t COLS_SPACE[] = {0u, 0u};

const tiles_glyph_t TILES_GLYPH_A = {COLS_A, 3u};
const tiles_glyph_t TILES_GLYPH_B = {COLS_B, 3u};
const tiles_glyph_t TILES_GLYPH_C = {COLS_C, 3u};
const tiles_glyph_t TILES_GLYPH_D = {COLS_D, 3u};
const tiles_glyph_t TILES_GLYPH_E = {COLS_E, 3u};
const tiles_glyph_t TILES_GLYPH_F = {COLS_F, 3u};
const tiles_glyph_t TILES_GLYPH_G = {COLS_G, 3u};
const tiles_glyph_t TILES_GLYPH_I = {COLS_I, 1u};
const tiles_glyph_t TILES_GLYPH_L = {COLS_L, 3u};
const tiles_glyph_t TILES_GLYPH_N = {COLS_N, 3u};
const tiles_glyph_t TILES_GLYPH_S = {COLS_S, 3u};
const tiles_glyph_t TILES_GLYPH_T = {COLS_T, 3u};
const tiles_glyph_t TILES_GLYPH_DASH = {COLS_DASH, 3u};
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
