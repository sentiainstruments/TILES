#include "octave_control.h"

#include "board_layout.h"
#include "buttons.h"
#include "expression_control.h"
#include "game_mode.h"
#include "lighting.h"
#include "note_map.h"
#include "op_mode.h"
#include "pixel_font.h"
#include "standby.h"

#include "pico/time.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define BUTTON_ID_MINUS 1u /* SW1, left capsule */
#define BUTTON_ID_PLUS 2u  /* SW2, right capsule */

#define OCTAVE_CONTROL_PI 3.14159265358979323846f

/* One shared building block, "a pulse": a raised-cosine bump rising
 * smoothly from the dim rest level up to full and back down -- no hard
 * edge anywhere. Every magnitude is built from repeats of this exact
 * same shape so the three animations read as one coherent family (just
 * "how many pulses, then how long a rest") instead of three unrelated
 * effects -- direct fix for real feedback that magnitude 2 and 3 didn't
 * pulse evenly with each other. Both periods below were slowed from an
 * initial pass that read as "too fast" across the board on real
 * hardware -- magnitude 1 especially, since it never rests between
 * pulses and so needs to be noticeably slower on its own to still read
 * as a calm single breath rather than a continuous flutter. */
#define OCTAVE_PULSE1_PERIOD_MS 1400.0f
#define OCTAVE_PULSE_UNIT_MS 650.0f
#define OCTAVE_PULSE_REST_MS 650u
#define OCTAVE_PULSE_REST_LEVEL 0.15f
#define OCTAVE_PULSE_PEAK_LEVEL 1.0f

static float pulse_unit_level(float phase01) {
    float raw = 0.5f * (1.0f - cosf(2.0f * OCTAVE_CONTROL_PI * phase01));
    return OCTAVE_PULSE_REST_LEVEL + (OCTAVE_PULSE_PEAK_LEVEL - OCTAVE_PULSE_REST_LEVEL) * raw;
}

/* Magnitude 1: the same pulse shape repeating back to back forever --
 * "pulses even," no burst/rest structure at all, just a steady regular
 * breathing -- but at its own, slower period (OCTAVE_PULSE1_PERIOD_MS)
 * rather than the burst unit magnitude 2/3 use, since a pulse that never
 * pauses reads as much faster than the same period would in a burst.
 * Replaces the earlier flat-solid magnitude-1 look per real feedback
 * that one click should read as pulsing too, not just lit. */
static float magnitude1_level(uint32_t now_ms) {
    float phase = fmodf((float)now_ms / OCTAVE_PULSE1_PERIOD_MS, 1.0f);
    return pulse_unit_level(phase);
}

/* Magnitude 2 and 3: `magnitude` unit pulses (OCTAVE_PULSE_UNIT_MS each)
 * back to back, then a dim (not fully dark) rest, then the whole burst
 * repeats. Magnitude 3 is literally magnitude 2's shape plus one more
 * pulse appended before the same rest -- not a separately-tuned
 * animation -- per real feedback that the two should be "the same, just
 * with an additional pulse followed by a rest in dim." Unmeasured --
 * a starting guess at pacing. */
static float magnitude_burst_level(uint8_t magnitude, uint32_t now_ms) {
    uint32_t burst_ms = (uint32_t)((float)magnitude * OCTAVE_PULSE_UNIT_MS);
    uint32_t cycle_ms = burst_ms + OCTAVE_PULSE_REST_MS;
    uint32_t t = now_ms % cycle_ms;
    if (t >= burst_ms) {
        return OCTAVE_PULSE_REST_LEVEL; /* dim rest between bursts */
    }
    float within = fmodf((float)t, OCTAVE_PULSE_UNIT_MS);
    return pulse_unit_level(within / OCTAVE_PULSE_UNIT_MS);
}

static float level_for_magnitude(uint8_t magnitude, uint32_t now_ms) {
    switch (magnitude) {
    case 1u:
        return magnitude1_level(now_ms);
    case 2u:
    case 3u:
        return magnitude_burst_level(magnitude, now_ms);
    default:
        return 0.0f;
    }
}

