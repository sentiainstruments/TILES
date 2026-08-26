#include "expression_control.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression.h"
#include "game_mode.h"
#include "haptics.h"
#include "lighting.h"
#include "touch.h"

#include "pico/time.h"

#include <stdio.h>

/* Square's own LED levels -- same reasoning octave_control.c's
 * BUTTON_ID_MINUS/_PLUS overrides and the pre-correction circle version
 * of this feature used: SQUARE_LED_HELD_LEVEL matches default press
 * feedback exactly (still feels tactile regardless of what the press
 * turns out to be); SQUARE_LED_TOGGLE_ON_LEVEL is the resting glow once
 * pitch bend is on and square isn't currently held -- deliberately close
 * to 1.0, not dramatically dimmer, per real feedback "the toggle
 * brightness is less than the regular click brightness but not by a
 * lot." Unmeasured -- a first guess at how much dimmer reads as "toggled
 * on" rather than "still being pressed." */
#define SQUARE_LED_HELD_LEVEL 1.0f
#define SQUARE_LED_TOGGLE_ON_LEVEL 0.8f

/* How long circle+square must be held together before expression mute
 * toggles -- edge-latched (s_mute_fired below) so a single long hold
 * can't re-fire, mirroring services/standby.h's own
 * TILES_CIRCLE_SCREENSAVER_HOLD_MS/_DEEP_SLEEP_HOLD_MS pattern. Real
 * feedback: "hold shift and sentia together for 3 seconds." */
#define EXPRESSION_MUTE_HOLD_MS 3000u

/* Mute's LED pattern on square -- "a blinking light with a two blink
 * pattern and rest at medium brightness." Two brief on/off blinks, then
 * a longer rest at MUTE_REST_LEVEL, then repeat. Unmeasured -- a first
 * attempt at pacing that reads clearly as "blink blink... pause" rather
 * than a flutter. */
#define MUTE_BLINK_ON_MS 120u
#define MUTE_BLINK_GAP_MS 120u
#define MUTE_REST_MS 900u
#define MUTE_BLINK_LEVEL 1.0f
#define MUTE_REST_LEVEL 0.5f

/* Sentia Instruments' own brand magenta -- reused here (also seen in
 * services/boot_sequence.c's final pulse phase) specifically because
 * it's already established as distinctly "this brand," not a color any
 * other mode in this codebase uses -- real feedback: "make the selected
 * level of everything sentia magenta color to differenciate from
 * standard modes." */
#define SENTIA_MAGENTA_R 1.0f
#define SENTIA_MAGENTA_G 0.0f
#define SENTIA_MAGENTA_B 1.0f

#define EXPRESSION_SUBMENU_NUM_ROWS 4u
#define EXPRESSION_SUBMENU_MIN_COLUMN 1u
#define EXPRESSION_SUBMENU_MAX_COLUMN 6u
#define EXPRESSION_SUBMENU_DEFAULT_COLUMN 4u

typedef enum {
    SUBMENU_ROW_HAPTICS = 0,
    SUBMENU_ROW_PITCH_BEND = 1,
    SUBMENU_ROW_Y_AXIS = 2, /* reserved -- stored, not yet consumed anywhere */
    SUBMENU_ROW_AFTERTOUCH = 3,
} submenu_row_t;

/* One selected column (1-6) per row, all defaulting to column 4 -- see
 * the file header's "sweet spot" reasoning. Board row 1 (top, nearest
 * the buttons) is SUBMENU_ROW_HAPTICS, row 4 (bottom) is
 * SUBMENU_ROW_AFTERTOUCH, matching the physical top-to-bottom order real
 * feedback described them in. */
static uint8_t s_row_column[EXPRESSION_SUBMENU_NUM_ROWS];

static bool s_submenu_active;
static bool s_prev_pad_touched[TILES_NUM_PADS];

static bool s_circle_was_held;
static bool s_square_was_held;
static bool s_square_press_became_combo;
static bool s_prev_minus_pressed;
static bool s_prev_plus_pressed;

