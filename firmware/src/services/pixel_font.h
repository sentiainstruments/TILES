#pragma once

/*
 * Shared tiny pixel font -- 4 rows tall (one pixel per pad row 1-4),
 * variable width per glyph, monochrome (a glyph is just which pixels
 * are lit; callers apply their own color/brightness). Used by anything
 * that draws text or a single big letter across the pad grid:
 * services/standby.c's scrolling marquee animation, and
 * services/octave_control.c's transpose-mode key-letter display.
 *
 * Format: each glyph is an array of column bytes, one byte per column,
 * left to right; bit0 = row 1 (top) ... bit3 = row 4 (bottom).
 * Hand-designed specifically for 4 rows -- there's no 5th row on this
 * board to borrow from, so this isn't an off-the-shelf font shrunk
 * down. Only the letters actually needed exist: A-G (the seven natural
 * note names, for the transpose key display) plus I/L/N/S/T (for
 * "SENTIA - TILES -"), a dash, and a blank space. Reworked from an
 * earlier version that lived duplicated inside standby.c and had at
 * least one real mistake (E and F were nearly indistinguishable, E was
 * missing its bottom bar) -- pulled out into its own module so both
 * callers share one already-checked set of glyphs instead of each
 * hand-guessing their own.
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
