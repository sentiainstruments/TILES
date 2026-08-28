#include "game_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression_control.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "op_mode.h"
#include "touch.h"

#include "pico/rand.h"
#include "pico/time.h"

#include <math.h>
#include <stdlib.h>

#define GAME_MODE_PI 3.14159265358979323846f

#define GM_HOLD_MS 700u /* how long the 4-button combo must be held to toggle */
#define GM_FRAME_INTERVAL_MS 40u
#define GM_ROUND_END_FLASH_MS 2200u
#define GM_ROUND_END_TOGGLE_MS 260u

typedef enum {
    GM_STATE_OFF = 0,
    GM_STATE_MENU,
    GM_STATE_PLAYING_SNAKE,
    GM_STATE_PLAYING_BRICK,
    GM_STATE_PLAYING_TETRIS,
    GM_STATE_PLAYING_PONG,
    GM_STATE_PLAYING_SIMON,
    GM_STATE_ROUND_END,
} gm_state_t;

static gm_state_t s_gm_state;
static uint32_t s_gm_last_frame_ms;
static uint32_t s_gm_round_end_ms;
static bool s_gm_round_end_red_only;

static bool s_gm_combo_was_held;
static bool s_gm_combo_fired;
static uint32_t s_gm_hold_start_ms;

static bool s_gm_prev_pad1_touched;
static bool s_gm_prev_pad2_touched;
static bool s_gm_prev_pad3_touched;
static bool s_gm_prev_pad4_touched;
static bool s_gm_prev_pad5_touched;

/* ---- Interactive snake ---------------------------------------------------
 * Player-controlled version of standby.c's autonomous snake -- separate
 * state, deliberately not shared with it (see game_mode.h's file
 * header). */

#define GS_MAX_LENGTH 20u
#define GS_STEP_MS 350u

typedef struct {
    int8_t row;
    int8_t col;
} gs_cell_t;

static gs_cell_t s_gs_body[GS_MAX_LENGTH]; /* [0] = head */
static uint8_t s_gs_length;
static int8_t s_gs_dir_row;
static int8_t s_gs_dir_col;
static int8_t s_gs_pending_dir_row;
static int8_t s_gs_pending_dir_col;
static gs_cell_t s_gs_food;
static uint32_t s_gs_last_step_ms;
static bool s_gs_prev_left;
static bool s_gs_prev_right;
static bool s_gs_prev_up;
static bool s_gs_prev_down;

static bool gs_cell_in_body(int8_t row, int8_t col) {
    for (uint8_t i = 0; i < s_gs_length; i++) {
        if (s_gs_body[i].row == row && s_gs_body[i].col == col) {
            return true;
        }
    }
    return false;
}

static void gs_place_food(void) {
    for (uint8_t attempt = 0; attempt < 50u; attempt++) {
        int8_t r = (int8_t)(TILES_GRID_MIN_ROW + (rand() % (TILES_GRID_MAX_ROW - TILES_GRID_MIN_ROW + 1u)));
        int8_t c = (int8_t)(TILES_GRID_MIN_COL + (rand() % (TILES_GRID_MAX_COL - TILES_GRID_MIN_COL + 1u)));
        if (!gs_cell_in_body(r, c)) {
            s_gs_food.row = r;
            s_gs_food.col = c;
            return;
        }
    }
    s_gs_food.row = (int8_t)TILES_GRID_MIN_ROW;
    s_gs_food.col = (int8_t)TILES_GRID_MIN_COL;
}

static void gs_start(uint32_t now_ms) {
    /* Real feedback: "check the seed for all games" -- see
     * gsim_new_game()'s own comment (and standby.c's deeper fix) for why
     * this matters; every player-started game now reseeds with fresh
     * hardware entropy right as it begins, not just Simon Says. */
    srand((unsigned int)get_rand_32());
    /* 2, not 3 -- real feedback that 3 felt cramped starting out given
     * how little space this board actually has (5x6 cells total). */
    s_gs_length = 2u;
    int8_t start_row = 2;
    int8_t start_col = 3;
    s_gs_dir_row = 0;
    s_gs_dir_col = 1; /* start heading right */
    s_gs_pending_dir_row = s_gs_dir_row;
    s_gs_pending_dir_col = s_gs_dir_col;
    for (uint8_t i = 0; i < s_gs_length; i++) {
        s_gs_body[i].row = start_row;
        s_gs_body[i].col = (int8_t)(start_col - (int8_t)i);
    }
    gs_place_food();
    s_gs_last_step_ms = now_ms;
    s_gs_prev_left = false;
    s_gs_prev_right = false;
    s_gs_prev_up = false;
    s_gs_prev_down = false;
}

/* red_only: Tetris topping out flashes plain red (real feedback: "when
 * game is lost it should flash red"), while snake/brick breaker keep
 * the original red/purple alternation -- see render_round_end() below. */
static void gm_start_round_end(uint32_t now_ms, bool red_only) {
    s_gm_state = GM_STATE_ROUND_END;
    s_gm_round_end_ms = now_ms;
    s_gm_round_end_red_only = red_only;
}

static void gs_try_set_direction(int8_t dr, int8_t dc) {
    /* Disallow reversing straight into the current heading -- the
     * standard "can't turn 180 into your own neck" snake-game rule. */
    bool is_reverse = (dr == (int8_t)(-s_gs_dir_row)) && (dc == (int8_t)(-s_gs_dir_col)) &&
                       (s_gs_dir_row != 0 || s_gs_dir_col != 0);
    if (is_reverse) {
        return;
    }
    s_gs_pending_dir_row = dr;
    s_gs_pending_dir_col = dc;
}

static void gs_handle_input(void) {
    bool left = tiles_button_is_pressed(1u);  /* SW1 "-" */
    bool right = tiles_button_is_pressed(2u); /* SW2 "+" */
    bool up = tiles_button_is_pressed(3u);    /* SW3 triangle */
    bool down = tiles_button_is_pressed(4u);  /* SW4 diamond */

    if (left && !s_gs_prev_left) {
        gs_try_set_direction(0, -1);
    }
    if (right && !s_gs_prev_right) {
        gs_try_set_direction(0, 1);
    }
    if (up && !s_gs_prev_up) {
        gs_try_set_direction(-1, 0);
    }
    if (down && !s_gs_prev_down) {
        gs_try_set_direction(1, 0);
    }

    s_gs_prev_left = left;
    s_gs_prev_right = right;
    s_gs_prev_up = up;
    s_gs_prev_down = down;
}

static void gs_step(uint32_t now_ms) {
    s_gs_dir_row = s_gs_pending_dir_row;
    s_gs_dir_col = s_gs_pending_dir_col;

    gs_cell_t head = s_gs_body[0];
    int8_t nr = (int8_t)(head.row + s_gs_dir_row);
    int8_t nc = (int8_t)(head.col + s_gs_dir_col);

    /* Wrap around edges -- friendlier than instant death on a wall,
     * given how small this board is. */
    if (nr < (int8_t)TILES_GRID_MIN_ROW) {
        nr = (int8_t)TILES_GRID_MAX_ROW;
    }
    if (nr > (int8_t)TILES_GRID_MAX_ROW) {
        nr = (int8_t)TILES_GRID_MIN_ROW;
    }
    if (nc < (int8_t)TILES_GRID_MIN_COL) {
        nc = (int8_t)TILES_GRID_MAX_COL;
    }
    if (nc > (int8_t)TILES_GRID_MAX_COL) {
        nc = (int8_t)TILES_GRID_MIN_COL;
    }

    if (gs_cell_in_body(nr, nc)) {
        gm_start_round_end(now_ms, false);
        return;
    }

    bool ate = (nr == s_gs_food.row && nc == s_gs_food.col);
    uint8_t new_length = ate ? (uint8_t)(s_gs_length + 1u) : s_gs_length;
    if (new_length > GS_MAX_LENGTH) {
        /* Board effectively full -- treat it as a win, same flash. */
        gm_start_round_end(now_ms, false);
        return;
    }

    for (uint8_t i = (uint8_t)(new_length - 1u); i > 0u; i--) {
        s_gs_body[i] = s_gs_body[i - 1u];
    }
    s_gs_body[0].row = nr;
    s_gs_body[0].col = nc;
    s_gs_length = new_length;

    if (ate) {
        gs_place_food();
    }
}

