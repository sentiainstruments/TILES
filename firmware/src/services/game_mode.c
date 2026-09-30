#include "game_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "cv_gate.h"
#include "expression.h"
#include "expression_control.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "op_mode.h"
#include "touch.h"

#include "pico/rand.h"
#include "pico/time.h"

#include <math.h>
#include <stdlib.h>

#define GAME_MODE_PI 3.14159265358979323846f

#define GM_HOLD_MS 700u /* 4-button hold time to toggle */
#define GM_FRAME_INTERVAL_MS 40u
#define GM_ROUND_END_FLASH_MS 2200u
#define GM_ROUND_END_TOGGLE_MS 260u

typedef enum {
    GM_STATE_OFF = 0,
    GM_STATE_MENU,
    GM_STATE_PLAYING_SNAKE,
    GM_STATE_PLAYING_TILE_BREAKER,
    GM_STATE_PLAYING_TETRIS,
    GM_STATE_PLAYING_PADDLE,
    GM_STATE_PLAYING_SIMON,
    GM_STATE_ROUND_END,
} gm_state_t;

static gm_state_t s_gm_state;
static uint32_t s_gm_last_frame_ms;
static uint32_t s_gm_round_end_ms;
static bool s_gm_round_end_red_only;

/* ---- Win/lose melodies + menu-select haptic ----------------------------
 * Pads play no notes and no haptics in game mode (expression.c ignores
 * the grid while a game is active). The only sounds are these short
 * melodies, and the only haptics are the menu-select kick and Simon Says'
 * pattern.
 *
 * Game mode's own fixed channel (services/midi_channels.h), outside the
 * MPE allocator's range. */
#define GM_MELODY_CHANNEL TILES_MIDI_CH_GAME
#define GM_MELODY_STEP_MS 130u
#define GM_MELODY_VELOCITY 100u
#define GM_MELODY_MAX_STEPS 4u
#define GM_MENU_SELECT_VELOCITY 90u

typedef struct {
    uint8_t notes[GM_MELODY_MAX_STEPS];
    uint8_t length;
} gm_melody_t;

/* Win: rising C major arpeggio (C4-E4-G4-C5). Loss: three falling notes
 * (C4-A3-F3). */
static const gm_melody_t GM_MELODY_WIN = {{60u, 64u, 67u, 72u}, 4u};
static const gm_melody_t GM_MELODY_LOSE = {{60u, 57u, 53u}, 3u};

static bool s_gm_melody_active;
static const gm_melody_t *s_gm_melody_current;
static uint32_t s_gm_melody_start_ms;
static uint8_t s_gm_melody_step;
static uint8_t s_gm_melody_sounding_note;

static void gm_melody_stop(void) {
    if (s_gm_melody_active) {
        tiles_midi_note_off(GM_MELODY_CHANNEL, s_gm_melody_sounding_note, 0u); /* scheduled melody note: release velocity 0 */
        tiles_cv_gate_note_off(s_gm_melody_sounding_note);
        s_gm_melody_active = false;
    }
}

static void gm_melody_start(const gm_melody_t *melody, uint32_t now_ms) {
    gm_melody_stop();
    s_gm_melody_current = melody;
    s_gm_melody_start_ms = now_ms;
    s_gm_melody_step = 0xFFu; /* sentinel: nothing sounded yet, so step 0 fires */
    s_gm_melody_active = true;
}

/* Advances the playing melody by elapsed time (non-blocking). Call every
 * scan while game mode owns the board; no-op when idle. */
