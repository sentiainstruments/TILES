#include "op_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression_control.h"
#include "game_mode.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_clock.h"
#include "midi_out.h"
#include "note_map.h"
#include "octave_control.h"
#include "standby.h"
#include "touch.h"

#include <stdio.h>

typedef enum {
    OP_MODE_MELODIC = 0,
    OP_MODE_CHORD,
    OP_MODE_SEQUENCER,
    OP_MODE_ARP,
} tiles_op_mode_t;

#define OP_NUM_MODES 4u

/* Row assignment within the menu -- matches the order real feedback
 * listed the 4 modes in ("standard melodic, chord trigger mode...,
 * sequencer mode..., arpeggiator mode"). Row 1 is the pad row physically
 * closest to the function buttons (TILES_GRID_MIN_ROW + 1), same
 * top-to-bottom sense services/expression_control.h's sub-menu rows use. */
#define OP_ROW_MELODIC 1u
#define OP_ROW_CHORD 2u
#define OP_ROW_SEQUENCER 3u
#define OP_ROW_ARP 4u

/* Mode-selector row colors -- real feedback's own phrase, "mode selector
 * color": melodic = Sentia magenta (this codebase's brand color,
 * redefined locally here same as services/expression_control.c and
 * services/boot_sequence.c each already do rather than sharing one
 * definition -- established precedent, not an oversight), chord =
 * green, sequencer = red, arp = blue. */
#define OP_MENU_MELODIC_R 1.0f
#define OP_MENU_MELODIC_G 0.0f
#define OP_MENU_MELODIC_B 1.0f
#define OP_MENU_CHORD_R 0.0f
#define OP_MENU_CHORD_G 1.0f
#define OP_MENU_CHORD_B 0.0f
#define OP_MENU_SEQUENCER_R 1.0f
#define OP_MENU_SEQUENCER_G 0.0f
#define OP_MENU_SEQUENCER_B 0.0f
#define OP_MENU_ARP_R 0.0f
#define OP_MENU_ARP_G 0.0f
#define OP_MENU_ARP_B 1.0f

/* Diamond LED glow while something other than plain melodic play is
 * going on -- the button itself is monochrome PWM (not addressable RGB
 * like the pads), so it can't show the mode-selector colors above;
 * these are just "something's active, click to get back" brightness
 * cues, same reasoning services/expression_control.c's SQUARE_LED_*
 * levels use. */
#define OP_DIAMOND_LED_MENU_LEVEL 1.0f
#define OP_DIAMOND_LED_MODE_ACTIVE_LEVEL 0.6f

static tiles_op_mode_t s_active_mode;
static bool s_menu_visible;

static bool s_diamond_was_held;
/* True if SW3 was ALSO seen held at any point during the CURRENT diamond
 * press -- see this file's own header on why (game_mode.h's entry combo
 * is SW3+SW4+SW5+SW6; without this, imperfectly-simultaneous presses of
 * that combo could misread as a plain diamond click, the identical
 * collision real feedback already found once between this board's
 * circle+square gestures and that same combo). Checked on release, not
 * press, same "can't know it's a genuine click until it's over" reasoning
 * services/standby.c's own circle-short-tap wake fix uses. */
static bool s_diamond_press_had_conflict;

static bool s_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Sequencer state ------------------------------------------------- */

#define OP_SEQ_NUM_STEPS TILES_NUM_PADS /* 24 -- one step per pad, see this file's header */
/* 24 MIDI clocks per quarter note (fixed by the MIDI spec, not a guess)
 * / 4 sixteenths per quarter = 6 -- the standard "1 step = 1 sixteenth
 * note" sequencer convention. Unmeasured against what feels right on
 * this board specifically, same as every other timing constant in this
 * codebase -- a reasonable, well-established starting point rather than
 * an arbitrary guess. */
#define OP_SEQ_CLOCKS_PER_STEP 6u
/* No strike/touch event exists for a clock-triggered note to derive a
 * velocity from -- fixed at a solid, unaccented level rather than
 * guessing a curve with no input to drive it. */
#define OP_SEQ_VELOCITY 100u
/* The last of the 15 MPE Member Channels, reserved for sequencer
 * playback -- v1 only ever sounds one sequencer note at a time (a single
 * playhead, full-length gate, see this file's header), so one dedicated
 * channel is enough rather than routing through services/expression.c's
 * own per-touch allocator (which is private to that file, and built
 * around touch/strike lifecycle events sequencer playback doesn't have).
 * Narrow accepted edge case: a touch-driven note that happened to land on
 * this exact channel and is still sounding at the instant sequencer mode
 * starts could theoretically overlap with the first sequencer note --
 * sequencer mode already suppresses NEW touch strikes while it owns the
 * grid (see services/expression.c's own owns_pad_grid check), so this
 * only matters for a note that was already held before switching modes,
 * a brief, rare overlap rather than a structural conflict. */