static void gs_update(uint32_t now_ms) {
    if (now_ms - s_gs_last_step_ms >= GS_STEP_MS) {
        gs_step(now_ms);
        s_gs_last_step_ms = now_ms;
    }
}

#define GS_HEAD_LEVEL 1.0f
#define GS_BODY_LEVEL 0.75f
#define GS_FOOD_PULSE_PERIOD_MS 700.0f
#define GS_FOOD_MIN_LEVEL 0.6f
#define GS_FOOD_MAX_LEVEL 1.0f

static void render_snake(uint32_t now_ms) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_buttons_set_standby_led(board_button_for_col(col), 0.0f);
    }

    float food_raw = 0.5f + 0.5f * sinf(2.0f * GAME_MODE_PI * (float)now_ms / GS_FOOD_PULSE_PERIOD_MS);
    float food_level = GS_FOOD_MIN_LEVEL + (GS_FOOD_MAX_LEVEL - GS_FOOD_MIN_LEVEL) * food_raw;

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            float r = 0.0f;
            float g = 0.0f;
            uint8_t pad = board_pad_for_row_col(row, col);

            if ((int8_t)row == s_gs_food.row && (int8_t)col == s_gs_food.col) {
                r = food_level;
            } else {
                for (uint8_t i = 0; i < s_gs_length; i++) {
                    if (s_gs_body[i].row == (int8_t)row && s_gs_body[i].col == (int8_t)col) {
                        g = (i == 0u) ? GS_HEAD_LEVEL : GS_BODY_LEVEL;
                        break;
                    }
                }
            }
            tiles_lighting_set_standby_pad_rgb(pad, r, g, 0.0f);
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Interactive brick breaker --------------------------------------------
 * Player-controlled version of standby.c's autonomous brick breaker --
 * same ball/brick/wall physics, paddle is player-controlled instead of
 * AI-tracked. Separate state, deliberately not shared with the
 * autonomous version. */

#define GB_NUM_COLS 6u
#define GB_PADDLE_ROW 4u
#define GB_STEP_MS 300u

static bool s_gb_brick_alive[GB_NUM_COLS];
static int8_t s_gb_ball_row;
static int8_t s_gb_ball_col;
static int8_t s_gb_ball_drow;
static int8_t s_gb_ball_dcol;
static int8_t s_gb_paddle_center; /* 2-5 */
static uint32_t s_gb_last_step_ms;
static bool s_gb_prev_left;
static bool s_gb_prev_right;

static void gb_start(uint32_t now_ms) {
    /* Real feedback: "check the seed for all games" -- see
     * gs_start()'s own comment. */
    srand((unsigned int)get_rand_32());
    for (uint8_t i = 0; i < GB_NUM_COLS; i++) {
        s_gb_brick_alive[i] = true;
    }
    s_gb_paddle_center = 3;
    s_gb_ball_row = (int8_t)(GB_PADDLE_ROW - 1u);
    s_gb_ball_col = s_gb_paddle_center;
    s_gb_ball_drow = -1;
    s_gb_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_gb_last_step_ms = now_ms;
    s_gb_prev_left = false;
    s_gb_prev_right = false;
}

static void gb_handle_input(void) {
    bool left = tiles_button_is_pressed(1u);  /* SW1 "-" */
    bool right = tiles_button_is_pressed(2u); /* SW2 "+" */

    if (left && !s_gb_prev_left) {
        s_gb_paddle_center--;
        if (s_gb_paddle_center < 2) {
            s_gb_paddle_center = 2;
        }
    }
    if (right && !s_gb_prev_right) {
        s_gb_paddle_center++;
        if (s_gb_paddle_center > 5) {
            s_gb_paddle_center = 5;
        }
    }

    s_gb_prev_left = left;
    s_gb_prev_right = right;
}

/* Real feedback: "brickbraker is having a hard time hitting all function
 * button leds, i suspect its because of the alignement" -- correctly
 * diagnosed as an alignment issue, though not a rendering one. Every step
 * used to move row by exactly +/-1 AND col by exactly +/-1 in lockstep (a
 * wall bounce flips dcol's SIGN but a step still always changes col by 1
 * either way), which makes (row + col) mod 2 an exact invariant of the
 * ball's entire trajectory. gb_start() above always starts the ball at
 * row 3, col 3 (paddle_center), an EVEN sum, so the ball could only ever
 * reach row 1 (the brick wall) on the 3 columns sharing that same parity
 * -- the other 3 bricks were mathematically unreachable every single
 * round, not just unlucky.
 *
 * First fix attempt throttled column movement to every OTHER step -- real
 * feedback after flashing it: "now moves weird and still cant reach 3 of
 * the 5 lights." It didn't actually fix reachability: row's own
 * bounce-to-bounce period is ALWAYS an even number of ticks (a fixed
 * function of GB_PADDLE_ROW, independent of column state), so jumping
 * column by a fixed 4 ticks' worth every row-bounce cycle just walks a
 * fixed stride around the column's own reflecting orbit -- landing on
 * only every other reachable column forever, same bug, different
 * numbers -- while also visibly breaking the normal diagonal motion.
 * Real fix, matching standby.c's autonomous version (bb_step()): column
 * advances every step again, but each bounce off the top wall or the
 * paddle now also gets a coin-flip chance to reverse dcol. Row's bounce
 * timing is still perfectly periodic, but column's direction at each
 * bounce is now a genuine random variable instead of a deterministic
 * function of the previous bounce, so there's no fixed relationship left
 * for a parity/stride argument to lock onto. */
static void gb_step(uint32_t now_ms) {
    int8_t new_col = (int8_t)(s_gb_ball_col + s_gb_ball_dcol);
    if (new_col < (int8_t)TILES_GRID_MIN_COL || new_col > (int8_t)TILES_GRID_MAX_COL) {
        s_gb_ball_dcol = (int8_t)(-s_gb_ball_dcol);
        new_col = (int8_t)(s_gb_ball_col + s_gb_ball_dcol);
    }
    int8_t new_row = (int8_t)(s_gb_ball_row + s_gb_ball_drow);

    if (new_row < 1) {
        uint8_t col_index = (uint8_t)(new_col - TILES_GRID_MIN_COL);
        s_gb_brick_alive[col_index] = false;
        s_gb_ball_drow = 1;
        new_row = 1;
        if ((rand() % 2) == 0) {
            s_gb_ball_dcol = (int8_t)(-s_gb_ball_dcol);
        }

        bool all_dead = true;
        for (uint8_t i = 0; i < GB_NUM_COLS; i++) {
            if (s_gb_brick_alive[i]) {
                all_dead = false;
                break;
            }
        }
        if (all_dead) {
            gm_start_round_end(now_ms, false);
        }
    } else if (new_row > (int8_t)GB_PADDLE_ROW) {
        int8_t paddle_min = (int8_t)(s_gb_paddle_center - 1);
        int8_t paddle_max = (int8_t)(s_gb_paddle_center + 1);
        if (new_col >= paddle_min && new_col <= paddle_max) {
            s_gb_ball_drow = -1;
            new_row = (int8_t)GB_PADDLE_ROW;
            if ((rand() % 2) == 0) {
                s_gb_ball_dcol = (int8_t)(-s_gb_ball_dcol);
            }
        } else {
            gm_start_round_end(now_ms, false);
        }
    }

    s_gb_ball_col = new_col;
    s_gb_ball_row = new_row;
}