static void gm_melody_update(uint32_t now_ms) {
    if (!s_gm_melody_active) {
        return;
    }
    uint8_t step = (uint8_t)((now_ms - s_gm_melody_start_ms) / GM_MELODY_STEP_MS);
    if (step >= s_gm_melody_current->length) {
        gm_melody_stop();
        return;
    }
    if (step != s_gm_melody_step) {
        if (s_gm_melody_step != 0xFFu) {
            tiles_midi_note_off(GM_MELODY_CHANNEL, s_gm_melody_sounding_note, 0u); /* scheduled melody note: release velocity 0 */
            tiles_cv_gate_note_off(s_gm_melody_sounding_note);
        }
        s_gm_melody_step = step;
        s_gm_melody_sounding_note = s_gm_melody_current->notes[step];
        tiles_midi_note_on(GM_MELODY_CHANNEL, s_gm_melody_sounding_note, GM_MELODY_VELOCITY);
        tiles_cv_gate_note_on(s_gm_melody_sounding_note, GM_MELODY_VELOCITY);
    }
}

static bool s_gm_combo_was_held;
static bool s_gm_combo_fired;
static uint32_t s_gm_hold_start_ms;

/* The 4-button hold forgives brief drops: once all four have been down
 * together, a release shorter than this doesn't restart the 700 ms hold
 * (four fingers wobble; resetting on every blip made it very hard to
 * trigger). It doesn't help the four land together in the first place. */
#define GM_COMBO_DROPOUT_GRACE_MS 250u
static uint32_t s_gm_combo_last_true_ms;
static bool s_gm_combo_last_true_valid;

static bool s_gm_prev_pad1_touched;
static bool s_gm_prev_pad2_touched;
static bool s_gm_prev_pad3_touched;
static bool s_gm_prev_pad4_touched;
static bool s_gm_prev_pad5_touched;

/* ---- Snake ---------------------------------------------------------------
 * Separate state from standby's self-playing snake (see game_mode.h). */

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
    /* Reseed from hardware entropy at every game start (see standby.c
     * tiles_standby_init() for the seeding story). */
    srand((unsigned int)get_rand_32());
    /* Start with length 2: the board is only 5x6. */
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

/* red_only: Tetris and Simon Says flash plain red; Snake and Tile Breaker
 * alternate red/purple. is_win is separate (Snake and Tile Breaker send
 * both outcomes through here with red_only=false) and picks the melody. */
static void gm_start_round_end(uint32_t now_ms, bool red_only, bool is_win) {
    s_gm_state = GM_STATE_ROUND_END;
    s_gm_round_end_ms = now_ms;
    s_gm_round_end_red_only = red_only;
    gm_melody_start(is_win ? &GM_MELODY_WIN : &GM_MELODY_LOSE, now_ms);
}

