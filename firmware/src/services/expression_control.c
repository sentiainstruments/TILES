#include "expression_control.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression.h"
#include "game_mode.h"
#include "haptics.h"
#include "lighting.h"
#include "op_mode.h"
#include "standby.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* Square LED: HELD matches normal press feedback; TOGGLE_ON is the resting
 * glow while pitch bend is on, a little dimmer than a press ("less ... but
 * not by a lot"). First guess. */
#define SQUARE_LED_HELD_LEVEL 1.0f
#define SQUARE_LED_TOGGLE_ON_LEVEL 0.8f

/* Circle+square hold that toggles MPE, edge-latched (s_mpe_toggle_fired)
 * so one long hold fires once. The same gesture once toggled an
 * "expression mute" (since removed). */
#define EXPRESSION_MPE_TOGGLE_HOLD_MS 2000u

/* Square held ALONE this long locks the menu open (sticky); the menu
 * itself shows from the first instant. Own timer and latch
 * (s_submenu_toggle_fired), separate from the MPE combo. */
#define EXPRESSION_SUBMENU_TOGGLE_HOLD_MS 2000u

/* Soft pulse on square while MPE is off (the non-default mode). Same
 * breathing shape as standby's deep-sleep pulse, kept as a separate copy.
 * First guess at pacing. */
#define MPE_DISABLED_PULSE_PERIOD_MS 3000.0f
#define MPE_DISABLED_PULSE_MIN 0.03f
#define MPE_DISABLED_PULSE_MAX 0.35f

/* Sentia brand magenta (as in the boot animation's final pulse). */
#define SENTIA_MAGENTA_R 1.0f
#define SENTIA_MAGENTA_G 0.0f
#define SENTIA_MAGENTA_B 1.0f

/* Unselected pads sit at this fraction of magenta, not dark: readable but
 * clearly not selected. 0.5 matches the scale picker's
 * OP_SCALE_AVAILABLE_LEVEL so both menus look the same. The "off"
 * indicator's dim phase uses the same level. Unmeasured. */
#define SUBMENU_UNSELECTED_LEVEL 0.5f

/* Menu standard: the selected/active item is always bright pulsing white
 * (same shape as op_mode.c's scale picker, redefined per file). */
#define SUBMENU_SELECTED_PULSE_PERIOD_MS 900.0f
#define SUBMENU_SELECTED_PULSE_MIN 0.5f
#define SUBMENU_SELECTED_PULSE_MAX 1.0f
#define EXPRESSION_CONTROL_PI 3.14159265358979323846f

static float submenu_selected_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / SUBMENU_SELECTED_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * EXPRESSION_CONTROL_PI * phase);
    return SUBMENU_SELECTED_PULSE_MIN + (SUBMENU_SELECTED_PULSE_MAX - SUBMENU_SELECTED_PULSE_MIN) * raw;
}

/* Row 1 column 1 (haptics OFF) blinks when selected, so the menu shows the
 * parameter is off, not just low. A plain on/off blink, faster than the
 * MPE-off pulse. */
#define OFF_INDICATOR_BLINK_PERIOD_MS 500u

#define EXPRESSION_SUBMENU_NUM_ROWS 4u
#define EXPRESSION_SUBMENU_MIN_COLUMN 1u
#define EXPRESSION_SUBMENU_MAX_COLUMN 6u
#define EXPRESSION_SUBMENU_DEFAULT_COLUMN 4u

typedef enum {
    SUBMENU_ROW_HAPTICS = 0,
    SUBMENU_ROW_PITCH_BEND = 1,
    SUBMENU_ROW_Y_AXIS = 2, /* reserved: stored, not used yet */
    SUBMENU_ROW_AFTERTOUCH = 3,
} submenu_row_t;

/* Selected column (1-6) per row, all default 4. Row 1 (top) = haptics,
 * row 4 (bottom) = aftertouch. The single source of truth: pad taps and
 * square's "-"/"+" both change it through apply_row(). */
static uint8_t s_row_column[EXPRESSION_SUBMENU_NUM_ROWS];

/* s_submenu_sticky survives a square release once locked; s_submenu_visible
 * is whether it shows this tick (square held alone OR sticky), which is
 * what owns_pad_grid() reports. */
static bool s_submenu_sticky;
static bool s_submenu_visible;
static bool s_prev_pad_touched[TILES_NUM_PADS];

static bool s_circle_was_held;
static bool s_square_was_held;
/* Set once a long-hold action (MPE combo or sticky lock) fired during the
 * CURRENT square press, so its release isn't also read as a click. Reset
 * on a fresh press. */