static void gb_update(uint32_t now_ms) {
    if (now_ms - s_gb_last_step_ms >= GB_STEP_MS) {
        gb_step(now_ms);
        s_gb_last_step_ms = now_ms;
    }
}

#define GB_BRICK_LEVEL 0.85f
#define GB_PADDLE_LEVEL 0.9f
#define GB_BALL_LEVEL 1.0f

static void render_brick(uint32_t now_ms) {
    (void)now_ms;

    /* Bricks live at the button row -- buttons are plain monochrome
     * PWM, not addressable RGB, so an alive brick is just a bright
     * single-channel level, not the orange used for the pad-grid
     * versions of "a brick" elsewhere (e.g. standby.c's autonomous
     * version, which draws bricks on actual RGB pads and can afford
     * color; here they're on the button row instead). */
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        uint8_t idx = (uint8_t)(col - TILES_GRID_MIN_COL);
        float level = s_gb_brick_alive[idx] ? GB_BRICK_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;

            if (s_gb_ball_row >= 1 && s_gb_ball_row <= (int8_t)GB_PADDLE_ROW && (int8_t)row == s_gb_ball_row &&
                (int8_t)col == s_gb_ball_col) {
                r = GB_BALL_LEVEL;
                g = GB_BALL_LEVEL;
                b = 0.4f * GB_BALL_LEVEL;
            } else if (row == GB_PADDLE_ROW) {
                int8_t paddle_min = (int8_t)(s_gb_paddle_center - 1);
                int8_t paddle_max = (int8_t)(s_gb_paddle_center + 1);
                if ((int8_t)col >= paddle_min && (int8_t)col <= paddle_max) {
                    g = 0.6f * GB_PADDLE_LEVEL;
                    b = GB_PADDLE_LEVEL;
                }
            }
            tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Interactive Tetris ----------------------------------------------------
 * A custom small-piece set falling into a 4-row x 6-col well (the pad
 * grid; function buttons stay off, same as the other two games) --
 * NOT the standard 7 tetrominoes. Real feedback on the original
 * standard set: full tetrominoes (4 cells, up to 4 wide/tall) are too
 * big for a board this size -- a single piece could span the entire
 * width or height, leaving no room to actually play. Replaced with 5
 * smaller pieces of increasing size (GT_PIECES below): a 1-cell dot, a
 * 2-cell domino, a 3-cell straight tromino (the "long piece," capped at
 * 3 instead of 4), a 3-cell corner tromino, and a 2x2 square (4 cells,
 * but compact -- its footprint doesn't sprawl the way a 4-in-a-row
 * piece does, so it stays as the largest piece). Pieces now have a
 * variable cell count (`num_cells`, 1-4) rather than always exactly 4,
 * so every loop over a piece's cells uses that field instead of a
 * hardcoded 4.
 *
 * Only 2 rotation states per piece (not full 4-state SRS -- with only 4
 * rows of height the extra states would rarely change anything) and no
 * wall kicks (a rotation that doesn't fit in place is just rejected).
 * SW1/SW2 move left/right, SW3 rotates, SW4 hard-drops. Gravity also
 * steps the piece down automatically every GT_STEP_MS. Landing locks
 * the piece into the well; full rows shift everything above them down
 * (gt_clear_lines() below, handles multiple simultaneous clears).
 * Topping out -- a freshly spawned piece has nowhere to fit -- ends the
 * round via the same gm_start_round_end() flash every other game
 * uses. */

#define GT_MIN_ROW 1u /* row 0 is buttons, not part of the well */
#define GT_MAX_ROW TILES_GRID_MAX_ROW
#define GT_MIN_COL TILES_GRID_MIN_COL
#define GT_MAX_COL TILES_GRID_MAX_COL
#define GT_ROWS 4u
#define GT_COLS 6u
#define GT_STEP_MS 550u
#define GT_SPAWN_COL 3 /* leaves room either side for every piece's max width (3) */
#define GT_NUM_PIECE_TYPES 5u
#define GT_MAX_CELLS 4u
/* Dramatic white underglow strobe on a line clear -- fast toggle, short
 * total duration, so it reads as a flash rather than a glow. */
#define GT_LINE_CLEAR_FLASH_MS 450u
#define GT_LINE_CLEAR_TOGGLE_MS 90u

typedef struct {
    int8_t dr;
    int8_t dc;
} gt_offset_t;

/* Two rotation states per piece; only the first num_cells entries of
 * each are used (1-4, GT_MAX_CELLS) -- see the file comment above for
 * why pieces are no longer always exactly 4 cells. */
typedef struct {
    uint8_t num_cells;
    gt_offset_t state0[GT_MAX_CELLS];
    gt_offset_t state1[GT_MAX_CELLS];
    float r, g, b;
} gt_piece_def_t;

/* Small custom piece set, smallest to largest -- see the file comment
 * above for why these replace the standard 7 tetrominoes. */
static const gt_piece_def_t GT_PIECES[GT_NUM_PIECE_TYPES] = {
    /* Dot: 1 cell, no real rotation (both states identical). */
    {1u, {{0, 0}}, {{0, 0}}, 1.0f, 1.0f, 1.0f},
    /* Domino: 2 cells, horizontal/vertical. */
    {2u, {{0, 0}, {0, 1}}, {{0, 0}, {1, 0}}, 0.0f, 1.0f, 1.0f},
    /* Straight tromino ("long piece," capped at 3): horizontal/vertical. */
    {3u, {{0, 0}, {0, 1}, {0, 2}}, {{0, 0}, {1, 0}, {2, 0}}, 0.0f, 1.0f, 0.0f},
    /* Corner tromino: two different bends, not a strict rotation pair,
     * just two distinct 3-cell shapes for variety. */
    {3u, {{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}}, 1.0f, 0.5f, 0.0f},
    /* Square: 2x2, 4 cells but compact -- rotation is a no-op. */
    {4u, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 1.0f, 1.0f, 0.0f},
};

/* 0 = empty, else (piece type index + 1) -- indexed [row - GT_MIN_ROW][col - GT_MIN_COL]. */
static uint8_t s_gt_board[GT_ROWS][GT_COLS];
static uint8_t s_gt_piece_type;
static uint8_t s_gt_rotation;
static int8_t s_gt_origin_row;
static int8_t s_gt_origin_col;
static uint32_t s_gt_last_step_ms;
static uint32_t s_gt_line_clear_flash_ms;
static bool s_gt_prev_left;
static bool s_gt_prev_right;
static bool s_gt_prev_rotate;
static bool s_gt_prev_drop;

static const gt_offset_t *gt_offsets(uint8_t piece_type, uint8_t rotation) {
    return (rotation == 0u) ? GT_PIECES[piece_type].state0 : GT_PIECES[piece_type].state1;
}

static bool gt_fits(uint8_t piece_type, uint8_t rotation, int8_t origin_row, int8_t origin_col) {
    const gt_offset_t *offsets = gt_offsets(piece_type, rotation);
    uint8_t num_cells = GT_PIECES[piece_type].num_cells;
    for (uint8_t i = 0; i < num_cells; i++) {
        int8_t r = (int8_t)(origin_row + offsets[i].dr);
        int8_t c = (int8_t)(origin_col + offsets[i].dc);
        if (r < (int8_t)GT_MIN_ROW || r > (int8_t)GT_MAX_ROW) {
            return false;
        }
        if (c < (int8_t)GT_MIN_COL || c > (int8_t)GT_MAX_COL) {
            return false;
        }
        if (s_gt_board[r - (int8_t)GT_MIN_ROW][c - (int8_t)GT_MIN_COL] != 0u) {
            return false;
        }
    }
    return true;
}

static void gt_spawn(void) {
    s_gt_piece_type = (uint8_t)(rand() % GT_NUM_PIECE_TYPES);
    s_gt_rotation = 0u;
    s_gt_origin_row = (int8_t)GT_MIN_ROW;
    s_gt_origin_col = (int8_t)GT_SPAWN_COL;
}

/* Standard line-clear sweep: bottom-up, a full row shifts everything
 * above it down by one and the top row clears; the same row index is
 * rechecked (not advanced) afterward since it now holds whatever
 * shifted into it -- this is what makes multiple simultaneous clears
 * collapse correctly in one pass. Returns how many rows were cleared,
 * so gt_lock() below can trigger the line-clear flash only when
 * something actually cleared. */
static uint8_t gt_clear_lines(void) {
    uint8_t cleared = 0u;
    int8_t row = (int8_t)(GT_ROWS - 1u);
    while (row >= 0) {
        bool full = true;
        for (uint8_t c = 0; c < GT_COLS; c++) {
            if (s_gt_board[row][c] == 0u) {
                full = false;
                break;
            }
        }
        if (!full) {
            row--;
            continue;
        }
        cleared++;
        for (int8_t r = row; r > 0; r--) {
            for (uint8_t c = 0; c < GT_COLS; c++) {
                s_gt_board[r][c] = s_gt_board[r - 1][c];
            }
        }
        for (uint8_t c = 0; c < GT_COLS; c++) {
            s_gt_board[0][c] = 0u;
        }
    }
    return cleared;
}

static void gt_lock(uint32_t now_ms) {
    const gt_offset_t *offsets = gt_offsets(s_gt_piece_type, s_gt_rotation);
    uint8_t num_cells = GT_PIECES[s_gt_piece_type].num_cells;
    for (uint8_t i = 0; i < num_cells; i++) {
        int8_t r = (int8_t)(s_gt_origin_row + offsets[i].dr);
        int8_t c = (int8_t)(s_gt_origin_col + offsets[i].dc);
        s_gt_board[r - (int8_t)GT_MIN_ROW][c - (int8_t)GT_MIN_COL] = (uint8_t)(s_gt_piece_type + 1u);
    }
    if (gt_clear_lines() > 0u) {
        /* Dramatic white underglow strobe -- see render_tetris()'s
         * underglow loop below. */
        s_gt_line_clear_flash_ms = now_ms;
    }
    gt_spawn();
    if (!gt_fits(s_gt_piece_type, s_gt_rotation, s_gt_origin_row, s_gt_origin_col)) {
        /* Nowhere for the next piece to go -- topped out. Plain red,
         * not the red/purple every other game's round-end uses -- real
         * feedback: "when game is lost it should flash red." */
        gm_start_round_end(now_ms, true);
    }
}

static void gt_start(uint32_t now_ms) {
    /* Real feedback: "check the seed for all games" -- see
     * gs_start()'s own comment. */
    srand((unsigned int)get_rand_32());
    for (uint8_t r = 0; r < GT_ROWS; r++) {
        for (uint8_t c = 0; c < GT_COLS; c++) {
            s_gt_board[r][c] = 0u;
        }
    }
    gt_spawn();
    s_gt_last_step_ms = now_ms;
    /* Set to "already long past" rather than 0 -- 0 could still read as
     * "within the flash window" if this round starts within
     * GT_LINE_CLEAR_FLASH_MS of boot. */
    s_gt_line_clear_flash_ms = now_ms - GT_LINE_CLEAR_FLASH_MS - 1u;
    s_gt_prev_left = false;
    s_gt_prev_right = false;
    s_gt_prev_rotate = false;
    s_gt_prev_drop = false;
}

static void gt_handle_input(uint32_t now_ms) {
    bool left = tiles_button_is_pressed(1u);   /* SW1 "-" */
    bool right = tiles_button_is_pressed(2u);  /* SW2 "+" */
    bool rotate = tiles_button_is_pressed(3u); /* SW3 triangle */
    bool drop = tiles_button_is_pressed(4u);   /* SW4 diamond */

    if (left && !s_gt_prev_left) {
        if (gt_fits(s_gt_piece_type, s_gt_rotation, s_gt_origin_row, (int8_t)(s_gt_origin_col - 1))) {
            s_gt_origin_col--;
        }
    }
    if (right && !s_gt_prev_right) {
        if (gt_fits(s_gt_piece_type, s_gt_rotation, s_gt_origin_row, (int8_t)(s_gt_origin_col + 1))) {
            s_gt_origin_col++;
        }
    }
    if (rotate && !s_gt_prev_rotate) {
        uint8_t next_rotation = (uint8_t)(1u - s_gt_rotation);
        if (gt_fits(s_gt_piece_type, next_rotation, s_gt_origin_row, s_gt_origin_col)) {
            s_gt_rotation = next_rotation;
        }
    }
    if (drop && !s_gt_prev_drop) {
        while (gt_fits(s_gt_piece_type, s_gt_rotation, (int8_t)(s_gt_origin_row + 1), s_gt_origin_col)) {
            s_gt_origin_row++;
        }
        gt_lock(now_ms);
    }

    s_gt_prev_left = left;
    s_gt_prev_right = right;
    s_gt_prev_rotate = rotate;
    s_gt_prev_drop = drop;
}

static void gt_update(uint32_t now_ms) {
    if (now_ms - s_gt_last_step_ms < GT_STEP_MS) {
        return;
    }
    s_gt_last_step_ms = now_ms;
    if (gt_fits(s_gt_piece_type, s_gt_rotation, (int8_t)(s_gt_origin_row + 1), s_gt_origin_col)) {
        s_gt_origin_row++;
    } else {
        gt_lock(now_ms);
    }
}

#define GT_LOCKED_LEVEL 0.8f

static void render_tetris(uint32_t now_ms) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_buttons_set_standby_led(board_button_for_col(col), 0.0f);
    }

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t v = s_gt_board[row - GT_MIN_ROW][col - GT_MIN_COL];
            if (v != 0u) {
                const gt_piece_def_t *def = &GT_PIECES[v - 1u];
                tiles_lighting_set_standby_pad_rgb(pad, def->r * GT_LOCKED_LEVEL, def->g * GT_LOCKED_LEVEL,
                                                    def->b * GT_LOCKED_LEVEL);
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            }
        }
    }

    /* The falling piece draws on top, at full brightness -- a visible
     * cue distinguishing it from the already-locked stack. */
    const gt_piece_def_t *active = &GT_PIECES[s_gt_piece_type];
    const gt_offset_t *offsets = gt_offsets(s_gt_piece_type, s_gt_rotation);
    for (uint8_t i = 0; i < active->num_cells; i++) {
        int8_t r = (int8_t)(s_gt_origin_row + offsets[i].dr);
        int8_t c = (int8_t)(s_gt_origin_col + offsets[i].dc);
        if (r < 1 || r > (int8_t)TILES_GRID_MAX_ROW || c < (int8_t)TILES_GRID_MIN_COL ||
            c > (int8_t)TILES_GRID_MAX_COL) {
            continue;
        }
        tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col((uint8_t)r, (uint8_t)c), active->r, active->g,
                                            active->b);
    }

    bool flashing = (now_ms - s_gt_line_clear_flash_ms) < GT_LINE_CLEAR_FLASH_MS;
    bool flash_on = flashing && (((now_ms - s_gt_line_clear_flash_ms) / GT_LINE_CLEAR_TOGGLE_MS) % 2u) == 0u;
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (flash_on) {
            tiles_lighting_set_standby_underglow_rgb(i, 1.0f, 1.0f, 1.0f);
        } else {
            tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
        }
    }
}

