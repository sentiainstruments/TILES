#include "op_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression_control.h"
#include "game_mode.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_clock.h"
#include "midi_out.h"
#include "note_map.h"
#include "octave_control.h"
#include "standby.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
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
/* Same idea, triangle's own LED, lit while its per-mode sub-menu (e.g.
 * melodic's scale picker) is showing. */
#define OP_TRIANGLE_LED_SUBMENU_LEVEL 1.0f

/* Real feedback: "lets standardize the pulsing and brightness and
 * behaviour for menues, meaning select color or active color is always
 * white bright pulsing in menues, respective less bright but still
 * readable bright for non selected and available and of for
 * unaveilabe." The shared shape (not a shared function -- redefined per
 * file, same precedent as SENTIA_MAGENTA_R/G/B/etc.) with
 * services/expression_control.c's own sub-menu, the other menu this
 * standard applies to. */
#define OP_MENU_SELECTED_PULSE_PERIOD_MS 900.0f
#define OP_MENU_SELECTED_PULSE_MIN 0.5f
#define OP_MENU_SELECTED_PULSE_MAX 1.0f
#define OP_MODE_PI 3.14159265358979323846f
/* The scale picker's own "available but not selected" level -- Sentia
 * magenta (this file's OP_MENU_MELODIC_* above), at a readable-but-
 * secondary brightness, distinct from the pulsing white selection.
 * Raised from 0.35, real feedback: "we could have idle led in scale mode
 * more bright not as dim." */
#define OP_SCALE_AVAILABLE_LEVEL 0.5f

static float menu_selected_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / OP_MENU_SELECTED_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
    return OP_MENU_SELECTED_PULSE_MIN + (OP_MENU_SELECTED_PULSE_MAX - OP_MENU_SELECTED_PULSE_MIN) * raw;
}

/* Real feedback: "when you touch but not click in menu make a strong
 * haptic click be felt, push pad to at least mroe than 50% to sleect and
 * selected pad give a constant haptic pulsing pattern to indicate its
 * the active one." Applies to both selection-style menus this file has
 * (the mode-picker and the scale-picker) -- sequencer mode's own step
 * taps are a different interaction (toggle on/off, not "pick one") and
 * deliberately unchanged.
 * 900 (tiles_hall_get_depth()'s own rough full-scale range on this
 * hardware -- see expression.c's s_depth_to_aftertouch_full_scale,
 * 900, the same reference point) * 0.5 = "more than half pressed." */
#define OP_MENU_SELECT_DEPTH_THRESHOLD 450.0f
/* Fires once on every fresh touch, not proportional to anything -- a
 * flat, firm acknowledgment ("you're touching something"), distinct
 * from the softer tiles_haptics_trigger_touch_pulse() normal play uses,
 * matching "strong" in the real feedback above. */
#define OP_MENU_TOUCH_CLICK_VELOCITY 110u
/* The active-selection pulse's own haptic strength -- lighter than the
 * touch click above, it repeats continuously (see
 * OP_MENU_SELECTED_PULSE_PERIOD_MS) rather than firing once, so a
 * click-equivalent strength every cycle would read as nagging rather
 * than a gentle "this is still the one" heartbeat. */
#define OP_MENU_SELECTED_HAPTIC_VELOCITY 70u

/* ---- Master tap tempo (SW6/circle) + beat flash -------------------------
 * Real feedback: "lets make the midi clock work in a way where we can do
 * master tap tempo on the instrument with shit round button when not
 * derecting midi clock from a daw. the tapp tempo is only active in
 * sequencer and arp mode and requiere 4 taps to calcualte minimum. and
 * then flash that light as the tempo even when midi sync flash the tempo
 * there." Circle is this board's established "shift" button
 * (services/standby.h's own file header); a qualifying press-edge (see
 * handle_circle_tap() below) feeds services/midi_clock.h's tap-tempo API
 * directly -- that file owns the actual averaging/generation, this one
 * only owns deciding WHEN a press counts and rendering the beat flash.
 * The flash itself is driven purely by tiles_midi_clock_get_state()'s
 * shared pulse_count, so it shows the beat identically whether that
 * count is currently coming from real external clock bytes or the
 * internal tap-tempo generator -- no special-casing needed, matching the
 * "flash the tempo there [too]" half of the ask. */