static bool s_square_press_had_long_action;
/* Same for circle: its release after the MPE combo isn't a click that
 * closes a sticky menu. */
static bool s_circle_press_had_long_action;
static bool s_prev_minus_pressed;
static bool s_prev_plus_pressed;

/* SW1-SW4 edge tracking for dismissing a sticky menu (index 0-3 = button
 * 1-4). */
static bool s_prev_dismiss_btn[4];

static bool s_combo_was_held;
static uint32_t s_combo_hold_start_ms;
static bool s_mpe_toggle_fired;

static bool s_square_alone_was_held;
static uint32_t s_square_alone_hold_start_ms;
static bool s_submenu_toggle_fired;

/* Column 1..6 -> value, linear between three anchors: column 1, column 4
 * (the default) and column 6. Every row uses this shape; only the anchor
 * values differ, and "stronger" can be larger or smaller (bend and
 * aftertouch sensitivity are smaller-is-stronger). */
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

/* Row 1: haptics intensity. Column 1 = true OFF (0.0). Column 4 = 0.72,
 * below the 1.0 duty ceiling so columns 5-6 have headroom. Not yet tuned
 * by feel. */
static void apply_row_haptics(uint8_t column) {
    float value = piecewise_column_value(column, 0.0f, 0.72f, 1.0f);
    tiles_haptics_set_intensity(value);
}

/* Row 2: pitch bend sensitivity (max cosine deviation; SMALLER is more
 * sensitive). Default 0.065 comes from captured data: a deliberate tilt
 * on this hardware rarely exceeds ~0.085 (see expression.c).
 *
 * Not the usual symmetric spread: with the deadzone at 0.04, half the
 * default (0.0325) would sit below it and do nothing. Column 1 (0.10) and
 * column 6 (0.055) come from the captured deliberate-tilt range (median
 * 0.0526, p90 0.0626). */
static void apply_row_pitch_bend(uint8_t column) {
    float value = piecewise_column_value(column, 0.10f, 0.065f, 0.055f);
    tiles_expression_set_pitch_bend_sensitivity(value);
}

/* Row 4: aftertouch full-scale depth (SMALLER is more sensitive). Column 4
 * = 900, the calibrated default; columns 1 (1300) and 6 (600) are
 * unmeasured extensions. */
static void apply_row_aftertouch(uint8_t column) {
    float value = piecewise_column_value(column, 1300.0f, 900.0f, 600.0f);
    tiles_expression_set_aftertouch_sensitivity((uint16_t)value);
}

/* The one path for every edit (pad tap or square's "-"/"+"). */
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
        /* Reserved: stored, nothing to apply. */
        break;
    case SUBMENU_ROW_AFTERTOUCH:
        apply_row_aftertouch(column);
        break;
    }
    printf("[expression_control] row %d column %u selected\n", (int)row, column);
}

/* Only row 1 has a real "off" (column 1); the others are just least
 * sensitive there. */
static bool row_column_is_off(submenu_row_t row, uint8_t column) {
    return row == SUBMENU_ROW_HAPTICS && column == EXPRESSION_SUBMENU_MIN_COLUMN;
}

static void step_haptics_column(int8_t direction) {
    int new_column = (int)s_row_column[SUBMENU_ROW_HAPTICS] + (direction > 0 ? 1 : -1);
    if (new_column < (int)EXPRESSION_SUBMENU_MIN_COLUMN) {
        new_column = EXPRESSION_SUBMENU_MIN_COLUMN;
    }
    if (new_column > (int)EXPRESSION_SUBMENU_MAX_COLUMN) {
        new_column = EXPRESSION_SUBMENU_MAX_COLUMN;
    }
    apply_row(SUBMENU_ROW_HAPTICS, (uint8_t)new_column);
}