/* Both held together for this long counts as "click them together" --
 * short enough to still feel instantaneous, long enough to reliably
 * distinguish a deliberate combo from two independent presses that
 * happen to briefly overlap. Unmeasured -- a starting guess. */
#define TRANSPOSE_COMBO_HOLD_MS 120u

/* Natural-note letter + whether that semitone is the sharp of it, one
 * entry per tiles_note_map_get_key_offset() value 0-11 (0 = C). */
typedef struct {
    char letter;
    bool sharp;
} tiles_key_info_t;

static const tiles_key_info_t s_key_table[12] = {
    {'C', false}, {'C', true}, {'D', false}, {'D', true}, {'E', false}, {'F', false},
    {'F', true},  {'G', false}, {'G', true},  {'A', false}, {'A', true}, {'B', false},
};

/* Sharp-key flash: show the letter first, then alternate with the
 * cross. Both re-anchored to now_ms whenever transpose mode is entered
 * or the key changes, so a fresh letter is never caught mid-cross.
 * Unmeasured -- a starting guess at pacing. */
#define TRANSPOSE_FLASH_LETTER_MS 900u
#define TRANSPOSE_FLASH_CROSS_MS 500u
#define TRANSPOSE_LETTER_LEVEL 0.9f
/* Warm amber rather than reusing white for the cross -- a deliberate,
 * if unrequested, color distinction so the sharp flash reads
 * unambiguously as a different signal from the letter itself rather
 * than as a glitch. */
#define TRANSPOSE_CROSS_R 1.0f
#define TRANSPOSE_CROSS_G 0.55f
#define TRANSPOSE_CROSS_B 0.0f
/* The cross: a proper plus sign contained in a 4x4 box (matching the
 * pixel font's own 4x4 glyph size), not a bar spanning the full 6-wide
 * grid -- real feedback that the horizontal arm was too long relative
 * to the vertical one. Vertical arm: the two middle columns (3, 4),
 * full 4-row height. Horizontal arm: row 2, but only cols 2-5 (4 wide,
 * centered) -- not the full 1-6 width. */
#define TRANSPOSE_CROSS_ROW 2u
#define TRANSPOSE_CROSS_COL_A 3u
#define TRANSPOSE_CROSS_COL_B 4u
#define TRANSPOSE_CROSS_HBAR_COL_MIN 2u
#define TRANSPOSE_CROSS_HBAR_COL_MAX 5u

static bool s_prev_minus_pressed;
static bool s_prev_plus_pressed;
/* True once the CURRENT press of that button has been part of both_held
 * at any point -- suppresses that press's eventual release from firing
 * a solo octave/key step. Reset on that button's own fresh press-edge.
 * See tiles_octave_control_scan()'s own comment on why the solo step
 * fires on release, not press, and why that's what this exists for. */
static bool s_minus_became_combo;
static bool s_plus_became_combo;

static bool s_combo_was_held;
static bool s_combo_fired;
static uint32_t s_combo_hold_start_ms;

static bool s_transpose_mode;
static uint32_t s_transpose_flash_anchor_ms;

void tiles_octave_control_init(void) {
    s_prev_minus_pressed = false;
    s_prev_plus_pressed = false;
    s_minus_became_combo = false;
    s_plus_became_combo = false;
    s_combo_was_held = false;
    s_combo_fired = false;
    s_transpose_mode = false;
    tiles_buttons_set_override_active(BUTTON_ID_MINUS, true);
    tiles_buttons_set_override_active(BUTTON_ID_PLUS, true);
}

bool tiles_octave_control_is_transpose_active(void) {
    return s_transpose_mode;
}

static void transpose_toggle(uint32_t now_ms) {
    s_transpose_mode = !s_transpose_mode;
    s_transpose_flash_anchor_ms = now_ms;
    /* Claims/releases the pad grid exactly like standby.c's own
     * animations and game_mode.c do -- see buttons/lighting's standby-
     * active doc comments for why this also immediately repaints
     * correctly on release. */
    tiles_lighting_set_standby_active(s_transpose_mode);
}