static void gs_try_set_direction(int8_t dr, int8_t dc) {
    /* Can't reverse straight into your own neck. */
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

    /* Wrap at the edges rather than die on a wall (the board is small). */
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
        gm_start_round_end(now_ms, false, false);
        return;
    }

    bool ate = (nr == s_gs_food.row && nc == s_gs_food.col);
    uint8_t new_length = ate ? (uint8_t)(s_gs_length + 1u) : s_gs_length;
    if (new_length > GS_MAX_LENGTH) {
        /* Board full: counts as a win. */
        gm_start_round_end(now_ms, false, true);
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

/* ---- Tile Breaker -------------------------------------------------------
 * Standby's tile breaker physics with a player-controlled paddle;
 * separate state. */

#define GB_NUM_COLS 6u
#define GB_PADDLE_ROW 4u
#define GB_STEP_MS 300u

static bool s_gb_block_alive[GB_NUM_COLS];
static int8_t s_gb_ball_row;
static int8_t s_gb_ball_col;
static int8_t s_gb_ball_drow;
static int8_t s_gb_ball_dcol;
static int8_t s_gb_paddle_center; /* 2-5 */
static uint32_t s_gb_last_step_ms;
static bool s_gb_prev_left;
static bool s_gb_prev_right;

static void gb_start(uint32_t now_ms) {
    /* Reseed per game (see gs_start()). */
    srand((unsigned int)get_rand_32());
    for (uint8_t i = 0; i < GB_NUM_COLS; i++) {
        s_gb_block_alive[i] = true;
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

/* Moving row and column by exactly 1 each step keeps (row + col) mod 2
 * fixed for the whole flight, so from the fixed start (3, 3) half the
 * blocks were unreachable. Fix (as in standby.c tb_step()): each bounce
 * off the top wall or paddle gets a coin-flip chance to reverse the
 * column direction, which breaks the parity lock. */
static void gb_step(uint32_t now_ms) {
    int8_t new_col = (int8_t)(s_gb_ball_col + s_gb_ball_dcol);
    if (new_col < (int8_t)TILES_GRID_MIN_COL || new_col > (int8_t)TILES_GRID_MAX_COL) {
        s_gb_ball_dcol = (int8_t)(-s_gb_ball_dcol);
        new_col = (int8_t)(s_gb_ball_col + s_gb_ball_dcol);
    }
    int8_t new_row = (int8_t)(s_gb_ball_row + s_gb_ball_drow);

    if (new_row < 1) {
        uint8_t col_index = (uint8_t)(new_col - TILES_GRID_MIN_COL);
        s_gb_block_alive[col_index] = false;
        s_gb_ball_drow = 1;
        new_row = 1;
        if ((rand() % 2) == 0) {
            s_gb_ball_dcol = (int8_t)(-s_gb_ball_dcol);
        }

        bool all_dead = true;
        for (uint8_t i = 0; i < GB_NUM_COLS; i++) {
            if (s_gb_block_alive[i]) {
                all_dead = false;
                break;
            }
        }
        if (all_dead) {
            gm_start_round_end(now_ms, false, true);
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
            gm_start_round_end(now_ms, false, false);
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

#define GB_BLOCK_LEVEL 0.85f
#define GB_PADDLE_LEVEL 0.9f
#define GB_BALL_LEVEL 1.0f

static void render_tile_breaker(uint32_t now_ms) {
    (void)now_ms;

    /* Blocks sit on the button row (monochrome PWM), so an alive block is a
     * single bright level, not a color. */
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        uint8_t idx = (uint8_t)(col - TILES_GRID_MIN_COL);
        float level = s_gb_block_alive[idx] ? GB_BLOCK_LEVEL : 0.0f;
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

/* ---- Tetris --------------------------------------------------------------
 * A 4x6 well (the pad grid; buttons off) with a custom small-piece set
 * (GT_PIECES), since full tetrominoes are too big for 4 rows: dot, domino,
 * straight tromino, corner tromino, 2x2 square. Pieces have 1-4 cells
 * (`num_cells`).
 *
 * 2 rotation states per piece and no wall kicks (a rotation that doesn't
 * fit is rejected). "-"/"+" move, triangle rotates, diamond hard-drops;
 * gravity every GT_STEP_MS. Full rows collapse (several at once is fine).
 * Topping out ends the round. */

#define GT_MIN_ROW 1u /* row 0 is buttons, not part of the well */
#define GT_MAX_ROW TILES_GRID_MAX_ROW
#define GT_MIN_COL TILES_GRID_MIN_COL
#define GT_MAX_COL TILES_GRID_MAX_COL
#define GT_ROWS 4u
#define GT_COLS 6u
#define GT_STEP_MS 550u
#define GT_SPAWN_COL 3 /* room either side for the widest piece (3) */
#define GT_NUM_PIECE_TYPES 5u
#define GT_MAX_CELLS 4u
/* White underglow strobe on a line clear: fast and short, a flash not a
 * glow. */
#define GT_LINE_CLEAR_FLASH_MS 450u
#define GT_LINE_CLEAR_TOGGLE_MS 90u

typedef struct {
    int8_t dr;
    int8_t dc;
} gt_offset_t;

/* Two rotation states; only the first num_cells entries are used. */
typedef struct {
    uint8_t num_cells;
    gt_offset_t state0[GT_MAX_CELLS];
    gt_offset_t state1[GT_MAX_CELLS];
    float r, g, b;
} gt_piece_def_t;

/* Smallest to largest. */
static const gt_piece_def_t GT_PIECES[GT_NUM_PIECE_TYPES] = {
    /* Dot: 1 cell, rotation is a no-op. */
    {1u, {{0, 0}}, {{0, 0}}, 1.0f, 1.0f, 1.0f},
    /* Domino: horizontal/vertical. */
    {2u, {{0, 0}, {0, 1}}, {{0, 0}, {1, 0}}, 0.0f, 1.0f, 1.0f},
    /* Straight tromino: horizontal/vertical. */
    {3u, {{0, 0}, {0, 1}, {0, 2}}, {{0, 0}, {1, 0}, {2, 0}}, 0.0f, 1.0f, 0.0f},
    /* Corner tromino: two different bends for variety (not a strict rotation). */
    {3u, {{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}}, 1.0f, 0.5f, 0.0f},
    /* Square: 2x2, rotation is a no-op. */
    {4u, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {0, 1}, {1, 0}, {1, 1}}, 1.0f, 1.0f, 0.0f},
};

/* 0 = empty, else piece type + 1; [row - GT_MIN_ROW][col - GT_MIN_COL]. */
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

/* Bottom-up: a full row shifts everything above down and the same row is
 * checked again, so multiple clears collapse in one pass. Returns rows
 * cleared (for the flash). */
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
        /* Line-clear strobe (see render_tetris()). */
        s_gt_line_clear_flash_ms = now_ms;
    }
    gt_spawn();
    if (!gt_fits(s_gt_piece_type, s_gt_rotation, s_gt_origin_row, s_gt_origin_col)) {
        /* Topped out: plain red. */
        gm_start_round_end(now_ms, true, false);
    }
}

static void gt_start(uint32_t now_ms) {
    /* Reseed per game (see gs_start()). */
    srand((unsigned int)get_rand_32());
    for (uint8_t r = 0; r < GT_ROWS; r++) {
        for (uint8_t c = 0; c < GT_COLS; c++) {
            s_gt_board[r][c] = 0u;
        }
    }
    gt_spawn();
    s_gt_last_step_ms = now_ms;
    /* "Long past", not 0, so a round starting right after boot doesn't flash. */
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

    /* The falling piece draws on top at full brightness, distinct from the
     * locked stack. */
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

/* ---- Paddle --------------------------------------------------------------
 * Two players on one board: left paddle column 1 ("-" up, "+" down), right
 * paddle column 6 (square up, circle down; the rightmost pair mirrors the
 * leftmost). Paddles are 2 pads, white; the ball is blue.
 *
 * A miss scores for the other side, flashes the underglow white and
 * re-serves at once (a point is too short to go back to the menu each
 * time). First to GP_WIN_SCORE (2) wins. Scores glow on each side's
 * control buttons: 1 point = the "up" button, 2 = both; triangle and
 * diamond stay dark. A win freezes the board for GP_MATCH_END_DISPLAY_MS,
 * then returns to the menu. Handled locally (s_gp_match_over), not via
 * GM_STATE_ROUND_END, since the "flash" is on the buttons. */

#define GP_MIN_ROW 1u /* row 0 is buttons, not part of the court */
#define GP_MAX_ROW TILES_GRID_MAX_ROW
#define GP_PADDLE_COL_LEFT TILES_GRID_MIN_COL
#define GP_PADDLE_COL_RIGHT TILES_GRID_MAX_COL
#define GP_PADDLE_TOP_MIN GP_MIN_ROW           /* paddle covers [top, top+1] */
#define GP_PADDLE_TOP_MAX (TILES_GRID_MAX_ROW - 1u)
#define GP_STEP_MS 260u
#define GP_POINT_FLASH_MS 500u
#define GP_POINT_FLASH_TOGGLE_MS 110u
#define GP_PADDLE_LEVEL 1.0f
#define GP_BALL_LEVEL 1.0f
#define GP_WIN_SCORE 2u
#define GP_MATCH_END_DISPLAY_MS 2500u
/* Breathing glow for a lit score button. */
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
    s_gp_ball_col = ((rand() % 2) == 0) ? 3 : 4; /* one of the two middle columns */
    s_gp_ball_drow = ((rand() % 2) == 0) ? -1 : 1;
    s_gp_ball_dcol = ((rand() % 2) == 0) ? -1 : 1;
    s_gp_last_step_ms = now_ms;
}

static void gp_start(uint32_t now_ms) {
    /* Reseed per game (see gs_start()). */
    srand((unsigned int)get_rand_32());
    s_gp_left_paddle_top = 2;
    s_gp_right_paddle_top = 2;
    s_gp_left_score = 0u;
    s_gp_right_score = 0u;
    s_gp_match_over = false;
    gp_serve(now_ms);
    /* "Long past", as for Tetris's flash. */
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

/* left_missed: the ball passed the left paddle (right scores), else left
 * scores. At GP_WIN_SCORE the match freezes instead of re-serving. */
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
        /* Paddle skips gm_start_round_end(), so play the melody here. Always WIN:
         * two local players, no single loser. */
        gm_melody_start(&GM_MELODY_WIN, now_ms);
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

/* Score on each side's control buttons (see the Paddle section). */
static void render_paddle_score_buttons(uint32_t now_ms) {
    float raw = 0.5f + 0.5f * sinf(2.0f * GAME_MODE_PI * (float)now_ms / GP_SCORE_GLOW_PERIOD_MS);
    float glow = GP_SCORE_GLOW_MIN + (GP_SCORE_GLOW_MAX - GP_SCORE_GLOW_MIN) * raw;

    tiles_buttons_set_standby_led(1u, (s_gp_left_score >= 1u) ? glow : 0.0f);  /* SW1 "-" */
    tiles_buttons_set_standby_led(2u, (s_gp_left_score >= 2u) ? glow : 0.0f);  /* SW2 "+" */
    tiles_buttons_set_standby_led(3u, 0.0f);
    tiles_buttons_set_standby_led(4u, 0.0f);
    tiles_buttons_set_standby_led(5u, (s_gp_right_score >= 1u) ? glow : 0.0f); /* SW5 square */
    tiles_buttons_set_standby_led(6u, (s_gp_right_score >= 2u) ? glow : 0.0f); /* SW6 circle */
}

static void render_paddle(uint32_t now_ms) {
    render_paddle_score_buttons(now_ms);

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            float r = 0.0f;
            float g = 0.0f;
            float b = 0.0f;

            if ((int8_t)row == s_gp_ball_row && (int8_t)col == s_gp_ball_col) {
                /* Ball before paddles, so it draws on top when they share a cell. */
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
 * A pattern of pads flashes, each with a haptic kick and its own color
 * from a small palette; the player repeats it by PRESSING the pads (Hall
 * depth, GSIM_PRESS_DEPTH; touch is ignored). Like the real game it grows
 * by one step each round. A correct press re-flashes that pad's color with
 * a haptic; a wrong pad ends the round (red flash, back to the menu).
 * Pads may repeat across steps. */

#define GSIM_MAX_LENGTH 32u
#define GSIM_NUM_COLORS 6u
#define GSIM_ROUND_START_DELAY_MS 700u /* pause before playback */
#define GSIM_PLAYBACK_STEP_MS 550u     /* per pattern step, lit + gap */
#define GSIM_PLAYBACK_FLASH_MS 380u    /* lit part of a step */
#define GSIM_PLAYBACK_VELOCITY 110u    /* firm: this is what the player must remember */
#define GSIM_FEEDBACK_VELOCITY 90u
#define GSIM_FEEDBACK_FLASH_MS 220u /* correct-press flash length */
/* Same as expression.c's MIN_STRIKE_DEPTH_DELTA (300): a real push. */
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
static uint8_t s_gsim_length;                         /* steps this round */
static gsim_phase_t s_gsim_phase;
static uint32_t s_gsim_phase_start_ms;
static uint8_t s_gsim_playback_step;      /* step PLAYBACK is showing */
static uint8_t s_gsim_last_haptic_step;   /* step whose haptic already fired; 0xFF = none this round */
static uint8_t s_gsim_input_index;        /* correct steps reproduced so far this round */
static bool s_gsim_prev_pressed[TILES_NUM_PADS];
static uint8_t s_gsim_feedback_pad;    /* 0 = no confirmation flash */
static uint32_t s_gsim_feedback_start_ms;

static void gsim_new_game(uint32_t now_ms) {
    /* Reseed per game so every game gets a new pattern (see standby.c's
     * seeding fix). */
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
        return; /* ceiling reached: keep replaying the longest pattern */
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
        /* Seed with what's already pressed so a held pad isn't a new press. */
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
            /* One haptic per step, as its flash starts. */
            s_gsim_last_haptic_step = s_gsim_playback_step;
            tiles_haptics_trigger_kick(s_gsim_pattern_pad[s_gsim_playback_step], GSIM_PLAYBACK_VELOCITY);
        }
        break;
    }
    case GSIM_PHASE_INPUT:
        /* Advanced by gsim_handle_input(), like the other games' input handlers. */
        break;
    }
}

/* Reads Hall depth only (no touch) for the whole input phase. */
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
            /* Wrong pad: plain red flash, back to the menu. */
            gm_start_round_end(now_ms, true, false);
            return;
        }

        /* Correct: re-flash this pad's pattern color, plus a haptic. */
        s_gsim_feedback_pad = pad;
        s_gsim_feedback_start_ms = now_ms;
        tiles_haptics_trigger_kick(pad, GSIM_FEEDBACK_VELOCITY);

        s_gsim_input_index++;
        if (s_gsim_input_index >= s_gsim_length) {
            /* Whole pattern right: next round (a round end would leave the game). */
            gsim_extend_pattern();
            gsim_begin_round_start(now_ms);
        }
        return; /* one pad per scan, so a brush across two isn't double-counted */
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
        /* Confirmation flash: the step just confirmed is input_index - 1. */
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
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, GS_HEAD_LEVEL, 0.0f); /* Snake = green */
            } else if (row == 1u && col == 2u) {
                tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 0.4f, 0.0f); /* Tile Breaker = orange */
            } else if (row == 1u && col == 3u) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 1.0f, 1.0f); /* Tetris = cyan */
            } else if (row == 1u && col == 4u) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 1.0f); /* Paddle = blue, like its ball */
            } else if (row == 1u && col == 5u) {
                tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 1.0f, 1.0f); /* Simon Says = white (its pattern is the colorful part) */
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
            /* Tetris/Simon: plain red blink. */
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
    /* Pads and buttons stay frozen as the game left them while the underglow
     * flashes. */
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

