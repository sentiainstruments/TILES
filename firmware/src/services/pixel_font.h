#pragma once

/* Tiny pixel font for the pad grid: 4x4 per glyph (one pixel per pad row,
 * 4 columns), monochrome (callers pick the color). Used by the standby
 * marquee (services/standby.c) and the transpose key letter
 * (services/octave_control.c).
 *
 * Format: 4 column bytes per glyph, left to right; bit0 = row 1 (top) ...
 * bit3 = row 4. Monospaced: narrow letters (I, T) leave columns dark.
 *
 * Hand-drawn in the style of the "FOUR BIT" pixel font, since no existing
 * font is designed for 4 rows. Only what's used exists: A-G (note names),
 * I/L/S/T (the "TILES -" marquee), a dash and a space. */

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