/* ---- Interactive Pong -------------------------------------------------------
 * Two-player, same board: column 1 is the left paddle, column 6 is the
 * right paddle, both 2 pads tall (white); the ball is a single blue
 * dot bouncing between them. Left paddle: SW1 ("-") up, SW2 ("+") down.
 * Right paddle: SW5 (square) up, SW6 (circle) down -- SW5/SW6 chosen as
 * the mirror-image pair to SW1/SW2 (leftmost two vs. rightmost two of
 * the six buttons); flag this to the user if "square" wasn't the button
 * they meant by "the other one next to circle."
 *
 * A miss (real feedback: Pong wasn't tracking who was winning at all)
 * scores the *other* side a point and triggers a short local white
 * underglow flash (gp_point_scored()); first to GP_WIN_SCORE (2) wins
 * the match. A side's score shows on its own movement-control buttons,
 * glowing rather than flat-on: 0 points = both dark, 1 point = the
 * "up" button (SW1 left / SW5 right) glows, 2 points = both glow --
 * "one point one control lit, 2 points both buttons on." SW3/SW4 stay
 * dark, unused by Pong.
 *
 * Individual points still don't go through the shared win/lose
 * round-end machinery every other game here uses -- a rally on a board
 * this small can end in a couple of seconds, so bouncing to the menu
 * on every point would be disruptive -- the ball just re-serves
 * immediately after a non-winning miss. Reaching the winning score is
 * different: real feedback was "don't reset the game immediately,
 * return to the game menu" -- so a match win freezes the board (ball
 * and paddles stop where they are, the winner's controls glow) for
 * GP_MATCH_END_DISPLAY_MS, then returns to GM_STATE_MENU via
 * gm_enter_menu(), the same way every other game's round ends.
 * Handled locally in tiles_game_mode_scan()'s PLAYING_PONG branch
 * (checking s_gp_match_over) rather than through GM_STATE_ROUND_END,
 * since the "flash" here is on the button LEDs, not underglow. */