#define OP_SEQ_CHANNEL ((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + TILES_MIDI_MPE_NUM_MEMBER_CHANNELS - 1u))

#define OP_SEQ_DIM_RED_LEVEL 0.3f

static bool s_seq_step_armed[OP_SEQ_NUM_STEPS];
static uint8_t s_seq_current_step; /* 0..23 */
static bool s_seq_note_sounding;
static uint8_t s_seq_sounding_pad; /* 1..24, valid iff s_seq_note_sounding */
static uint32_t s_seq_step_started_at_pulse;
static bool s_seq_prev_pad_touched[TILES_NUM_PADS];

static bool other_feature_owns_input(void) {
    return tiles_game_mode_is_active() || tiles_expression_control_owns_pad_grid() ||
           tiles_octave_control_is_transpose_active() || tiles_standby_is_active() || tiles_standby_is_deep_sleep();
}

static void seq_end_current_note(void) {
    if (!s_seq_note_sounding) {
        return;
    }
    tiles_midi_note_off(OP_SEQ_CHANNEL, tiles_note_map_get_note(s_seq_sounding_pad));
    tiles_haptics_stop(s_seq_sounding_pad);
    s_seq_note_sounding = false;
}

static void seq_enter_step(uint8_t step) {
    seq_end_current_note();
    s_seq_current_step = step;
    if (!s_seq_step_armed[step]) {
        return;
    }
    uint8_t pad = (uint8_t)(step + 1u);
    uint8_t note = tiles_note_map_get_note(pad);
    tiles_midi_note_on(OP_SEQ_CHANNEL, note, OP_SEQ_VELOCITY);
    tiles_haptics_trigger_kick(pad, OP_SEQ_VELOCITY);
    s_seq_note_sounding = true;
    s_seq_sounding_pad = pad;
}

static void seq_reset(uint32_t now_pulse) {
    s_seq_step_started_at_pulse = now_pulse;
    seq_enter_step(0u);
}

/* Called every time sequencer mode is (re-)entered from the menu --
 * deliberately does NOT clear s_seq_step_armed[]: leaving to melodic
 * mode to check something and coming back would otherwise wipe whatever
 * pattern was already programmed, which is more frustrating than useful.
 * Armed steps are only ever cleared at boot (tiles_op_mode_init()). */
static void seq_start(void) {
    s_seq_current_step = 0u;
    s_seq_note_sounding = false;
    s_seq_step_started_at_pulse = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_seq_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
}

static void seq_handle_step_taps(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        if (touched && !s_seq_prev_pad_touched[pad - 1u]) {
            s_seq_step_armed[pad - 1u] = !s_seq_step_armed[pad - 1u];
        }
        s_seq_prev_pad_touched[pad - 1u] = touched;
    }
}

static void seq_advance_clock(void) {
    tiles_midi_clock_state_t clock = tiles_midi_clock_get_state();

    if (clock.start_edge) {
        seq_reset(clock.pulse_count);
        return;
    }

    if (!clock.running) {
        seq_end_current_note();
        return;
    }

    uint32_t elapsed = clock.pulse_count - s_seq_step_started_at_pulse;
    if (elapsed < OP_SEQ_CLOCKS_PER_STEP) {
        return;
    }
    /* Handles more than one step's worth of pulses having accumulated
     * between scans (robust regardless of main-loop iteration rate vs
     * incoming clock rate), not just the common one-step case. */
    uint32_t steps_to_advance = elapsed / OP_SEQ_CLOCKS_PER_STEP;
    s_seq_step_started_at_pulse += steps_to_advance * OP_SEQ_CLOCKS_PER_STEP;
    uint8_t new_step = (uint8_t)((s_seq_current_step + steps_to_advance) % OP_SEQ_NUM_STEPS);
    seq_enter_step(new_step);
}

static void render_sequencer(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t step = (uint8_t)(pad - 1u);
            if (!s_seq_step_armed[step]) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            } else if (step == s_seq_current_step && s_seq_note_sounding) {
                tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 1.0f, 1.0f);
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_DIM_RED_LEVEL, 0.0f, 0.0f);
            }
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? OP_DIAMOND_LED_MODE_ACTIVE_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Menu -------------------------------------------------------------- */

