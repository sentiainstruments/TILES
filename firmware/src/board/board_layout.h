#pragma once

/*
 * Shared logical-grid layout: how the 6 function buttons + 24 pads + 4
 * underglow pixels map onto one unified 5-row x 6-col coordinate space
 * (row 0 = buttons, physically just above pad row 1; rows 1-4 = the pad
 * grid, matching pad_config.c's row numbering). Used by every module
 * that treats the board as one low-res animated display:
 * services/standby.c, services/boot_sequence.c, and any future
 * power-saving-mode indicator.
 *
 * The underglow anchor points ("under pad 3, pad 5, pad 15, pad 17", in
 * SK6805 chain order) are based on the user's verbal description of the
 * physical board, not a hardware doc (confirmed absent from
 * docs/hardware/) -- easy to correct here if the real LED1-4 order
 * turns out different once seen lit.
 */

#include <stdint.h>

#define TILES_GRID_MIN_ROW 0u /* function buttons */
#define TILES_GRID_MAX_ROW 4u /* pad row 4, the bottom row */
#define TILES_GRID_MIN_COL 1u
#define TILES_GRID_MAX_COL 6u

typedef struct {
    uint8_t row;
    uint8_t col;
} tiles_grid_point_t;

/* logical_pad = (row-1)*6 + col for row 1-4, col 1-6 -- the row-major
 * layout pad_config.c documents and its table's literal data confirms,
 * not re-derived via a table lookup since it's a fixed invariant of
 * this board revision. Only valid for row >= 1 (row 0 is buttons, see
 * board_button_for_col() instead). */
static inline uint8_t board_pad_for_row_col(uint8_t row, uint8_t col) {
    return (uint8_t)((row - 1u) * 6u + col);
}

/* SW1-SW6 sit left-to-right directly above pad columns 1-6
 * (docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's button x_mm order,
 * verified against services/buttons.c's own comment) -- a clean 1:1
 * mapping, so button id == col. */
static inline uint8_t board_button_for_col(uint8_t col) {
    return col;
}

/* SW6 (circle), the rightmost function button -- used by any indicator
 * that highlights just one button (e.g. power-saving mode). */
#define TILES_CIRCLE_BUTTON_ID 6u
#define TILES_CIRCLE_BUTTON_COL 6u

/* SW5 (square), one left of circle -- "sentia" button, a secondary
 * modifier for expression features (services/expression_control.h). */
#define TILES_SQUARE_BUTTON_ID 5u
#define TILES_SQUARE_BUTTON_COL 5u

/* SW4 (diamond), two left of circle -- operation-mode select
 * (services/op_mode.h): a single click toggles the mode-picker menu (or
 * exits back to melodic play). */
#define TILES_DIAMOND_BUTTON_ID 4u
#define TILES_DIAMOND_BUTTON_COL 4u

/* SW3 (triangle), three left of circle -- each operation mode's own
 * sub-menu (services/op_mode.h): a single click toggles that mode's
 * sub-menu (melodic's is the scale picker; other modes don't have one
 * yet). */
#define TILES_TRIANGLE_BUTTON_ID 3u
#define TILES_TRIANGLE_BUTTON_COL 3u

/* SW2 ("+")/SW1 ("-"), the leftmost two -- services/octave_control.h's
 * default octave-shift function everywhere except sequencer mode, where
 * services/op_mode.h repurposes them as transport (start/stop) and,
 * combined with circle held, pattern length. octave_control.c keeps its
 * own private BUTTON_ID_MINUS/_PLUS #defines (identical values) rather
 * than migrating to these -- these exist purely so op_mode.c has a
 * shared name for the same two physical buttons without duplicating a
 * second private definition of its own. */
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

/* Underglow pixels in their actual physical (geometric) circular order
 * rather than SK6805 chain order -- chain order 0,1,2,3 zigzags
 * diagonally (upper-left -> upper-right -> lower-left -> lower-right);
 * going clockwise around the perimeter is 0 (upper-left) -> 1
 * (upper-right) -> 3 (lower-right) -> 2 (lower-left). Index this by
 * chain-order pixel index to get that pixel's position (0-3) in the
 * circular sweep -- used by anything animating a wave traveling around
 * the loop rather than along the chain. */
static const uint8_t g_tiles_underglow_circular_position[TILES_NUM_UNDERGLOW_ANCHORS] = {0u, 1u, 3u, 2u};