#define OP_BEAT_FLASH_DURATION_MS 100u
#define OP_BEAT_FLASH_LEVEL 1.0f
/* 24 clock pulses per quarter note, fixed by the MIDI spec -- one beat
 * flash per quarter note, not per individual clock pulse. */
#define OP_CLOCK_PULSES_PER_BEAT 24u

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

/* ---- Per-mode sub-menu (SW3/triangle) ---------------------------------
 * Real feedback: "the triangle is sub menues per each mode so that
 * button toggles its onw menue in each mode. for melodic it toggles
 * different scale modes." Only melodic's sub-menu (the scale picker) is
 * built -- other modes don't have one yet, same "selectable but not
 * implemented" spirit as chord/arp themselves; handle_triangle_click()
 * below is the extension point once they do. */
static bool s_scale_menu_visible;
static bool s_triangle_was_held;
static bool s_triangle_press_had_conflict; /* see s_diamond_press_had_conflict's own comment -- same reasoning, watches diamond instead of triangle */
static bool s_scale_menu_prev_pad_touched[TILES_NUM_PADS];

/* Tap-tempo press tracking -- see this file's own "Master tap tempo"
 * section above. No conflict flag like diamond/triangle's own: a tap
 * must register on the PRESS edge (not release) for accurate timing, so
 * handle_circle_tap() checks the other three combo buttons' CURRENT
 * state at the moment of the press instead of latching a flag to check
 * on release. */
static bool s_circle_was_held;
/* Beat-flash rendering state -- shared across whichever mode is
 * currently showing it (render_sequencer() directly, or the diamond-
 * style button override for arp mode), computed once per scan in
 * tiles_op_mode_scan() so both paths see the identical flash timing. */
static uint32_t s_last_beat_index = 0xFFFFFFFFu;
static uint32_t s_beat_flash_start_ms;

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

/* Takes the clock snapshot as a parameter rather than calling
 * tiles_midi_clock_get_state() itself -- that function consumes
 * (clears) start_edge as a side effect, and tiles_op_mode_scan() also
 * needs the same snapshot for the beat flash (see this file's own
 * "Master tap tempo" section) -- fetching it twice in one scan would
 * silently drop a real start_edge on whichever call ran second. */