/* Centers a 4-row/N-col glyph in the 6-column grid and lights it via
 * the standby pad-RGB path; row 0 (buttons) is untouched here since
 * SW1/SW2's LEDs are driven separately below and the other 4 buttons
 * aren't part of this display. */
static void render_transpose_letter(const tiles_glyph_t *glyph) {
    uint8_t grid_width = (uint8_t)(TILES_GRID_MAX_COL - TILES_GRID_MIN_COL + 1u);
    uint8_t col_start = (uint8_t)(TILES_GRID_MIN_COL + (grid_width - glyph->width) / 2u);

    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t row_bit = (uint8_t)(1u << (row - 1u));
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            bool lit = false;
            if (col >= col_start && col < (uint8_t)(col_start + glyph->width)) {
                lit = (glyph->cols[col - col_start] & row_bit) != 0u;
            }
            float level = lit ? TRANSPOSE_LETTER_LEVEL : 0.0f;
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), level, level, level);
        }
    }
}

static void render_transpose_cross(void) {
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            bool horiz_arm = (row == TRANSPOSE_CROSS_ROW) && (col >= TRANSPOSE_CROSS_HBAR_COL_MIN) &&
                             (col <= TRANSPOSE_CROSS_HBAR_COL_MAX);
            bool vert_arm = (col == TRANSPOSE_CROSS_COL_A) || (col == TRANSPOSE_CROSS_COL_B);
            bool lit = horiz_arm || vert_arm;
            if (lit) {
                tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), TRANSPOSE_CROSS_R,
                                                    TRANSPOSE_CROSS_G, TRANSPOSE_CROSS_B);
            } else {
                tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), 0.0f, 0.0f, 0.0f);
            }
        }
    }
}