#define GP_MIN_ROW 1u /* row 0 is buttons, not part of the court */
#define GP_MAX_ROW TILES_GRID_MAX_ROW
#define GP_PADDLE_COL_LEFT TILES_GRID_MIN_COL
#define GP_PADDLE_COL_RIGHT TILES_GRID_MAX_COL
#define GP_PADDLE_TOP_MIN GP_MIN_ROW           /* paddle spans [top, top+1] */
#define GP_PADDLE_TOP_MAX (TILES_GRID_MAX_ROW - 1u)
#define GP_STEP_MS 260u
#define GP_POINT_FLASH_MS 500u
#define GP_POINT_FLASH_TOGGLE_MS 110u
#define GP_PADDLE_LEVEL 1.0f
#define GP_BALL_LEVEL 1.0f
#define GP_WIN_SCORE 2u
#define GP_MATCH_END_DISPLAY_MS 2500u
/* Breathing pulse for a lit score-indicator button -- "glowing," not
 * flat-on. */
#define GP_SCORE_GLOW_PERIOD_MS 900.0f
#define GP_SCORE_GLOW_MIN 0.5f
#define GP_SCORE_GLOW_MAX 1.0f

static int8_t s_gp_left_paddle_top;  /* GP_PADDLE_TOP_MIN..GP_PADDLE_TOP_MAX */
static int8_t s_gp_right_paddle_top;
static int8_t s_gp_ball_row;
static int8_t s_gp_ball_col;
static int8_t s_gp_ball_drow;
static int8_t s_gp_ball_dcol;
static uint32_t s_gp_last_step_ms;
static uint32_t s_gp_point_flash_ms;
static uint8_t s_gp_left_score;
static uint8_t s_gp_right_score;
static bool s_gp_match_over;
static uint32_t s_gp_match_over_ms;
static bool s_gp_prev_left_up;
static bool s_gp_prev_left_down;
static bool s_gp_prev_right_up;
static bool s_gp_prev_right_down;

static void gp_serve(uint32_t now_ms) {
    s_gp_ball_row = (int8_t)(GP_MIN_ROW + (rand() % (GP_MAX_ROW - GP_MIN_ROW + 1u)));
    s_gp_ball_col = ((rand() % 2) == 0) ? 3 : 4; /* the two middle columns of 1-6 */
    s_gp_ball_drow = ((rand() % 2) == 0) ? -1 : 1;
    s_gp_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_gp_last_step_ms = now_ms;
}

static void gp_start(uint32_t now_ms) {
    /* Real feedback: "check the seed for all games" -- see
     * gs_start()'s own comment. */
    srand((unsigned int)get_rand_32());
    s_gp_left_paddle_top = 2;
    s_gp_right_paddle_top = 2;
    s_gp_left_score = 0u;
    s_gp_right_score = 0u;
    s_gp_match_over = false;
    gp_serve(now_ms);
    /* "Already long past" rather than 0 -- see the same pattern/reasoning
     * on Tetris's line-clear flash above. */
    s_gp_point_flash_ms = now_ms - GP_POINT_FLASH_MS - 1u;
    s_gp_prev_left_up = false;
    s_gp_prev_left_down = false;
    s_gp_prev_right_up = false;
    s_gp_prev_right_down = false;
}


static void gp_handle_input(void) {
    bool left_up = tiles_button_is_pressed(1u);    /* SW1 "-" */
    bool left_down = tiles_button_is_pressed(2u);  /* SW2 "+" */
    bool right_up = tiles_button_is_pressed(5u);   /* SW5 square */
    bool right_down = tiles_button_is_pressed(6u); /* SW6 circle */

    if (left_up && !s_gp_prev_left_up && s_gp_left_paddle_top > (int8_t)GP_PADDLE_TOP_MIN) {
        s_gp_left_paddle_top--;
    }
    if (left_down && !s_gp_prev_left_down && s_gp_left_paddle_top < (int8_t)GP_PADDLE_TOP_MAX) {
        s_gp_left_paddle_top++;
    }
    if (right_up && !s_gp_prev_right_up && s_gp_right_paddle_top > (int8_t)GP_PADDLE_TOP_MIN) {
        s_gp_right_paddle_top--;
    }
    if (right_down && !s_gp_prev_right_down && s_gp_right_paddle_top < (int8_t)GP_PADDLE_TOP_MAX) {
        s_gp_right_paddle_top++;
    }

    s_gp_prev_left_up = left_up;
    s_gp_prev_left_down = left_down;
    s_gp_prev_right_up = right_up;
    s_gp_prev_right_down = right_down;
}

/* left_missed: true if the ball got past the left paddle (so the right
 * side scores), false if it got past the right paddle (left scores).
 * On reaching GP_WIN_SCORE, freezes the match instead of re-serving --
 * see the file header. */
static void gp_point_scored(uint32_t now_ms, bool left_missed) {
    s_gp_point_flash_ms = now_ms;
    if (left_missed) {
        s_gp_right_score++;
    } else {
        s_gp_left_score++;
    }
    if (s_gp_left_score >= GP_WIN_SCORE || s_gp_right_score >= GP_WIN_SCORE) {
        s_gp_match_over = true;
        s_gp_match_over_ms = now_ms;
        return;
    }
    gp_serve(now_ms);
}

static void gp_step(uint32_t now_ms) {
    int8_t new_row = (int8_t)(s_gp_ball_row + s_gp_ball_drow);
    if (new_row < (int8_t)GP_MIN_ROW || new_row > (int8_t)GP_MAX_ROW) {
        s_gp_ball_drow = (int8_t)(-s_gp_ball_drow);
        new_row = (int8_t)(s_gp_ball_row + s_gp_ball_drow);
    }

    int8_t new_col = (int8_t)(s_gp_ball_col + s_gp_ball_dcol);
    if (new_col < (int8_t)GP_PADDLE_COL_LEFT) {
        if (new_row >= s_gp_left_paddle_top && new_row <= (int8_t)(s_gp_left_paddle_top + 1)) {
            s_gp_ball_dcol = 1;
            new_col = (int8_t)GP_PADDLE_COL_LEFT;
        } else {
            gp_point_scored(now_ms, true);
            return;
        }
    } else if (new_col > (int8_t)GP_PADDLE_COL_RIGHT) {
        if (new_row >= s_gp_right_paddle_top && new_row <= (int8_t)(s_gp_right_paddle_top + 1)) {
            s_gp_ball_dcol = -1;
            new_col = (int8_t)GP_PADDLE_COL_RIGHT;
        } else {
            gp_point_scored(now_ms, false);
            return;
        }
    }

    s_gp_ball_row = new_row;
    s_gp_ball_col = new_col;
}