static void seq_advance_clock(tiles_midi_clock_state_t clock) {
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

static void render_sequencer(float beat_flash_level) {
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
        float level = 0.0f;
        if (col == TILES_DIAMOND_BUTTON_COL) {
            level = OP_DIAMOND_LED_MODE_ACTIVE_LEVEL;
        } else if (col == TILES_CIRCLE_BUTTON_COL) {
            /* Real feedback: "flash that light as the tempo." */
            level = beat_flash_level;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Menu -------------------------------------------------------------- */

static void menu_enter(void) {
    s_menu_visible = true;
    /* The mode-picker takes priority over a still-open per-mode
     * sub-menu -- can't sensibly show both at once. */
    s_scale_menu_visible = false;
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

/* ---- Melodic mode's sub-menu: the scale picker -------------------------
 * "Ableton Push style": one pad per scale, the whole 24-pad grid, no
 * row/column structure -- see note_map.h's TILES_NOTE_MAP_NUM_SCALE_GRID_
 * SLOTS and the 18 named scales + 6 reserved slots that exactly fill it.
 * Follows the standardized menu language above: selected = white bright
 * pulsing, available-but-unselected = Sentia magenta at a readable-but-
 * secondary level, a reserved/undefined slot = fully off and not
 * selectable at all (tapping one is a no-op, see handle_scale_menu_taps()
 * below). */
/* Real feedback: "selected pad give a constant haptic pulsing pattern to
 * indicate its the active one." Fires tiles_haptics_trigger_kick() on
 * the currently-selected pad every OP_MENU_SELECTED_PULSE_PERIOD_MS,
 * synced to the same period the visual pulse uses -- a repeating pulse,
 * not a continuous buzz (tiles_haptics_set_sustain_level() needs an
 * already-active voice from trigger_kick() to mean anything, so a real
 * periodic re-kick is what actually produces a felt "pattern" here). */
static uint32_t s_scale_menu_haptic_pulse_ms;

static void render_scale_menu(uint32_t now_ms) {
    float pulse = menu_selected_pulse_level(now_ms);
    tiles_scale_mode_t current = tiles_note_map_get_scale();
    uint8_t selected_pad = 0u;

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        tiles_scale_mode_t slot_scale = tiles_note_map_scale_for_grid_slot(pad);
        if (!tiles_note_map_scale_is_defined(slot_scale)) {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        } else if (slot_scale == current) {
            tiles_lighting_set_standby_pad_rgb(pad, pulse, pulse, pulse);
            selected_pad = pad;
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R * OP_SCALE_AVAILABLE_LEVEL,
                                                OP_MENU_MELODIC_G * OP_SCALE_AVAILABLE_LEVEL,
                                                OP_MENU_MELODIC_B * OP_SCALE_AVAILABLE_LEVEL);
        }
    }
    if (selected_pad != 0u && (now_ms - s_scale_menu_haptic_pulse_ms) >= (uint32_t)OP_MENU_SELECTED_PULSE_PERIOD_MS) {
        s_scale_menu_haptic_pulse_ms = now_ms;
        tiles_haptics_trigger_kick(selected_pad, OP_MENU_SELECTED_HAPTIC_VELOCITY);
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = (col == TILES_TRIANGLE_BUTTON_COL) ? OP_TRIANGLE_LED_SUBMENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void scale_menu_exit(void);

/* Real feedback: "when you touch but not click in menu make a strong
 * haptic click be felt, push pad to at least mroe than 50% to sleect."
 * A fresh capacitive touch alone only fires the click (an
 * acknowledgment, "you're touching this") -- selection itself only
 * commits once Hall depth crosses OP_MENU_SELECT_DEPTH_THRESHOLD while
 * still touched.
 * Selecting now closes the menu immediately -- real feedback: "when we
 * select a menu item the menu should close not it doesnt stick around
 * until disabeled." An earlier version deliberately left it open (so
 * different scales could be tried while watching/hearing the
 * difference); real feedback reversed that call -- matches the
 * mode-picker's own close-on-select behavior now, one consistent rule
 * for both menus in this file. Re-opening (a fresh triangle click) shows
 * whatever's now selected pulsing, same as before. */
static void handle_scale_menu_taps(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        if (touched && !s_scale_menu_prev_pad_touched[pad - 1u]) {
            tiles_haptics_trigger_kick(pad, OP_MENU_TOUCH_CLICK_VELOCITY);
        }
        if (touched && (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD) {
            tiles_scale_mode_t slot_scale = tiles_note_map_scale_for_grid_slot(pad);
            if (tiles_note_map_scale_is_defined(slot_scale)) {
                if (slot_scale != tiles_note_map_get_scale()) {
                    printf("[op_mode] scale -> %d\n", (int)slot_scale);
                }
                tiles_note_map_set_scale(slot_scale);
                scale_menu_exit();
                return; /* grid ownership just changed under this loop -- stop iterating it */
            }
            /* An undefined (reserved custom) slot is simply not
             * selectable -- "unavailable" per the standardized menu
             * language, not a smaller version of a real choice. */
        }
        s_scale_menu_prev_pad_touched[pad - 1u] = touched;
    }
}

static void scale_menu_enter(void) {
    s_scale_menu_visible = true;
    s_scale_menu_haptic_pulse_ms = to_ms_since_boot(get_absolute_time());
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_scale_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void scale_menu_exit(void) {
    s_scale_menu_visible = false;
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
}

static void set_active_mode(tiles_op_mode_t mode) {
    if (s_active_mode == OP_MODE_SEQUENCER && mode != OP_MODE_SEQUENCER) {
        seq_end_current_note();
    }
    if (s_active_mode == OP_MODE_ARP && mode != OP_MODE_ARP) {
        /* Releases circle's beat-flash override -- see this file's own
         * "Master tap tempo" section for why this is claimed/released
         * around arp mode specifically rather than permanently like
         * diamond's own override: circle has a real pre-existing "LED
         * follows press" default (services/standby.h's screensaver/
         * deep-sleep hold gesture's own visual feedback) that claiming
         * it permanently would silently break outside sequencer/arp
         * mode. Releasing immediately re-asserts that default per
         * buttons.h's own documented contract. */
        tiles_buttons_set_override_active(TILES_CIRCLE_BUTTON_ID, false);
    }
    if (mode != OP_MODE_MELODIC) {
        /* Melodic's own sub-menu can't stay open once melodic isn't the
         * active mode anymore. */
        s_scale_menu_visible = false;
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
    if (mode == OP_MODE_ARP) {
        tiles_buttons_set_override_active(TILES_CIRCLE_BUTTON_ID, true);
    }
    tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, mode == OP_MODE_MELODIC ? 0.0f : OP_DIAMOND_LED_MODE_ACTIVE_LEVEL);
    printf("[op_mode] active mode -> %d\n", (int)mode);
}

/* Same touch-clicks/press-past-50%-selects shape as
 * handle_scale_menu_taps() above -- see that function's own comment and
 * OP_MENU_SELECT_DEPTH_THRESHOLD's. Picking a mode still closes the menu
 * immediately (no persistent "selected, still browsing" state here the
 * way the scale picker has -- a mode activates and takes over the grid
 * the instant it's confirmed), so there's no pulsing-haptic step for
 * this menu specifically. */
static void handle_menu_taps(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            if (touched && !s_menu_prev_pad_touched[pad - 1u]) {
                tiles_haptics_trigger_kick(pad, OP_MENU_TOUCH_CLICK_VELOCITY);
            }
            if (touched && (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD) {
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
                return; /* grid ownership/state just changed under this loop -- stop iterating it */
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

/* Same shape as handle_diamond_click() above, one level down: toggles
 * whichever per-mode sub-menu (if any) the CURRENT mode has. Only
 * melodic has one right now. Guarded against the mode-picker also being
 * open (a sub-menu click while picking a top-level mode would be
 * ambiguous/unwanted) the same way the mode-picker itself is guarded
 * against other_feature_owns_input(). */
static void handle_triangle_click(void) {
    bool held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);

    if (held && !s_triangle_was_held) {
        s_triangle_press_had_conflict = false;
    }
    if (held && tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID)) {
        s_triangle_press_had_conflict = true;
    }

    if (!held && s_triangle_was_held) {
        if (!s_triangle_press_had_conflict && !s_menu_visible && s_active_mode == OP_MODE_MELODIC) {
            if (s_scale_menu_visible) {
                scale_menu_exit();
            } else {
                scale_menu_enter();
            }
        }
    }

    s_triangle_was_held = held;
}

/* Real feedback: "master tap tempo on the instrument with shit round
 * button when not derecting midi clock from a daw. the tapp tempo is
 * only active in sequencer and arp mode." Registers on the PRESS edge
 * (not release, unlike diamond/triangle's click handlers) -- accurate
 * tap timing needs the moment of contact, not release. Qualifies a press
 * as a genuine tap only if: sequencer or arp mode is the active mode,
 * no real external clock is currently detected (services/midi_clock.h's
 * own tiles_midi_clock_register_tap() also defensively re-checks this),
 * and none of game_mode.h's other three reserved combo buttons (SW3
 * triangle/SW4 diamond/SW5 square) are ALSO currently held -- the same
 * "not part of the 4-button combo" heuristic diamond/triangle's own
 * click handlers use, just checked at press-time instead of release-time
 * since a tap can't wait for release. */
static void handle_circle_tap(uint32_t now_ms) {
    bool held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);

    if (held && !s_circle_was_held) {
        bool mode_ok = (s_active_mode == OP_MODE_SEQUENCER || s_active_mode == OP_MODE_ARP);
        bool combo_conflict = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
        if (mode_ok && !combo_conflict && !tiles_midi_clock_external_active(now_ms)) {
            tiles_midi_clock_register_tap(now_ms);
        }
    }

    s_circle_was_held = held;
}

/* Real feedback: "flash that light as the tempo even when midi sync
 * flash the tempo there." Fires once per quarter note (24 clock pulses,
 * the MIDI spec's own resolution), regardless of whether pulse_count is
 * currently advancing from real external clock bytes or the internal
 * tap-tempo generator -- see services/midi_clock.h's own file header for
 * why that distinction doesn't need to leak into this function at all. */
static float compute_beat_flash_level(uint32_t now_ms, tiles_midi_clock_state_t clock) {
    if (!clock.running) {
        return 0.0f;
    }
    uint32_t beat_index = clock.pulse_count / OP_CLOCK_PULSES_PER_BEAT;
    if (beat_index != s_last_beat_index) {
        s_last_beat_index = beat_index;
        s_beat_flash_start_ms = now_ms;
    }
    if ((now_ms - s_beat_flash_start_ms) < OP_BEAT_FLASH_DURATION_MS) {
        return OP_BEAT_FLASH_LEVEL;
    }
    return 0.0f;
}

void tiles_op_mode_init(void) {
    s_active_mode = OP_MODE_MELODIC;
    s_menu_visible = false;
    s_diamond_was_held = false;
    s_diamond_press_had_conflict = false;
    s_scale_menu_visible = false;
    s_triangle_was_held = false;
    s_triangle_press_had_conflict = false;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        s_seq_step_armed[i] = false;
    }
    s_seq_current_step = 0u;
    s_seq_note_sounding = false;
    s_seq_step_started_at_pulse = 0u;
    s_circle_was_held = false;
    s_last_beat_index = 0xFFFFFFFFu;
    s_beat_flash_start_ms = 0u;
    tiles_buttons_set_override_active(TILES_DIAMOND_BUTTON_ID, true);
    tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, 0.0f);
    /* Circle's own override is claimed/released around arp mode
     * specifically, not here at boot -- see set_active_mode()'s own
     * comment for why a permanent claim (like diamond's) would break
     * circle's pre-existing "LED follows press" default. */
}

void tiles_op_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (other_feature_owns_input()) {
        /* Keep edge-tracking current so a button/touch already active the
         * instant control hands back doesn't misread as a fresh
         * click/tap -- same pattern services/expression_control.c and
         * services/game_mode.c already use for their own equivalent
         * guards. */
        s_diamond_was_held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
        s_triangle_was_held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
        s_circle_was_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
        return;
    }

    handle_diamond_click();
    handle_triangle_click();
    handle_circle_tap(now_ms);

    /* Fetched exactly once per scan -- tiles_midi_clock_get_state()
     * consumes start_edge as a side effect (see midi_clock.h's own
     * comment), and both the sequencer's own clock-advance below and the
     * beat-flash computation need to see the SAME snapshot. */
    tiles_midi_clock_state_t clock = tiles_midi_clock_get_state();
    float beat_flash_level = compute_beat_flash_level(now_ms, clock);

    if (s_menu_visible) {
        handle_menu_taps();
        render_menu();
        return;
    }

    if (s_scale_menu_visible) {
        handle_scale_menu_taps();
        render_scale_menu(now_ms);
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER) {
        seq_handle_step_taps();
        seq_advance_clock(clock);
        render_sequencer(beat_flash_level);
    } else if (s_active_mode == OP_MODE_ARP) {
        /* Arp mode itself isn't implemented yet (see this file's own
         * header) -- this is the one real piece of it: circle's beat
         * flash, driven by the exact same tap-tempo/external-clock state
         * sequencer mode uses. Override was claimed in set_active_mode()
         * on entering arp mode. */
        tiles_buttons_set_override_led(TILES_CIRCLE_BUTTON_ID, beat_flash_level);
    }
}

bool tiles_op_mode_owns_pad_grid(void) {
    return s_menu_visible || s_scale_menu_visible || s_active_mode == OP_MODE_SEQUENCER;
}