static bool s_combo_was_held;
static uint32_t s_combo_hold_start_ms;
static bool s_mute_fired;
static bool s_mute_active;

/* Column 1..6 -> a parameter value, anchored at three points: column 1,
 * column EXPRESSION_SUBMENU_DEFAULT_COLUMN (4, every row's "sweet
 * spot"), and column 6 -- linearly interpolated between consecutive
 * anchors. Shared by every row below rather than each duplicating its
 * own interpolation, since all three real parameters (haptics
 * intensity, pitch bend sensitivity, aftertouch sensitivity) want the
 * exact same shape: "default at column 4, extends weaker toward column
 * 1 and stronger toward column 6" -- only the anchor VALUES differ per
 * row, and which direction is "stronger" is just baked into which value
 * is passed as value_col1 vs. value_col6 (works whether the underlying
 * quantity increases or decreases with "more sensitive," e.g. aftertouch
 * and pitch bend sensitivity are both *smaller-is-stronger*). */
static float piecewise_column_value(uint8_t column, float value_col1, float value_col4, float value_col6) {
    if (column <= EXPRESSION_SUBMENU_DEFAULT_COLUMN) {
        float t = (float)(column - EXPRESSION_SUBMENU_MIN_COLUMN) /
                  (float)(EXPRESSION_SUBMENU_DEFAULT_COLUMN - EXPRESSION_SUBMENU_MIN_COLUMN);
        return value_col1 + (value_col4 - value_col1) * t;
    }
    float t = (float)(column - EXPRESSION_SUBMENU_DEFAULT_COLUMN) /
              (float)(EXPRESSION_SUBMENU_MAX_COLUMN - EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    return value_col4 + (value_col6 - value_col4) * t;
}

/* Row 1 -- haptics.c's intensity scalar. Column 4 lands at 0.72, close
 * to but deliberately below the old fixed default of 1.0 (the physical
 * duty ceiling -- set_motor_level() clamps at 1.0 regardless), so
 * columns 5-6 have real headroom left to be "extra strong" rather than
 * column 4 already sitting at the ceiling with nowhere to go. Column 1
 * (0.3) matches haptics.c's own HAPTIC_INTENSITY_MIN floor. Unmeasured
 * -- a first attempt at a new tuned default, not yet felt on real
 * hardware at this specific value. */
static void apply_row_haptics(uint8_t column) {
    float value = piecewise_column_value(column, 0.3f, 0.72f, 1.0f);
    tiles_haptics_set_intensity(value);
}

/* Row 2 -- expression.c's pitch bend sensitivity (max cosine deviation).
 * SMALLER is MORE sensitive (less real motion needed for full bend), so
 * column 1 gets the LARGEST value (0.30, least sensitive) and column 6
 * the SMALLEST (0.075, most sensitive); column 4 is exactly 0.15, the
 * feature's original fixed default. Unmeasured -- same caveat as
 * expression.c's own PITCH_BEND_MAX_COSINE_DEVIATION history: no
 * captured real-hardware data yet for how far this should actually
 * range. */
static void apply_row_pitch_bend(uint8_t column) {
    float value = piecewise_column_value(column, 0.30f, 0.15f, 0.075f);
    tiles_expression_set_pitch_bend_sensitivity(value);
}

/* Row 4 -- expression.c's aftertouch full-scale depth. SMALLER is MORE
 * sensitive (full-scale aftertouch reached at a shallower press), so
 * column 1 gets the LARGEST value (1300, least sensitive) and column 6
 * the SMALLEST (600, most sensitive); column 4 is exactly 900, the
 * real-calibration-derived default (see expression.c's own
 * DEPTH_TO_AFTERTOUCH_FULL_SCALE history) -- preserved exactly so a
 * fresh boot's default feel is unchanged. Columns 1/6 are unmeasured
 * extensions off that real data, not independently calibrated. */
static void apply_row_aftertouch(uint8_t column) {
    float value = piecewise_column_value(column, 1300.0f, 900.0f, 600.0f);
    tiles_expression_set_aftertouch_sensitivity((uint16_t)value);
}

static void apply_row(submenu_row_t row, uint8_t column) {
    s_row_column[row] = column;
    switch (row) {
    case SUBMENU_ROW_HAPTICS:
        apply_row_haptics(column);
        break;
    case SUBMENU_ROW_PITCH_BEND:
        apply_row_pitch_bend(column);
        break;
    case SUBMENU_ROW_Y_AXIS:
        /* Reserved -- stored above, nothing to apply yet. */
        break;
    case SUBMENU_ROW_AFTERTOUCH:
        apply_row_aftertouch(column);
        break;
    }
    printf("[expression_control] row %d column %u selected\n", (int)row, column);
}

void tiles_expression_control_init(void) {
    for (uint8_t i = 0; i < EXPRESSION_SUBMENU_NUM_ROWS; i++) {
        s_row_column[i] = EXPRESSION_SUBMENU_DEFAULT_COLUMN;
    }
    s_submenu_active = false;
    s_circle_was_held = false;
    s_square_was_held = false;
    s_square_press_became_combo = false;
    s_prev_minus_pressed = false;
    s_prev_plus_pressed = false;
    s_combo_was_held = false;
    s_mute_fired = false;
    s_mute_active = false;
    tiles_buttons_set_override_active(TILES_SQUARE_BUTTON_ID, true);
    /* Every row starts at its default column, so apply each once up
     * front -- keeps haptics.c/expression.c's own defaults as the single
     * source of truth for "what column 4 means" instead of this module
     * silently assuming they already agree. */
    apply_row(SUBMENU_ROW_HAPTICS, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    apply_row(SUBMENU_ROW_PITCH_BEND, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    apply_row(SUBMENU_ROW_Y_AXIS, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    apply_row(SUBMENU_ROW_AFTERTOUCH, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
}

bool tiles_expression_control_owns_pad_grid(void) {
    return s_submenu_active;
}

bool tiles_expression_control_shift_active(void) {
    return s_square_was_held && !s_circle_was_held;
}

static void enter_submenu(void) {
    s_submenu_active = true;
    tiles_lighting_set_standby_active(true);
    /* A finger already resting on a pad the instant the combo starts
     * shouldn't immediately read as a fresh tap -- only a genuine rising
     * edge captured *after* the sub-menu opens selects a column. */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
}

static void exit_submenu(void) {
    s_submenu_active = false;
    tiles_lighting_set_standby_active(false);
}

static void toggle_mute(void) {
    s_mute_active = !s_mute_active;
    printf("[expression_control] expression mute %s\n", s_mute_active ? "enabled" : "disabled");
    tiles_haptics_set_muted(s_mute_active);
    tiles_expression_set_muted(s_mute_active);
}

/* Reads capacitive touch directly (not services/expression.c's state
 * machine, which is suppressed for new strikes while the sub-menu owns
 * the grid) -- a rising edge on any pad within the 4x6 slider grid
 * selects that pad's column for its row. */
static void handle_submenu_taps(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            if (touched && !s_prev_pad_touched[pad - 1u]) {
                apply_row((submenu_row_t)(row - 1u), col);
            }
            s_prev_pad_touched[pad - 1u] = touched;
        }
    }
}

static void render_submenu(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t selected_col = s_row_column[row - 1u];
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            if (col == selected_col) {
                tiles_lighting_set_standby_pad_rgb(pad, SENTIA_MAGENTA_R, SENTIA_MAGENTA_G, SENTIA_MAGENTA_B);
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            }
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* Square's own "-"/"+" shift input -- only called while square is held
 * alone (see tiles_expression_control_scan()) and mute is inactive
 * (square's LED is busy showing the mute pattern instead, and per the
 * file header, "expression functions mute" suppresses this too). Same
 * shape services/standby.c's pre-correction circle version used. */
static void handle_square_shift_input(void) {
    bool minus = tiles_button_is_pressed(1u); /* SW1 "-" */
    bool plus = tiles_button_is_pressed(2u);  /* SW2 "+" */

    if (minus && !s_prev_minus_pressed) {
        tiles_haptics_adjust_intensity(-1);
    }
    if (plus && !s_prev_plus_pressed) {
        tiles_haptics_adjust_intensity(1);
    }

    s_prev_minus_pressed = minus;
    s_prev_plus_pressed = plus;
}

static float mute_blink_level(uint32_t now_ms) {
    uint32_t cycle_ms = 2u * (MUTE_BLINK_ON_MS + MUTE_BLINK_GAP_MS) + MUTE_REST_MS;
    uint32_t phase = now_ms % cycle_ms;
    uint32_t blink_unit = MUTE_BLINK_ON_MS + MUTE_BLINK_GAP_MS;

    if (phase < blink_unit) {
        return (phase < MUTE_BLINK_ON_MS) ? MUTE_BLINK_LEVEL : 0.0f;
    }
    phase -= blink_unit;
    if (phase < blink_unit) {
        return (phase < MUTE_BLINK_ON_MS) ? MUTE_BLINK_LEVEL : 0.0f;
    }
    return MUTE_REST_LEVEL;
}

static void render_square_led(bool square_held, bool combo_held, uint32_t now_ms) {
    float level;
    if (s_mute_active) {
        level = mute_blink_level(now_ms);
    } else if (combo_held || square_held) {
        level = SQUARE_LED_HELD_LEVEL;
    } else {
        level = tiles_expression_is_pitch_bend_enabled() ? SQUARE_LED_TOGGLE_ON_LEVEL : 0.0f;
    }
    tiles_buttons_set_override_led(TILES_SQUARE_BUTTON_ID, level);
}

void tiles_expression_control_scan(void) {
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    bool square_held = tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);

    if (tiles_game_mode_is_active()) {
        /* Pong claims SW5 (square)/SW6 (circle) as live paddle controls
         * -- see the file header's "Deferring to game mode" section.
         * Keep edge-tracking state current so a still-held button
         * doesn't read as a fresh press/combo the instant control hands
         * back, and don't touch the LED (game_mode.c already claims
         * buttons.c's standby-active flag, making override writes a
         * no-op during play anyway). */
        s_square_was_held = square_held;
        s_circle_was_held = circle_held;
        s_combo_was_held = circle_held && square_held;
        return;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool combo_held = circle_held && square_held;

    if (combo_held && !s_combo_was_held) {
        s_combo_hold_start_ms = now_ms;
        s_mute_fired = false;
        enter_submenu();
    }
    if (!combo_held && s_combo_was_held) {
        exit_submenu();
    }
    if (combo_held) {
        s_square_press_became_combo = true;
        if (!s_mute_fired && (now_ms - s_combo_hold_start_ms) >= EXPRESSION_MUTE_HOLD_MS) {
            s_mute_fired = true;
            toggle_mute();
        }
        handle_submenu_taps();
        render_submenu();
    }
    s_combo_was_held = combo_held;

    if (square_held && !s_square_was_held) {
        /* Fresh press from fully released -- this press cycle hasn't
         * become a combo yet. */
        s_square_press_became_combo = false;
    }

    if (square_held && !circle_held && !s_mute_active) {
        handle_square_shift_input();
    }

    if (!square_held && s_square_was_held && !s_square_press_became_combo && !s_mute_active) {
        /* Released without ever joining circle in a combo, and mute
         * isn't suppressing square's own click -- a genuine short
         * click. */
        tiles_expression_toggle_pitch_bend();
    }

    render_square_led(square_held, combo_held, now_ms);

    s_square_was_held = square_held;
    s_circle_was_held = circle_held;
}
