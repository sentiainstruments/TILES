#pragma once

/* 4x4 pixel font for the pad grid (marquee, transpose letter): 4 column
 * bytes per glyph, bit0 = top row. Hand-drawn; only A-G, I/L/S/T, dash and
 * space exist. */

#include <stdint.h>

typedef struct {
    const uint8_t *cols;
    uint8_t width;
} tiles_glyph_t;

extern const tiles_glyph_t TILES_GLYPH_A;
extern const tiles_glyph_t TILES_GLYPH_B;
extern const tiles_glyph_t TILES_GLYPH_C;
extern const tiles_glyph_t TILES_GLYPH_D;
extern const tiles_glyph_t TILES_GLYPH_E;
extern const tiles_glyph_t TILES_GLYPH_F;
extern const tiles_glyph_t TILES_GLYPH_G;
extern const tiles_glyph_t TILES_GLYPH_I;
extern const tiles_glyph_t TILES_GLYPH_L;
extern const tiles_glyph_t TILES_GLYPH_S;
extern const tiles_glyph_t TILES_GLYPH_T;
extern const tiles_glyph_t TILES_GLYPH_DASH;
extern const tiles_glyph_t TILES_GLYPH_SPACE;

/* Glyph for a natural note letter ('A'-'G', uppercase), for runtime
 * lookups (transpose display). NULL for anything else. */
const tiles_glyph_t *tiles_pixel_font_glyph_for_note_letter(char letter);