static void gm_start_tile_breaker(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_TILE_BREAKER;
    gb_start(now_ms);
}

static void gm_start_tetris(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_TETRIS;
    gt_start(now_ms);
}

static void gm_start_paddle(uint32_t now_ms) {
    s_gm_state = GM_STATE_PLAYING_PADDLE;
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

    /* The one haptic outside Simon Says: a felt confirmation a game started. */
    if (pad1 && !s_gm_prev_pad1_touched) {
        tiles_haptics_trigger_kick(1u, GM_MENU_SELECT_VELOCITY);
        gm_start_snake(now_ms);
    } else if (pad2 && !s_gm_prev_pad2_touched) {
        tiles_haptics_trigger_kick(2u, GM_MENU_SELECT_VELOCITY);
        gm_start_tile_breaker(now_ms);
    } else if (pad3 && !s_gm_prev_pad3_touched) {
        tiles_haptics_trigger_kick(3u, GM_MENU_SELECT_VELOCITY);
        gm_start_tetris(now_ms);
    } else if (pad4 && !s_gm_prev_pad4_touched) {
        tiles_haptics_trigger_kick(4u, GM_MENU_SELECT_VELOCITY);
        gm_start_paddle(now_ms);
    } else if (pad5 && !s_gm_prev_pad5_touched) {
        tiles_haptics_trigger_kick(5u, GM_MENU_SELECT_VELOCITY);
        gm_start_simon(now_ms);
    }
    s_gm_prev_pad1_touched = pad1;
    s_gm_prev_pad2_touched = pad2;
    s_gm_prev_pad3_touched = pad3;
    s_gm_prev_pad4_touched = pad4;
    s_gm_prev_pad5_touched = pad5;
}

