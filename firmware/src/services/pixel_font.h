#pragma once

/*
 * Shared tiny pixel font -- a fixed 4x4 grid per glyph (one pixel per
 * pad row 1-4, 4 columns wide), monochrome (a glyph is just which
 * pixels are lit; callers apply their own color/brightness). Used by
 * anything that draws text or a single big letter across the pad grid:
 * services/standby.c's scrolling marquee animation, and
 * services/octave_control.c's transpose-mode key-letter display.
 *
 * Format: each glyph is an array of 4 column bytes, left to right;
 * bit0 = row 1 (top) ... bit3 = row 4 (bottom). Every glyph is exactly
 * 4 columns wide -- true monospacing, letters that don't need the full
 * width (I, T) just leave their unused columns dark rather than the
 * previous version's per-glyph variable widths + explicit gap columns.
 *
 * Styled after the reference "FOUR BIT" pixel font (bold, blocky,
 * geometric strokes) -- hand-drawn to fit this board's actual 4x4
 * constraint rather than an off-the-shelf font shrunk down, since no
 * existing font is designed for exactly 4 rows. Reworked once already:
 * the original version used variable-width 3-column glyphs with a
 * separate gap column between letters; this version moved to a fixed
 * 4x4 grid per the "FOUR BIT" reference and widened N's diagonal (the
 * extra column made a real diagonal possible instead of the old
 * H-like compromise). Only the letters actually needed exist: A-G (the
 * seven natural note names, for the transpose key display) plus
 * I/L/N/S/T (for "SENTIA - TILES -"), a dash, and a blank space.
 */

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
extern const tiles_glyph_t TILES_GLYPH_N;
extern const tiles_glyph_t TILES_GLYPH_S;
extern const tiles_glyph_t TILES_GLYPH_T;
extern const tiles_glyph_t TILES_GLYPH_DASH;
extern const tiles_glyph_t TILES_GLYPH_SPACE;

/* Looks up one of the seven natural-note letter glyphs above (uppercase
 * 'A'-'G' only) -- convenience for a caller keying off a runtime note
 * letter (services/octave_control.c's transpose display) instead of a
 * fixed message. Returns NULL for anything else. */
const tiles_glyph_t *tiles_pixel_font_glyph_for_note_letter(char letter);