static void render_transpose_frame(uint32_t now_ms) {
    const tiles_key_info_t *key = &s_key_table[tiles_note_map_get_key_offset()];

    bool show_cross = false;
    if (key->sharp) {
        uint32_t cycle_ms = TRANSPOSE_FLASH_LETTER_MS + TRANSPOSE_FLASH_CROSS_MS;
        uint32_t phase = (now_ms - s_transpose_flash_anchor_ms) % cycle_ms;
        show_cross = phase >= TRANSPOSE_FLASH_LETTER_MS;
    }

    if (show_cross) {
        render_transpose_cross();
    } else {
        const tiles_glyph_t *glyph = tiles_pixel_font_glyph_for_note_letter(key->letter);
        render_transpose_letter(glyph);
    }

    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

void tiles_octave_control_scan(void) {
    bool minus_pressed = tiles_button_is_pressed(BUTTON_ID_MINUS);
    bool plus_pressed = tiles_button_is_pressed(BUTTON_ID_PLUS);

    if (tiles_game_mode_is_active() || tiles_standby_owns_octave_buttons() ||
        tiles_expression_control_owns_pad_grid() || tiles_op_mode_owns_octave_buttons()) {
        /* A game has claimed SW1/SW2 as its own controls -- see the
         * "Deferring to game mode" section of the file header -- or a
         * manually-entered screensaver has repurposed them as
         * animation-scroll controls (see standby.h's
         * tiles_standby_owns_octave_buttons()) -- or the expression
         * sub-menu is showing (square held alone, adjusting haptic
         * intensity via SW1/SW2 directly, or just passively visible with
         * SW1/SW2 otherwise idle -- see expression_control.h's
         * tiles_expression_control_owns_pad_grid()) -- or op_mode.h's
         * mode-select menu/sequencer/guitar mode owns "-"/"+"
         * (tiles_op_mode_owns_octave_buttons(), broader than that file's
         * own tiles_op_mode_owns_pad_grid() specifically so guitar mode
         * can take over just these two buttons without also suppressing
         * real note playing the way full grid ownership would). Same fix
         * either way: keep edge-tracking state current and do nothing
         * else, so a scroll/game/intensity/mode-select/fret-shift press
         * -- or a press meant only to dismiss the sub-menu -- doesn't
         * *also* silently step the octave or transpose key underneath. */
        s_prev_minus_pressed = minus_pressed;
        s_prev_plus_pressed = plus_pressed;
        s_combo_was_held = minus_pressed && plus_pressed;
        return;
    }

    bool both_held = minus_pressed && plus_pressed;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Real feedback: "transpose menu enter and exit accidentally
     * triggers octave up or down on enter or exit because button
     * [presses land] before entering menu because simultaneous press is
     * impossible." True: a human can never press SW1/SW2 in exactly the
     * same tick, so the first of the two to register used to read as a
     * genuine solo press (both_held still false at that instant) and
     * fired a real octave/key step an instant before the second button
     * joined and the combo took over -- both entering AND exiting
     * transpose mode this way. Fixed by moving the solo step from the
     * PRESS edge to the RELEASE edge, gated on whether this press ever
     * became part of both_held at any point during its hold
     * (s_minus_became_combo/s_plus_became_combo, reset on that button's
     * own fresh press) -- so a press that's about to become half of a
     * combo never fires its solo action first, no matter which button's
     * press happened to land a few ms earlier. Same "click vs. long
     * action" shape services/expression_control.c's own square-button
     * handling already uses. */
    if (minus_pressed && !s_prev_minus_pressed) {
        s_minus_became_combo = false;
    }
    if (plus_pressed && !s_prev_plus_pressed) {
        s_plus_became_combo = false;
    }

    if (both_held && !s_combo_was_held) {
        s_combo_hold_start_ms = now_ms;
        s_combo_fired = false;
    }
    if (both_held) {
        s_minus_became_combo = true;
        s_plus_became_combo = true;
        if (!s_combo_fired && (now_ms - s_combo_hold_start_ms) >= TRANSPOSE_COMBO_HOLD_MS) {
            s_combo_fired = true;
            transpose_toggle(now_ms);
        }
    }
    s_combo_was_held = both_held;

    if (!minus_pressed && s_prev_minus_pressed && !s_minus_became_combo) {
        if (s_transpose_mode) {
            tiles_note_map_set_key_offset((int8_t)(tiles_note_map_get_key_offset() - 1));
            s_transpose_flash_anchor_ms = now_ms;
        } else {
            tiles_note_map_set_octave_shift((int8_t)(tiles_note_map_get_octave_shift() - 1));
        }
    }
    if (!plus_pressed && s_prev_plus_pressed && !s_plus_became_combo) {
        if (s_transpose_mode) {
            tiles_note_map_set_key_offset((int8_t)(tiles_note_map_get_key_offset() + 1));
            s_transpose_flash_anchor_ms = now_ms;
        } else {
            tiles_note_map_set_octave_shift((int8_t)(tiles_note_map_get_octave_shift() + 1));
        }
    }
    s_prev_minus_pressed = minus_pressed;
    s_prev_plus_pressed = plus_pressed;

    if (s_transpose_mode) {
        /* Both LEDs pulse together -- same phase, since both come from
         * the same now_ms with no per-button offset. */
        float pulse = magnitude1_level(now_ms);
        tiles_buttons_set_override_led(BUTTON_ID_MINUS, pulse);
        tiles_buttons_set_override_led(BUTTON_ID_PLUS, pulse);
        render_transpose_frame(now_ms);
        return;
    }

    int8_t shift = tiles_note_map_get_octave_shift();
    float minus_level = 0.0f;
    float plus_level = 0.0f;
    if (shift != 0) {
        uint8_t magnitude = (uint8_t)(shift < 0 ? -shift : shift);
        float level = level_for_magnitude(magnitude, now_ms);
        if (shift < 0) {
            minus_level = level;
        } else {
            plus_level = level;
        }
    }

    tiles_buttons_set_override_led(BUTTON_ID_MINUS, minus_level);
    tiles_buttons_set_override_led(BUTTON_ID_PLUS, plus_level);
}