static bool gm_combo_held(void) {
    if (tiles_expression_control_owns_pad_grid() || tiles_op_mode_has_menu_open()) {
        /* Don't enter while the expression menu or an op_mode sub-view (mode menu,
         * scale menu, pattern bank, step edit, capture) owns the grid: square and
         * circle are part of this combo and a player might be resting on them.
         * Uses tiles_op_mode_has_menu_open(), not owns_pad_grid(), which is true
         * for the whole time the sequencer is showing and made game mode
         * unreachable from it. Only blocks entry, never the off toggle (a game
         * excludes those features anyway). */
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
        /* A pad already mid-strike when the combo fires (incidental contact, both
         * hands are on the buttons) is released, so no note or haptic runs on
         * unsupervised during play. */
        tiles_expression_force_release_all();
    } else {
        s_gm_state = GM_STATE_OFF;
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    (void)now_ms;
}

static void gm_check_toggle_gesture(uint32_t now_ms) {
    bool raw_held = gm_combo_held();
    if (raw_held) {
        s_gm_combo_last_true_ms = now_ms;
        s_gm_combo_last_true_valid = true;
    }
    /* Bridge brief drops (GM_COMBO_DROPOUT_GRACE_MS). */
    bool held =
        raw_held || (s_gm_combo_last_true_valid && (now_ms - s_gm_combo_last_true_ms) < GM_COMBO_DROPOUT_GRACE_MS);

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

/* No single-button exit: in a game or the game menu, the only way out is
 * the same 4-button hold, which turns game mode off and returns to the
 * previous mode. Losing a round returns to the game menu. A shape button
 * that isn't a control in the current game does nothing. */

void tiles_game_mode_init(void) {
    s_gm_state = GM_STATE_OFF;
    s_gm_last_frame_ms = 0u;
    s_gm_combo_was_held = false;
    s_gm_combo_fired = false;
    s_gm_combo_last_true_valid = false;
    s_gm_prev_pad1_touched = false;
    s_gm_prev_pad2_touched = false;
    s_gm_prev_pad3_touched = false;
    s_gm_prev_pad4_touched = false;
    s_gm_prev_pad5_touched = false;
    s_gm_melody_active = false;
}

void tiles_game_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    gm_check_toggle_gesture(now_ms);

    if (s_gm_state == GM_STATE_OFF) {
        gm_melody_stop();
        return;
    }
    gm_melody_update(now_ms);

    if (s_gm_state == GM_STATE_MENU) {
        gm_handle_menu_selection();
    } else if (s_gm_state == GM_STATE_PLAYING_SNAKE) {
        gs_handle_input();
        gs_update(now_ms);
    } else if (s_gm_state == GM_STATE_PLAYING_TILE_BREAKER) {
        gb_handle_input();
        gb_update(now_ms);
    } else if (s_gm_state == GM_STATE_PLAYING_TETRIS) {
        gt_handle_input(now_ms);
        gt_update(now_ms);
    } else if (s_gm_state == GM_STATE_PLAYING_PADDLE) {
        if (s_gp_match_over) {
            /* Frozen board, winner's buttons glowing, then back to the menu (see the
             * Paddle section). */
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
        } else if (s_gm_state == GM_STATE_PLAYING_TILE_BREAKER) {
            render_tile_breaker(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_TETRIS) {
            render_tetris(now_ms);
        } else if (s_gm_state == GM_STATE_PLAYING_PADDLE) {
            render_paddle(now_ms);
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
