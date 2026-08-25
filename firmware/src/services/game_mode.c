#include "game_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "lighting.h"
#include "touch.h"

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
    GM_STATE_ROUND_END,
} gm_state_t;

static gm_state_t s_gm_state;
static uint32_t s_gm_last_frame_ms;
static uint32_t s_gm_round_end_ms;

static bool s_gm_combo_was_held;
static bool s_gm_combo_fired;
static uint32_t s_gm_hold_start_ms;

static bool s_gm_prev_pad1_touched;
static bool s_gm_prev_pad2_touched;

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
    s_gs_length = 3u;
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

static void gm_start_round_end(uint32_t now_ms) {
    s_gm_state = GM_STATE_ROUND_END;
    s_gm_round_end_ms = now_ms;
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
        gm_start_round_end(now_ms);
        return;
    }

    bool ate = (nr == s_gs_food.row && nc == s_gs_food.col);
    uint8_t new_length = ate ? (uint8_t)(s_gs_length + 1u) : s_gs_length;
    if (new_length > GS_MAX_LENGTH) {
        /* Board effectively full -- treat it as a win, same flash. */
        gm_start_round_end(now_ms);
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

        bool all_dead = true;
        for (uint8_t i = 0; i < GB_NUM_COLS; i++) {
            if (s_gb_brick_alive[i]) {
                all_dead = false;
                break;
            }
        }
        if (all_dead) {
            gm_start_round_end(now_ms);
        }
    } else if (new_row > (int8_t)GB_PADDLE_ROW) {
        int8_t paddle_min = (int8_t)(s_gb_paddle_center - 1);
        int8_t paddle_max = (int8_t)(s_gb_paddle_center + 1);
        if (new_col >= paddle_min && new_col <= paddle_max) {
            s_gb_ball_drow = -1;
            new_row = (int8_t)GB_PADDLE_ROW;
        } else {
            gm_start_round_end(now_ms);
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
    bool red_phase = (toggle % 2u) == 0u;
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (red_phase) {
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

static void gm_handle_menu_selection(void) {
    bool pad1 = tiles_touch_is_touched(1u);
    bool pad2 = tiles_touch_is_touched(2u);
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (pad1 && !s_gm_prev_pad1_touched) {
        gm_start_snake(now_ms);
    } else if (pad2 && !s_gm_prev_pad2_touched) {
        gm_start_brick(now_ms);
    }
    s_gm_prev_pad1_touched = pad1;
    s_gm_prev_pad2_touched = pad2;
}

static bool gm_combo_held(void) {
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
        } else if (s_gm_state == GM_STATE_ROUND_END) {
            render_round_end(now_ms);
        }
        s_gm_last_frame_ms = now_ms;
    }
}

bool tiles_game_mode_is_active(void) {
    return s_gm_state != GM_STATE_OFF;
}
