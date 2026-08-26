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
 * feedback: "hold shift and sentia together for 3 seconds." Independent
 * of EXPRESSION_SUBMENU_TOGGLE_HOLD_MS below -- same numeric value, but
 * a completely separate gesture (this needs circle too, that doesn't). */
#define EXPRESSION_MUTE_HOLD_MS 3000u

/* How long square must be held ALONE (circle NOT also held) before the
 * sub-menu's momentary preview LOCKS open (sticky) -- real feedback:
 * "momentary press should open menu as well but after 3 seconds the
 * toggle should happen." The sub-menu itself is already visible from the
 * instant square is held (see tiles_expression_control_scan()); this
 * threshold only governs whether it disappears again on release (below
 * 3s) or stays visible after release until toggled off the same way
 * (at/above 3s). Same edge-latch shape as EXPRESSION_MUTE_HOLD_MS above
 * (s_submenu_toggle_fired), but tracked against its own independent
 * hold-start timestamp since it's a different gesture on a different
 * button combination. */
#define EXPRESSION_SUBMENU_TOGGLE_HOLD_MS 3000u

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

/* Every unselected pad in the sub-menu sits at this fraction of full
 * magenta rather than fully dark -- real feedback, after a first
 * hardware pass showed unselected pads reading at the same brightness as
 * the selection: "make the non selected default light pad very dim."
 * Also doubles as row 1's "off" indicator's dim phase (see
 * OFF_INDICATOR_BLINK_PERIOD_MS below) so that blink reads as "dipping
 * to the same baseline every other pad sits at," not a jump to a
 * different, unrelated darkness. Unmeasured -- a first guess at "very
 * dim but still visibly lit," not calibrated against real LED
 * brightness/diffusion. */
#define SUBMENU_UNSELECTED_LEVEL 0.06f

/* The "this row's selected level is OFF" indicator -- real feedback,
 * after row 1's column 1 was tried on real hardware as merely "weak"
 * rather than truly off: "the lowest setting is off and should be
 * blinking when active in menu to show its off." Faster/plainer than
 * the mute pattern above (a single on/off toggle, not a blink-blink-rest
 * shape) since this marks one row's state within an already-visible
 * menu, not the whole board's mode the way mute's button-LED indicator
 * does. */
#define OFF_INDICATOR_BLINK_PERIOD_MS 500u

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
 * feedback described them in. This is the single source of truth for
 * "what's currently selected" -- both a pad tap (handle_submenu_taps())
 * and square's own "-"/"+" shift (handle_square_shift_input(), row 1
 * only) go through apply_row() below to change it, so the two paths can
 * never disagree about what's actually applied -- real feedback: "theres
 * no continuity between menu and arrow keys control for haptics... any
 * changes that affect those 4 parameters should always be reflected on
 * the menu." */
static uint8_t s_row_column[EXPRESSION_SUBMENU_NUM_ROWS];

/* s_submenu_sticky persists across a square release once locked (the 3s
 * hold); s_submenu_visible is this tick's actual "is it shown right now"
 * state (square held alone OR sticky) -- what
 * tiles_expression_control_owns_pad_grid() reports, and what
 * pad-grid-claim/release + touch-tap edge tracking key off of. Kept
 * separate because "square held alone" alone should show the sub-menu
 * (a momentary preview) without necessarily being sticky yet. */
static bool s_submenu_sticky;
static bool s_submenu_visible;
static bool s_prev_pad_touched[TILES_NUM_PADS];

static bool s_circle_was_held;
static bool s_square_was_held;
/* True once EITHER a long-hold action (the mute combo, or the sub-menu's
 * sticky-lock threshold) has fired at any point during the CURRENT
 * square press (reset only when square transitions from fully released
 * to held) -- suppresses that press's eventual release from also being
 * read as a genuine short click. */
static bool s_square_press_had_long_action;
/* Same idea, circle's own press cycle -- suppresses an incidental circle
 * release right after a circle+square mute combo from also being read
 * as the plain click that dismisses a sticky sub-menu (see
 * tiles_expression_control_scan()'s release checks). */
static bool s_circle_press_had_long_action;
static bool s_prev_minus_pressed;
static bool s_prev_plus_pressed;

/* SW1-SW4 edge tracking for dismissing a sticky, passively-visible
 * sub-menu -- see handle_submenu_dismiss_buttons(). Indices 0-3 map to
 * button ids 1-4. */
static bool s_prev_dismiss_btn[4];

static bool s_combo_was_held;
static uint32_t s_combo_hold_start_ms;
static bool s_mute_fired;
static bool s_mute_active;

static bool s_square_alone_was_held;
static uint32_t s_square_alone_hold_start_ms;
static bool s_submenu_toggle_fired;

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