void tiles_expression_control_init(void) {
    for (uint8_t i = 0; i < EXPRESSION_SUBMENU_NUM_ROWS; i++) {
        s_row_column[i] = EXPRESSION_SUBMENU_DEFAULT_COLUMN;
    }
    s_submenu_sticky = false;
    s_submenu_visible = false;
    s_circle_was_held = false;
    s_square_was_held = false;
    s_square_press_had_long_action = false;
    s_circle_press_had_long_action = false;
    s_prev_minus_pressed = false;
    s_prev_plus_pressed = false;
    for (uint8_t i = 0; i < 4u; i++) {
        s_prev_dismiss_btn[i] = false;
    }
    s_combo_was_held = false;
    s_mpe_toggle_fired = false;
    s_square_alone_was_held = false;
    s_submenu_toggle_fired = false;
    tiles_buttons_set_override_active(TILES_SQUARE_BUTTON_ID, true);
    /* Apply each row's default once, so haptics.c/expression.c agree with
     * column 4 from boot. */
    apply_row(SUBMENU_ROW_HAPTICS, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    apply_row(SUBMENU_ROW_PITCH_BEND, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    apply_row(SUBMENU_ROW_Y_AXIS, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
    apply_row(SUBMENU_ROW_AFTERTOUCH, EXPRESSION_SUBMENU_DEFAULT_COLUMN);
}

bool tiles_expression_control_owns_pad_grid(void) {
    return s_submenu_visible;
}

/* Shows/hides the menu: claims or releases the pad grid and, on entry,
 * seeds the touch tracker so a finger already resting on a pad isn't read
 * as a tap. */
static void set_submenu_visible(bool visible) {
    s_submenu_visible = visible;
    tiles_lighting_set_standby_active(visible);
    if (visible) {
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            s_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        }
    }
    printf("[expression_control] sub-menu %s\n", visible ? "visible" : "hidden");
}

/* MPE on/off (the circle+square combo). */
static void toggle_mpe_mode(void) {
    bool enabled = !tiles_expression_is_mpe_enabled();
    tiles_expression_set_mpe_enabled(enabled);
}

/* Reads touch directly (expression.c's strikes are suppressed meanwhile):
 * a new touch on a pad selects that column for its row. */
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

/* Tracks SW1-4 edges every tick, separate from acting on them, so a
 * button already down when the sticky menu appears isn't read as a new
 * press. True if any had a rising edge. */
static bool poll_dismiss_button_edge(void) {
    bool edge = false;
    for (uint8_t i = 0; i < 4u; i++) {
        bool pressed = tiles_button_is_pressed((uint8_t)(i + 1u));
        if (pressed && !s_prev_dismiss_btn[i]) {
            edge = true;
        }
        s_prev_dismiss_btn[i] = pressed;
    }
    return edge;
}

static void render_submenu(uint32_t now_ms) {
    bool blink_on = ((now_ms / OFF_INDICATOR_BLINK_PERIOD_MS) % 2u) == 0u;
    float pulse = submenu_selected_pulse_level(now_ms);

    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        submenu_row_t row_enum = (submenu_row_t)(row - 1u);
        uint8_t selected_col = s_row_column[row - 1u];
        bool off = row_column_is_off(row_enum, selected_col);
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool selected = (col == selected_col);
            if (selected && (!off || blink_on)) {
                /* Selected: bright pulsing white (menu standard). */
                tiles_lighting_set_standby_pad_rgb(pad, pulse, pulse, pulse);
            } else {
                /* Unselected pads and the off-blink's dim phase share the same dim
                 * magenta. */
                tiles_lighting_set_standby_pad_rgb(pad, SENTIA_MAGENTA_R * SUBMENU_UNSELECTED_LEVEL,
                                                    SENTIA_MAGENTA_G * SUBMENU_UNSELECTED_LEVEL,
                                                    SENTIA_MAGENTA_B * SUBMENU_UNSELECTED_LEVEL);
            }
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* Square held alone: "-"/"+" step the haptics row's column through
 * apply_row(). */
static void handle_square_shift_input(void) {
    bool minus = tiles_button_is_pressed(1u); /* SW1 "-" */
    bool plus = tiles_button_is_pressed(2u);  /* SW2 "+" */

    if (minus && !s_prev_minus_pressed) {
        step_haptics_column(-1);
    }
    if (plus && !s_prev_plus_pressed) {
        step_haptics_column(1);
    }

    s_prev_minus_pressed = minus;
    s_prev_plus_pressed = plus;
}

static float mpe_disabled_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / MPE_DISABLED_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * EXPRESSION_CONTROL_PI * phase);
    return MPE_DISABLED_PULSE_MIN + (MPE_DISABLED_PULSE_MAX - MPE_DISABLED_PULSE_MIN) * raw;
}

static void render_square_led(bool square_held, bool combo_held, uint32_t now_ms) {
    float level;
    if (!tiles_expression_is_mpe_enabled()) {
        /* MPE-off pulse wins over everything else on square's LED. */
        level = mpe_disabled_pulse_level(now_ms);
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

    /* While asleep, let standby handle square/circle: the menu opens the
     * instant square is held, which claims the grid, and main.c doesn't run
     * standby's wake logic while something owns the grid, so square couldn't
     * wake the board. Edge tracking stays current. */
    if (tiles_standby_is_active() || tiles_standby_is_deep_sleep()) {
        s_square_was_held = square_held;
        s_circle_was_held = circle_held;
        s_combo_was_held = circle_held && square_held;
        s_square_alone_was_held = square_held && !circle_held;
        return;
    }

    if (tiles_game_mode_is_active()) {
        /* A game uses square/circle as controls: keep edge tracking current and
         * leave the LED alone. The menu can't be visible here (game mode won't
         * enter while it is). */
        s_square_was_held = square_held;
        s_circle_was_held = circle_held;
        s_combo_was_held = circle_held && square_held;
        s_square_alone_was_held = square_held && !circle_held;
        return;
    }

    if (tiles_op_mode_owns_pad_grid()) {
        /* op_mode's menu or sequencer owns the grid: same as above. */
        s_square_was_held = square_held;
        s_circle_was_held = circle_held;
        s_combo_was_held = circle_held && square_held;
        s_square_alone_was_held = square_held && !circle_held;
        return;
    }

    /* Triangle + diamond also held = the game mode entry combo in progress.
     * Fingers never land or lift together, so moments where only circle and
     * square are down must not count as this module's gestures. */
    if (tiles_button_is_pressed(3u) && tiles_button_is_pressed(4u)) {
        s_square_was_held = square_held;
        s_circle_was_held = circle_held;
        s_combo_was_held = circle_held && square_held;
        s_square_alone_was_held = square_held && !circle_held;
        return;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    /* Each combo must match its EXACT button set: circle+square with diamond
     * and triangle both UP. Otherwise debug mode's diamond+square+circle hold
     * (8 s) also fired this 2 s combo on the way. */
    bool combo_held = circle_held && square_held && !tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) &&
                       !tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
    bool square_alone_held = square_held && !circle_held;

    if (square_held && !s_square_was_held) {
        /* Fresh press: no long action yet, so a release is a click unless one
         * fires below. */
        s_square_press_had_long_action = false;
    }
    if (circle_held && !s_circle_was_held) {
        s_circle_press_had_long_action = false;
    }

    /* MPE toggle: circle+square held EXPRESSION_MPE_TOGGLE_HOLD_MS. */
    if (combo_held && !s_combo_was_held) {
        s_combo_hold_start_ms = now_ms;
        s_mpe_toggle_fired = false;
    }
    if (combo_held) {
        s_square_press_had_long_action = true;
        s_circle_press_had_long_action = true;
        if (!s_mpe_toggle_fired && (now_ms - s_combo_hold_start_ms) >= EXPRESSION_MPE_TOGGLE_HOLD_MS) {
            s_mpe_toggle_fired = true;
            toggle_mpe_mode();
        }
    }
    s_combo_was_held = combo_held;

    /* Sticky lock and haptics shift: square held ALONE. The streak restarts if
     * circle joins. */
    if (square_alone_held && !s_square_alone_was_held) {
        s_square_alone_hold_start_ms = now_ms;
        s_submenu_toggle_fired = false;
    }
    if (square_alone_held) {
        if (!s_submenu_toggle_fired && (now_ms - s_square_alone_hold_start_ms) >= EXPRESSION_SUBMENU_TOGGLE_HOLD_MS) {
            s_submenu_toggle_fired = true;
            s_square_press_had_long_action = true;
            s_submenu_sticky = !s_submenu_sticky;
            printf("[expression_control] sub-menu sticky=%d\n", (int)s_submenu_sticky);
        }
        handle_square_shift_input();
    }
    s_square_alone_was_held = square_alone_held;

    if (!square_held && s_square_was_held && !s_square_press_had_long_action) {
        /* A real click: closes a sticky menu, otherwise toggles pitch bend. */
        if (s_submenu_sticky) {
            s_submenu_sticky = false;
        } else {
            tiles_expression_toggle_pitch_bend();
        }
    }

    if (!circle_held && s_circle_was_held && !s_circle_press_had_long_action && s_submenu_sticky) {
        /* A circle click always closes a sticky menu (nothing competes). */
        s_submenu_sticky = false;
    }

    /* SW1-4 close a sticky menu, but only while square isn't held (then
     * "-"/"+" adjust haptics instead). Cleared before the visibility update so
     * it applies this tick. */
    bool dismiss_edge = poll_dismiss_button_edge();
    if (s_submenu_sticky && !square_alone_held && dismiss_edge) {
        s_submenu_sticky = false;
    }

    bool submenu_should_be_visible = square_alone_held || s_submenu_sticky;
    if (submenu_should_be_visible != s_submenu_visible) {
        set_submenu_visible(submenu_should_be_visible);
    }
    if (s_submenu_visible) {
        handle_submenu_taps();
        render_submenu(now_ms);
    }

    render_square_led(square_held, combo_held, now_ms);

    s_square_was_held = square_held;
    s_circle_was_held = circle_held;
}