static void menu_enter(void) {
    s_menu_visible = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void menu_exit(void) {
    s_menu_visible = false;
    if (s_active_mode != OP_MODE_SEQUENCER) {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
}

static void render_menu_row_color(uint8_t row, float *r, float *g, float *b) {
    switch (row) {
    case OP_ROW_MELODIC:
        *r = OP_MENU_MELODIC_R;
        *g = OP_MENU_MELODIC_G;
        *b = OP_MENU_MELODIC_B;
        break;
    case OP_ROW_CHORD:
        *r = OP_MENU_CHORD_R;
        *g = OP_MENU_CHORD_G;
        *b = OP_MENU_CHORD_B;
        break;
    case OP_ROW_SEQUENCER:
        *r = OP_MENU_SEQUENCER_R;
        *g = OP_MENU_SEQUENCER_G;
        *b = OP_MENU_SEQUENCER_B;
        break;
    default: /* OP_ROW_ARP */
        *r = OP_MENU_ARP_R;
        *g = OP_MENU_ARP_G;
        *b = OP_MENU_ARP_B;
        break;
    }
}

static void render_menu(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        float r, g, b;
        render_menu_row_color(row, &r, &g, &b);
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), r, g, b);
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? OP_DIAMOND_LED_MENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void set_active_mode(tiles_op_mode_t mode) {
    if (s_active_mode == OP_MODE_SEQUENCER && mode != OP_MODE_SEQUENCER) {
        seq_end_current_note();
    }
    s_active_mode = mode;
    if (mode == OP_MODE_SEQUENCER) {
        seq_start();
        tiles_lighting_set_standby_active(true);
        tiles_buttons_set_standby_active(true);
    } else {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, mode == OP_MODE_MELODIC ? 0.0f : OP_DIAMOND_LED_MODE_ACTIVE_LEVEL);
    printf("[op_mode] active mode -> %d\n", (int)mode);
}

static void handle_menu_taps(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            if (touched && !s_menu_prev_pad_touched[pad - 1u]) {
                tiles_op_mode_t mode = OP_MODE_MELODIC;
                if (row == OP_ROW_CHORD) {
                    mode = OP_MODE_CHORD;
                } else if (row == OP_ROW_SEQUENCER) {
                    mode = OP_MODE_SEQUENCER;
                } else if (row == OP_ROW_ARP) {
                    mode = OP_MODE_ARP;
                }
                menu_exit();
                set_active_mode(mode);
            }
            s_menu_prev_pad_touched[pad - 1u] = touched;
        }
    }
}

/* ---- Diamond click + top-level scan -------------------------------------- */

static void handle_diamond_click(void) {
    bool held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);

    if (held && !s_diamond_was_held) {
        s_diamond_press_had_conflict = false;
    }
    if (held && tiles_button_is_pressed(3u)) {
        /* See this file's header + s_diamond_press_had_conflict's own
         * comment -- part of game_mode.h's reserved 4-button combo, not
         * a genuine solo diamond press. */
        s_diamond_press_had_conflict = true;
    }

    if (!held && s_diamond_was_held) {
        if (!s_diamond_press_had_conflict) {
            if (s_menu_visible) {
                menu_exit();
            } else if (s_active_mode != OP_MODE_MELODIC) {
                set_active_mode(OP_MODE_MELODIC);
            } else {
                menu_enter();
            }
        }
    }

    s_diamond_was_held = held;
}

void tiles_op_mode_init(void) {
    s_active_mode = OP_MODE_MELODIC;
    s_menu_visible = false;
    s_diamond_was_held = false;
    s_diamond_press_had_conflict = false;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        s_seq_step_armed[i] = false;
    }
    s_seq_current_step = 0u;
    s_seq_note_sounding = false;
    s_seq_step_started_at_pulse = 0u;
    tiles_buttons_set_override_active(TILES_DIAMOND_BUTTON_ID, true);
    tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, 0.0f);
}

void tiles_op_mode_scan(void) {
    if (other_feature_owns_input()) {
        /* Keep edge-tracking current so a button/touch already active the
         * instant control hands back doesn't misread as a fresh
         * click/tap -- same pattern services/expression_control.c and
         * services/game_mode.c already use for their own equivalent
         * guards. */
        s_diamond_was_held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
        return;
    }

    handle_diamond_click();

    if (s_menu_visible) {
        handle_menu_taps();
        render_menu();
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER) {
        seq_handle_step_taps();
        seq_advance_clock();
        render_sequencer();
    }
}

bool tiles_op_mode_owns_pad_grid(void) {
    return s_menu_visible || s_active_mode == OP_MODE_SEQUENCER;
}