/* Row 1 -- haptics.c's intensity scalar. Column 1 is a real, true OFF
 * (0.0 -- real feedback: "the lowest setting is off") rather than just a
 * weak setting, matching haptics.c's own HAPTIC_INTENSITY_MIN floor,
 * which is 0.0 for exactly this reason. Column 4 lands at 0.72, close to
 * but deliberately below the old fixed default of 1.0 (the physical
 * duty ceiling -- set_motor_level() clamps at 1.0 regardless), so
 * columns 5-6 have real headroom left to be "extra strong" rather than
 * column 4 already sitting at the ceiling with nowhere to go. Unmeasured
 * -- a first attempt at a new tuned default, not yet felt on real
 * hardware at this specific value. */
static void apply_row_haptics(uint8_t column) {
    float value = piecewise_column_value(column, 0.0f, 0.72f, 1.0f);
    tiles_haptics_set_intensity(value);
}

/* Row 2 -- expression.c's pitch bend sensitivity (max cosine deviation).
 * SMALLER is MORE sensitive (less real motion needed for full bend), so
 * column 1 gets the LARGEST value (0.60, least sensitive) and column 6
 * the SMALLEST (0.15, most sensitive); column 4 is exactly 0.30, the
 * feature's current default -- raised from an original 0.15 (with these
 * anchors rescaled to match, same 2x/0.5x-of-default spread as before)
 * after real feedback that it read as too sensitive and jittery on real
 * hardware; see expression.c's own s_pitch_bend_max_cosine_deviation
 * comment for the fuller fix (this scalar plus a smoothing/deadzone
 * change together). Column 6's 0.15 stays comfortably above
 * PITCH_BEND_DEADZONE_COSINE_DELTA (0.03) so even the most sensitive
 * column keeps a real usable range. Unmeasured -- same caveat as
 * expression.c's own pitch-bend-sensitivity history: no captured
 * real-hardware data yet for how far this should actually range. */
static void apply_row_pitch_bend(uint8_t column) {
    float value = piecewise_column_value(column, 0.60f, 0.30f, 0.15f);
    tiles_expression_set_pitch_bend_sensitivity(value);
}

/* Row 4 -- expression.c's aftertouch full-scale depth. SMALLER is MORE
 * sensitive (full-scale aftertouch reached at a shallower press), so
 * column 1 gets the LARGEST value (1300, least sensitive) and column 6
 * the SMALLEST (600, most sensitive); column 4 is exactly 900, the
 * real-calibration-derived default (see expression.c's own aftertouch
 * sensitivity history) -- preserved exactly so a fresh boot's default
 * feel is unchanged. Columns 1/6 are unmeasured extensions off that real
 * data, not independently calibrated. */
static void apply_row_aftertouch(uint8_t column) {
    float value = piecewise_column_value(column, 1300.0f, 900.0f, 600.0f);
    tiles_expression_set_aftertouch_sensitivity((uint16_t)value);
}

/* The single funnel every real edit goes through, whether from a pad tap
 * (handle_submenu_taps()) or square's own "-"/"+" shift
 * (step_haptics_column()) -- see s_row_column's own comment for why that
 * matters for menu/shift continuity. Also the one place that can tell a
 * genuine CHANGE apart from a no-op re-selection or a clamped step, which
 * is what lets an in-menu edit override expression mute without simply
 * opening/viewing the menu also doing so -- real feedback: "changes to
 * the menu should override expression mute and turn it off but if the
 * menu is opened just to check settings and no change is made then mute
 * stays on." */
static void apply_row(submenu_row_t row, uint8_t column) {
    bool changed = (s_row_column[row] != column);
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
    if (changed && s_mute_active) {
        printf("[expression_control] in-menu change while muted -- unmuting\n");
        s_mute_active = false;
        tiles_haptics_set_muted(false);
        tiles_expression_set_muted(false);
    }
}

/* Row 1 (haptics), column 1 only -- see apply_row_haptics()'s own
 * comment. No other row has a true "off" position at column 1 (pitch
 * bend/aftertouch sensitivity are just "least sensitive" there, still a
 * real, functioning setting), so this deliberately doesn't generalize to
 * every row. */
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
    s_mute_fired = false;
    s_mute_active = false;
    s_square_alone_was_held = false;
    s_submenu_toggle_fired = false;
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
    return s_submenu_visible;
}

/* Called on the rising edge of "the sub-menu should be shown" (square
 * freshly held alone) and the falling edge (square released while not
 * sticky, or a dismiss/sticky-off while square isn't held) -- claims/
 * releases the pad grid and, on entry only, seeds the touch-edge tracker
 * so a finger already resting on a pad doesn't immediately read as a
 * fresh tap. */
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