static void gp_update(uint32_t now_ms) {
    if (now_ms - s_gp_last_step_ms < GP_STEP_MS) {
        return;
    }
    s_gp_last_step_ms = now_ms;
    gp_step(now_ms);
}

/* Score indicator on each side's own movement-control buttons: 0
 * points = both dark, 1 = the "up" button glows, 2 (win) = both glow --
 * a breathing pulse, not flat-on, so it reads as "glowing." SW3/SW4
 * stay dark, unused by Pong. */
static void render_pong_score_buttons(uint32_t now_ms) {
    float raw = 0.5f + 0.5f * sinf(2.0f * GAME_MODE_PI * (float)now_ms / GP_SCORE_GLOW_PERIOD_MS);
    float glow = GP_SCORE_GLOW_MIN + (GP_SCORE_GLOW_MAX - GP_SCORE_GLOW_MIN) * raw;

    tiles_buttons_set_standby_led(1u, (s_gp_left_score >= 1u) ? glow : 0.0f);  /* SW1 "-" */
    tiles_buttons_set_standby_led(2u, (s_gp_left_score >= 2u) ? glow : 0.0f);  /* SW2 "+" */
    tiles_buttons_set_standby_led(3u, 0.0f);
    tiles_buttons_set_standby_led(4u, 0.0f);
    tiles_buttons_set_standby_led(5u, (s_gp_right_score >= 1u) ? glow : 0.0f); /* SW5 square */
    tiles_buttons_set_standby_led(6u, (s_gp_right_score >= 2u) ? glow : 0.0f); /* SW6 circle */
}

static void render_pong(uint32_t now_ms) {
    render_pong_score_buttons(now_ms);

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;

            if ((int8_t)row == s_gp_ball_row && (int8_t)col == s_gp_ball_col) {
                /* Checked before either paddle so it draws on top during
                 * a bounce, when they briefly occupy the same cell --
                 * same precedent as brick breaker's ball. */
                b = GP_BALL_LEVEL;
            } else if (col == GP_PADDLE_COL_LEFT && (int8_t)row >= s_gp_left_paddle_top &&
                       (int8_t)row <= (int8_t)(s_gp_left_paddle_top + 1)) {
                r = GP_PADDLE_LEVEL;
                g = GP_PADDLE_LEVEL;
                b = GP_PADDLE_LEVEL;
            } else if (col == GP_PADDLE_COL_RIGHT && (int8_t)row >= s_gp_right_paddle_top &&
                       (int8_t)row <= (int8_t)(s_gp_right_paddle_top + 1)) {
                r = GP_PADDLE_LEVEL;
                g = GP_PADDLE_LEVEL;
                b = GP_PADDLE_LEVEL;
            }
            tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
        }
    }

    bool flashing = (now_ms - s_gp_point_flash_ms) < GP_POINT_FLASH_MS;
    bool flash_on = flashing && (((now_ms - s_gp_point_flash_ms) / GP_POINT_FLASH_TOGGLE_MS) % 2u) == 0u;
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (flash_on) {
            tiles_lighting_set_standby_underglow_rgb(i, 1.0f, 1.0f, 1.0f);
        } else {
            tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
        }
    }
}

/* ---- Simon Says ------------------------------------------------------------
 * Real feedback: "lets implement another mini game, simon says, so a
 * haptic and led patern appears on the pads and the user has to follow,
 * start with a simple one pad at a time but it gets exponentially longer
 * like the real simon says." Real Simon Says itself grows by exactly one
 * step per round (linear, not exponential) -- "the real simon says" is
 * the actual behavioral anchor here, so that's what this implements;
 * "exponentially" is read as colloquial ("gets longer and longer fast
 * enough to feel hard"), not literal doubling.
 *
 * Any of the 24 pads can appear in the pattern (repeats allowed across
 * different steps, same as real Simon Says -- a longer sequence isn't
 * bounded by pad count), each step ALSO gets its own color from a small
 * fixed palette (real feedback: "the sewqurnce has different colors
 * flashing"), assigned independently of which pad it lands on -- pure
 * visual variety, not a code for anything.
 *
 * "for this mini game touch pads are disabeled but push pads are used
 * for pattern receation" -- unlike every other game/menu in this
 * codebase (all touch-driven), GSIM_PRESS_DEPTH below reads real Hall
 * depth during the player's input phase, the same "an actual push, not
 * a light touch" threshold expression.c's own MIN_STRIKE_DEPTH_DELTA
 * uses for real note strikes. "the haptics play a big part... giving
 * you the vibraions paired with light to indicate the correct light" --
 * every playback step fires both together; "when player plays the
 * colors do come back when pressed" -- a correct press re-flashes that
 * exact pad's own pattern color as confirmation. */

#define GSIM_MAX_LENGTH 32u
#define GSIM_NUM_COLORS 6u
#define GSIM_ROUND_START_DELAY_MS 700u /* pause before playback, breathing room between rounds */
#define GSIM_PLAYBACK_STEP_MS 550u     /* total time budget per pattern step, on + gap */
#define GSIM_PLAYBACK_FLASH_MS 380u    /* how much of that step is actually lit */
#define GSIM_PLAYBACK_VELOCITY 110u    /* firm -- this IS the thing the player must remember */
#define GSIM_FEEDBACK_VELOCITY 90u
#define GSIM_FEEDBACK_FLASH_MS 220u /* correct-press confirmation flash length */
/* Mirrors expression.c's MIN_STRIKE_DEPTH_DELTA (300) -- "an actual
 * push," the same real-hardware-calibrated threshold real note strikes
 * use, not a guessed new number. */
#define GSIM_PRESS_DEPTH 300.0f

typedef struct {
    float r, g, b;
} gsim_color_t;

static const gsim_color_t GSIM_PALETTE[GSIM_NUM_COLORS] = {
    {1.0f, 0.0f, 0.0f},  /* red */
    {0.0f, 1.0f, 0.0f},  /* green */
    {0.1f, 0.3f, 1.0f},  /* blue */
    {1.0f, 0.85f, 0.0f}, /* yellow */
    {1.0f, 0.0f, 1.0f},  /* magenta */
    {0.0f, 1.0f, 1.0f},  /* cyan */
};

typedef enum {
    GSIM_PHASE_ROUND_START = 0,
    GSIM_PHASE_PLAYBACK,
    GSIM_PHASE_INPUT,
} gsim_phase_t;

static uint8_t s_gsim_pattern_pad[GSIM_MAX_LENGTH];   /* 1-24 */
static uint8_t s_gsim_pattern_color[GSIM_MAX_LENGTH]; /* index into GSIM_PALETTE */
static uint8_t s_gsim_length;                         /* steps active this round */
static gsim_phase_t s_gsim_phase;
static uint32_t s_gsim_phase_start_ms;
static uint8_t s_gsim_playback_step;      /* which step PLAYBACK is currently showing */
static uint8_t s_gsim_last_haptic_step;   /* which step's haptic already fired -- 0xFF = none yet this round */
static uint8_t s_gsim_input_index;        /* how many correct steps reproduced so far this round */
static bool s_gsim_prev_pressed[TILES_NUM_PADS];
static uint8_t s_gsim_feedback_pad;    /* 0 = no active confirmation flash */
static uint32_t s_gsim_feedback_start_ms;

