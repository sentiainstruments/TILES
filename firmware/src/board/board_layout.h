#pragma once

/* The board as one 5-row x 6-col grid, for modules that use it as a
 * low-res display (standby, boot animation): row 0 = the 6 function
 * buttons, rows 1-4 = the pads (pad_config.c numbering), plus 4 underglow
 * pixels anchored under pads 3, 5, 15 and 17.
 *
 * The underglow anchors come from how the board looks, not a hardware
 * doc; fix them here if the LED order turns out different. */

#include <stdint.h>

#define TILES_GRID_MIN_ROW 0u /* function buttons */
#define TILES_GRID_MAX_ROW 4u /* pad row 4, the bottom row */
#define TILES_GRID_MIN_COL 1u
#define TILES_GRID_MAX_COL 6u

typedef struct {
    uint8_t row;
    uint8_t col;
} tiles_grid_point_t;

/* logical_pad = (row-1)*6 + col, rows 1-4, cols 1-6 (the fixed row-major
 * layout of this board). Not valid for row 0 (buttons). */
static inline uint8_t board_pad_for_row_col(uint8_t row, uint8_t col) {
    return (uint8_t)((row - 1u) * 6u + col);
}

/* SW1-SW6 sit left to right above pad columns 1-6, so button id == col. */
static inline uint8_t board_button_for_col(uint8_t col) {
    return col;
}

/* SW6 (circle), rightmost: shift. Also used by single-button indicators. */
#define TILES_CIRCLE_BUTTON_ID 6u
#define TILES_CIRCLE_BUTTON_COL 6u

/* SW5 (square), "sentia": pitch bend toggle; hold for the expression
 * menu (services/expression_control.h). */
#define TILES_SQUARE_BUTTON_ID 5u
#define TILES_SQUARE_BUTTON_COL 5u

/* SW4 (diamond): DAW transport in play modes, pattern bank in sequencer
 * (services/op_mode.h owns the roles; roles have been swapped with
 * triangle before, the physical ids never change). */
#define TILES_DIAMOND_BUTTON_ID 4u
#define TILES_DIAMOND_BUTTON_COL 4u

/* SW3 (triangle): mode menu; with circle, the scale picker. */
#define TILES_TRIANGLE_BUTTON_ID 3u
#define TILES_TRIANGLE_BUTTON_COL 3u

/* SW1 "-" / SW2 "+", leftmost: octave shift (services/octave_control.h)
 * except in sequencer mode, where op_mode.c uses them for start/stop and,
 * with circle, pattern length. Shared names for op_mode.c;
 * octave_control.c keeps its own private copies of the same ids. */
#define TILES_MINUS_BUTTON_ID 1u
#define TILES_MINUS_BUTTON_COL 1u
#define TILES_PLUS_BUTTON_ID 2u
#define TILES_PLUS_BUTTON_COL 2u

#define TILES_NUM_UNDERGLOW_ANCHORS 4u

static const tiles_grid_point_t g_tiles_underglow_anchor[TILES_NUM_UNDERGLOW_ANCHORS] = {
    {1u, 3u}, /* under pad 3 */
    {1u, 5u}, /* under pad 5 */
    {3u, 3u}, /* under pad 15 */
    {3u, 5u}, /* under pad 17 */
};

/* Underglow position around the loop, indexed by chain order. The chain
 * zigzags (UL, UR, LL, LR); clockwise is UL, UR, LR, LL. For animations
 * that travel around the perimeter. */
static const uint8_t g_tiles_underglow_circular_position[TILES_NUM_UNDERGLOW_ANCHORS] = {0u, 1u, 3u, 2u};