/* Tracks SW1-4 press edges unconditionally, every tick, regardless of
 * sub-menu state -- deliberately separate from ACTING on an edge (see
 * the call site) so s_prev_dismiss_btn[] never goes stale while the
 * sub-menu is hidden or square is actively held; a button already down
 * at the exact moment the passive-sticky window opens must never read
 * as a fresh press just because tracking hadn't been running. Returns
 * true if any of the 4 had a rising edge this tick. */
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

    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        submenu_row_t row_enum = (submenu_row_t)(row - 1u);
        uint8_t selected_col = s_row_column[row - 1u];
        bool off = row_column_is_off(row_enum, selected_col);
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool bright = (col == selected_col) && (!off || blink_on);
            float level = bright ? 1.0f : SUBMENU_UNSELECTED_LEVEL;
            tiles_lighting_set_standby_pad_rgb(pad, SENTIA_MAGENTA_R * level, SENTIA_MAGENTA_G * level,
                                                SENTIA_MAGENTA_B * level);
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* Square's own "-"/"+" shift input -- only called while square is held
 * alone (see tiles_expression_control_scan()). Works regardless of mute
 * (see the file header's "Available during expression mute" section) --
 * a genuine step through apply_row() will auto-unmute via that
 * function's own change check. Steps the sub-menu's row 1 (haptics)
 * COLUMN, through the exact same apply_row() path a pad tap uses,
 * rather than haptics.c's intensity scalar directly -- see
 * s_row_column's own comment for why. */
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
         * no-op during play anyway). Sub-menu visibility can't be true
         * here -- game_mode.c's own gm_combo_held() refuses to enter
         * while it is -- so it's deliberately left untouched rather than
         * force-cleared. */
        s_square_was_held = square_held;
        s_circle_was_held = circle_held;
        s_combo_was_held = circle_held && square_held;
        s_square_alone_was_held = square_held && !circle_held;
        return;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool combo_held = circle_held && square_held;
    bool square_alone_held = square_held && !circle_held;

    if (square_held && !s_square_was_held) {
        /* Fresh press from fully released -- this press cycle hasn't had
         * a long-hold action (mute combo, sub-menu sticky-lock) yet, so
         * a plain release should still be read as a genuine short click
         * unless one of the blocks below sets this true first. */
        s_square_press_had_long_action = false;
    }
    if (circle_held && !s_circle_was_held) {
        s_circle_press_had_long_action = false;
    }

    /* Mute: circle+square held EXPRESSION_MUTE_HOLD_MS -- independent of
     * the sub-menu below, see the file header. */
    if (combo_held && !s_combo_was_held) {
        s_combo_hold_start_ms = now_ms;
        s_mute_fired = false;
    }
    if (combo_held) {
        s_square_press_had_long_action = true;
        s_circle_press_had_long_action = true;
        if (!s_mute_fired && (now_ms - s_combo_hold_start_ms) >= EXPRESSION_MUTE_HOLD_MS) {
            s_mute_fired = true;
            toggle_mute();
        }
    }
    s_combo_was_held = combo_held;

    /* Sub-menu sticky-lock threshold + haptics shift: square held ALONE.
     * The alone streak (and its 3s timer) restarts any time circle joins
     * mid-hold -- see the file header. Both work regardless of mute (see
     * "Available during expression mute" there); a real edit auto-
     * unmutes via apply_row() itself. */
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
        /* Released without ever triggering a long-hold action -- a
         * genuine short click. While the sub-menu is sticky, that click
         * closes it instead of toggling pitch bend -- real feedback:
         * "make sure we can exit from menu with single click of
         * sentia." Works even while muted (unlike the plain pitch-bend
         * toggle below), since the sub-menu itself is available during
         * mute and should stay dismissable regardless. */
        if (s_submenu_sticky) {
            s_submenu_sticky = false;
        } else if (!s_mute_active) {
            tiles_expression_toggle_pitch_bend();
        }
    }

    if (!circle_held && s_circle_was_held && !s_circle_press_had_long_action && s_submenu_sticky) {
        /* Circle has no competing short-click action of its own to
         * protect, so a plain click always closes a sticky sub-menu,
         * mute or not -- real feedback: "make sure we can exit from
         * menu with single click of... shift/power as well." */
        s_submenu_sticky = false;
    }

    /* Always polled/tracked (see poll_dismiss_button_edge()'s own
     * comment on why), but only ACTED on -- dismissing a sticky,
     * passively-visible sub-menu -- while square isn't actively held
     * (SW1/SW2 are busy doing the haptics shift instead while it is, see
     * the file header). Real feedback: "any of the 4 function buttons
     * should exit that menu it shouldnt have to be untoggled." Clears
     * s_submenu_sticky before the visibility recompute just below, so
     * the dismiss takes effect this same tick. */
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