static void gsim_new_game(uint32_t now_ms) {
    /* Real feedback: "is simon says generating unique patterns every
     * time? it should do that." Reseeds the shared rand()/srand() stream
     * (see standby.c's own tiles_standby_init() for the deeper fix --
     * this firmware's ONE seed used to be boot-time-based and could end
     * up nearly identical across boots) with fresh hardware entropy
     * right as each new game starts, on top of that fix -- extra
     * insurance specifically for this feature, directly matching the
     * question asked, regardless of anything else that happened to
     * consume rand() calls earlier in the session. */
    srand((unsigned int)get_rand_32());
    s_gsim_length = 1u;
    s_gsim_pattern_pad[0] = (uint8_t)(1u + (uint8_t)(rand() % TILES_NUM_PADS));
    s_gsim_pattern_color[0] = (uint8_t)(rand() % GSIM_NUM_COLORS);
    s_gsim_phase = GSIM_PHASE_ROUND_START;
    s_gsim_phase_start_ms = now_ms;
    s_gsim_feedback_pad = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_gsim_prev_pressed[i] = false;
    }
}

static void gsim_extend_pattern(void) {
    if (s_gsim_length >= GSIM_MAX_LENGTH) {
        return; /* practical ceiling reached -- keep replaying the max-length pattern rather than overflow */
    }
    s_gsim_pattern_pad[s_gsim_length] = (uint8_t)(1u + (uint8_t)(rand() % TILES_NUM_PADS));
    s_gsim_pattern_color[s_gsim_length] = (uint8_t)(rand() % GSIM_NUM_COLORS);
    s_gsim_length++;
}

static void gsim_begin_round_start(uint32_t now_ms) {
    s_gsim_phase = GSIM_PHASE_ROUND_START;
    s_gsim_phase_start_ms = now_ms;
}

static void gsim_begin_playback(uint32_t now_ms) {
    s_gsim_phase = GSIM_PHASE_PLAYBACK;
    s_gsim_phase_start_ms = now_ms;
    s_gsim_playback_step = 0u;
    s_gsim_last_haptic_step = 0xFFu;
}

static void gsim_begin_input(uint32_t now_ms) {
    s_gsim_phase = GSIM_PHASE_INPUT;
    s_gsim_phase_start_ms = now_ms;
    s_gsim_input_index = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        /* Seed to whatever's currently pressed rather than false -- a
         * pad already held down the instant input begins must not read
         * as a fresh press edge. */
        s_gsim_prev_pressed[i] = (float)tiles_hall_get_depth((uint8_t)(i + 1u)) > GSIM_PRESS_DEPTH;
    }
}

static void gsim_update(uint32_t now_ms) {
    switch (s_gsim_phase) {
    case GSIM_PHASE_ROUND_START:
        if (now_ms - s_gsim_phase_start_ms >= GSIM_ROUND_START_DELAY_MS) {
            gsim_begin_playback(now_ms);
        }
        break;
    case GSIM_PHASE_PLAYBACK: {
        uint32_t elapsed = now_ms - s_gsim_phase_start_ms;
        uint32_t step = elapsed / GSIM_PLAYBACK_STEP_MS;
        if (step >= s_gsim_length) {
            gsim_begin_input(now_ms);
            break;
        }
        s_gsim_playback_step = (uint8_t)step;
        if (s_gsim_playback_step != s_gsim_last_haptic_step) {
            /* Real feedback: "the haptics play a big part on this one
             * giving you the vibraions paired with light to inditcate
             * the correct light" -- fires once per step, exactly when
             * that step's flash window begins. */
            s_gsim_last_haptic_step = s_gsim_playback_step;
            tiles_haptics_trigger_kick(s_gsim_pattern_pad[s_gsim_playback_step], GSIM_PLAYBACK_VELOCITY);
        }
        break;
    }
    case GSIM_PHASE_INPUT:
        /* Advanced by gsim_handle_input() below, not here -- reading
         * Hall depth is an input concern, kept with the other games'
         * own gX_handle_input() functions for the same reason. */
        break;
    }
}

/* Real feedback: "touch pads are disabeled but push pads are used for
 * pattern receation" -- reads tiles_hall_get_depth() directly, never
 * tiles_touch_is_touched(), for the entire input phase. */
static void gsim_handle_input(uint32_t now_ms) {
    if (s_gsim_phase != GSIM_PHASE_INPUT) {
        return;
    }
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool pressed = (float)tiles_hall_get_depth(pad) > GSIM_PRESS_DEPTH;
        bool edge = pressed && !s_gsim_prev_pressed[pad - 1u];
        s_gsim_prev_pressed[pad - 1u] = pressed;
        if (!edge) {
            continue;
        }

        uint8_t expected_pad = s_gsim_pattern_pad[s_gsim_input_index];
        if (pad != expected_pad) {
            /* Wrong pad -- real feedback: "when game is lost it should
             * flash red" (Tetris's own precedent, same treatment here). */
            gm_start_round_end(now_ms, true);
            return;
        }

        /* Correct -- "when player plays the colors do come back when
         * pressed": re-flash this exact pad's own pattern color as
         * confirmation, plus a haptic echo. */
        s_gsim_feedback_pad = pad;
        s_gsim_feedback_start_ms = now_ms;
        tiles_haptics_trigger_kick(pad, GSIM_FEEDBACK_VELOCITY);

        s_gsim_input_index++;
        if (s_gsim_input_index >= s_gsim_length) {
            /* Full pattern reproduced correctly -- next round, staying
             * in Simon Says (NOT gm_start_round_end(), which always
             * returns to the menu; only a wrong press does that). */
            gsim_extend_pattern();
            gsim_begin_round_start(now_ms);
        }
        return; /* one input pad per scan is enough -- avoids double-counting a simultaneous multi-pad brush */
    }
}

static void render_simon(uint32_t now_ms) {
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), 0.0f, 0.0f, 0.0f);
        }
    }

    if (s_gsim_phase == GSIM_PHASE_PLAYBACK) {
        uint32_t elapsed = now_ms - s_gsim_phase_start_ms;
        uint32_t within_step = elapsed - (uint32_t)s_gsim_playback_step * GSIM_PLAYBACK_STEP_MS;
        if (within_step < GSIM_PLAYBACK_FLASH_MS) {
            const gsim_color_t *c = &GSIM_PALETTE[s_gsim_pattern_color[s_gsim_playback_step]];
            tiles_lighting_set_standby_pad_rgb(s_gsim_pattern_pad[s_gsim_playback_step], c->r, c->g, c->b);
        }
    } else if (s_gsim_phase == GSIM_PHASE_INPUT && s_gsim_feedback_pad != 0u &&
               (now_ms - s_gsim_feedback_start_ms) < GSIM_FEEDBACK_FLASH_MS) {
        /* Confirmation flash for the most recently correctly-pressed pad
         * -- uses whichever step it just confirmed, i.e. the step BEFORE
         * s_gsim_input_index (already advanced past it by the time this
         * renders). */
        uint8_t confirmed_step = (uint8_t)(s_gsim_input_index - 1u);
        const gsim_color_t *c = &GSIM_PALETTE[s_gsim_pattern_color[confirmed_step]];
        tiles_lighting_set_standby_pad_rgb(s_gsim_feedback_pad, c->r, c->g, c->b);
    }

    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_buttons_set_standby_led(board_button_for_col(col), 0.0f);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Menu + round-end + top-level state machine --------------------------- */

static void render_menu(uint32_t now_ms) {
    (void)now_ms;
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_buttons_set_standby_led(board_button_for_col(col), 0.0f);
    }
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            if (row == 1u && col == 1u) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, GS_HEAD_LEVEL, 0.0f); /* snake = green */
            } else if (row == 1u && col == 2u) {
                tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 0.4f, 0.0f); /* brick breaker = orange */
            } else if (row == 1u && col == 3u) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 1.0f, 1.0f); /* tetris = cyan */
            } else if (row == 1u && col == 4u) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 1.0f); /* pong = blue, matches its ball */
            } else if (row == 1u && col == 5u) {
                tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 1.0f, 1.0f); /* simon says = white, its own pattern is the multi-color one */
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            }
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void render_round_end(uint32_t now_ms) {
    uint32_t toggle = (now_ms - s_gm_round_end_ms) / GM_ROUND_END_TOGGLE_MS;
    bool on_phase = (toggle % 2u) == 0u;
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (s_gm_round_end_red_only) {
            /* Tetris: plain red blink, not an alternation -- see
             * gt_lock()'s comment. */
            if (on_phase) {
                tiles_lighting_set_standby_underglow_rgb(i, 1.0f, 0.0f, 0.0f);
            } else {
                tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
            }
        } else if (on_phase) {
            tiles_lighting_set_standby_underglow_rgb(i, 1.0f, 0.0f, 0.0f);
        } else {
            tiles_lighting_set_standby_underglow_rgb(i, 0.6f, 0.0f, 1.0f);
        }
    }
    /* Pad grid and buttons deliberately left as whatever the game last
     * drew -- frozen, not re-rendered, so the board still shows the
     * result (empty bricks, final snake shape) while underglow flashes. */
}

static void gm_enter_menu(void) {
    s_gm_state = GM_STATE_MENU;
    s_gm_prev_pad1_touched = false;
    s_gm_prev_pad2_touched = false;
    s_gm_prev_pad3_touched = false;
    s_gm_prev_pad4_touched = false;
    s_gm_last_frame_ms = 0u;
}

static void gm_start_snake(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_SNAKE;
    gs_start(now_ms);
}

static void gm_start_brick(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_BRICK;
    gb_start(now_ms);
}

static void gm_start_tetris(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_TETRIS;
    gt_start(now_ms);
}

static void gm_start_pong(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_PONG;
    gp_start(now_ms);
}

static void gm_start_simon(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_SIMON;
    gsim_new_game(now_ms);
}

static void gm_handle_menu_selection(void) {
    bool pad1 = tiles_touch_is_touched(1u);
    bool pad2 = tiles_touch_is_touched(2u);
    bool pad3 = tiles_touch_is_touched(3u);
    bool pad4 = tiles_touch_is_touched(4u);
    bool pad5 = tiles_touch_is_touched(5u);
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (pad1 && !s_gm_prev_pad1_touched) {
        gm_start_snake(now_ms);
    } else if (pad2 && !s_gm_prev_pad2_touched) {
        gm_start_brick(now_ms);
    } else if (pad3 && !s_gm_prev_pad3_touched) {
        gm_start_tetris(now_ms);
    } else if (pad4 && !s_gm_prev_pad4_touched) {
        gm_start_pong(now_ms);
    } else if (pad5 && !s_gm_prev_pad5_touched) {
        gm_start_simon(now_ms);
    }
    s_gm_prev_pad1_touched = pad1;
    s_gm_prev_pad2_touched = pad2;
    s_gm_prev_pad3_touched = pad3;
    s_gm_prev_pad4_touched = pad4;
    s_gm_prev_pad5_touched = pad5;
}

static bool gm_combo_held(void) {
    if (tiles_expression_control_owns_pad_grid() || tiles_op_mode_owns_pad_grid()) {
        /* services/expression_control.h's sub-menu (circle+square held)
         * or services/op_mode.h's mode-select menu/sequencer already owns
         * the pad grid -- SW4 (diamond) alone is op_mode.h's own click
         * trigger, and SW5 (square)/SW6 (circle) are two of THIS combo's
         * four buttons, so without this guard a player deep in an
         * already-open sub-menu/sequencer who also happens to be resting
         * on the other buttons could accidentally toggle game mode on
         * underneath it. Never true while a game is already active (see
         * expression_control.c's/op_mode.c's own tiles_game_mode_is_
         * active() guards, which keep all three features mutually
         * exclusive), so this only ever blocks a fresh entry, never the
         * OFF toggle. */
        return false;
    }
    return tiles_button_is_pressed(3u) && tiles_button_is_pressed(4u) && tiles_button_is_pressed(5u) &&
           tiles_button_is_pressed(6u);
}

static void gm_toggle(uint32_t now_ms) {
    if (s_gm_state == GM_STATE_OFF) {
        tiles_lighting_set_standby_active(true);
        tiles_buttons_set_standby_active(true);
        gm_enter_menu();
    } else {
        s_gm_state = GM_STATE_OFF;
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    (void)now_ms;
}

static void gm_check_toggle_gesture(uint32_t now_ms) {
    bool held = gm_combo_held();
    if (held && !s_gm_combo_was_held) {
        s_gm_hold_start_ms = now_ms;
        s_gm_combo_fired = false;
    }
    if (held && !s_gm_combo_fired && (now_ms - s_gm_hold_start_ms) >= GM_HOLD_MS) {
        s_gm_combo_fired = true;
        gm_toggle(now_ms);
    }
    s_gm_combo_was_held = held;
}

void tiles_game_mode_init(void) {
    s_gm_state = GM_STATE_OFF;
    s_gm_last_frame_ms = 0u;
    s_gm_combo_was_held = false;
    s_gm_combo_fired = false;
    s_gm_prev_pad1_touched = false;
    s_gm_prev_pad2_touched = false;
    s_gm_prev_pad3_touched = false;
    s_gm_prev_pad4_touched = false;
    s_gm_prev_pad5_touched = false;
}

void tiles_game_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    gm_check_toggle_gesture(now_ms);

    if (s_gm_state == GM_STATE_OFF) {
        return;
    }

    if (s_gm_state == GM_STATE_MENU) {
        gm_handle_menu_selection();
    } else if (s_gm_state == GM_STATE_PLAYING_SNAKE) {
        gs_handle_input();
        gs_update(now_ms);
    } else if (s_gm_state == GM_STATE_PLAYING_BRICK) {
        gb_handle_input();
        gb_update(now_ms);
    } else if (s_gm_state == GM_STATE_PLAYING_TETRIS) {
        gt_handle_input(now_ms);
        gt_update(now_ms);
    } else if (s_gm_state == GM_STATE_PLAYING_PONG) {
        if (s_gp_match_over) {
            /* Frozen -- ball/paddles stay exactly where the match ended,
             * winner's controls glow (render_pong_score_buttons()) --
             * then back to the menu, same as every other game's round
             * end. See the Pong file header for why this is handled
             * locally rather than through GM_STATE_ROUND_END. */
            if (now_ms - s_gp_match_over_ms >= GP_MATCH_END_DISPLAY_MS) {
                gm_enter_menu();
            }
        } else {
            gp_handle_input();
            gp_update(now_ms);
        }
    } else if (s_gm_state == GM_STATE_PLAYING_SIMON) {
        gsim_handle_input(now_ms);
        gsim_update(now_ms);
    } else if (s_gm_state == GM_STATE_ROUND_END) {
        if (now_ms - s_gm_round_end_ms >= GM_ROUND_END_FLASH_MS) {
            gm_enter_menu();
        }
    }

    if (now_ms - s_gm_last_frame_ms >= GM_FRAME_INTERVAL_MS) {
        if (s_gm_state == GM_STATE_MENU) {
            render_menu(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_SNAKE) {
            render_snake(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_BRICK) {
            render_brick(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_TETRIS) {
            render_tetris(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_PONG) {
            render_pong(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_SIMON) {
            render_simon(now_ms);
        } else if (s_gm_state == GM_STATE_ROUND_END) {
            render_round_end(now_ms);
        }
        s_gm_last_frame_ms = now_ms;
    }
}

bool tiles_game_mode_is_active(void) {
    return s_gm_state != GM_STATE_OFF;
}
