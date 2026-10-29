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

#define BUTTON_ID_MINUS 1u /* SW1, "-" */
#define BUTTON_ID_PLUS 2u  /* SW2, "+" */

#define OCTAVE_CONTROL_PI 3.14159265358979323846f

/* One building block, "a pulse": a raised-cosine bump from the dim rest
 * level to full and back, no hard edges. Every magnitude repeats this
 * shape, so the three read as one family. Magnitude 1 never rests, so it
 * runs at its own slower period to read as a calm breath. */
#define OCTAVE_PULSE1_PERIOD_MS 1400.0f
#define OCTAVE_PULSE_UNIT_MS 650.0f
#define OCTAVE_PULSE_REST_MS 650u
#define OCTAVE_PULSE_REST_LEVEL 0.15f
#define OCTAVE_PULSE_PEAK_LEVEL 1.0f

static float pulse_unit_level(float phase01) {
    float raw = 0.5f * (1.0f - cosf(2.0f * OCTAVE_CONTROL_PI * phase01));
    return OCTAVE_PULSE_REST_LEVEL + (OCTAVE_PULSE_PEAK_LEVEL - OCTAVE_PULSE_REST_LEVEL) * raw;
}

/* Magnitude 1: the pulse repeating back to back at its own slower period. */
static float magnitude1_level(uint32_t now_ms) {
    float phase = fmodf((float)now_ms / OCTAVE_PULSE1_PERIOD_MS, 1.0f);
    return pulse_unit_level(phase);
}

/* Magnitudes 2 and 3: `magnitude` pulses back to back, then a dim rest,
 * repeat (3 is 2 plus one more pulse). Pacing is a starting guess. */
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

/* Both held this long = "clicked together": feels instant, but separates a
 * deliberate combo from two overlapping presses. Starting guess. */
#define TRANSPOSE_COMBO_HOLD_MS 120u

/* Note letter and sharp flag per key offset 0-11 (0 = C). */
typedef struct {
    char letter;
    bool sharp;
} tiles_key_info_t;

static const tiles_key_info_t s_key_table[12] = {
    {'C', false}, {'C', true}, {'D', false}, {'D', true}, {'E', false}, {'F', false},
    {'F', true},  {'G', false}, {'G', true},  {'A', false}, {'A', true}, {'B', false},
};

/* Sharp keys: letter first, then alternate with the cross. Re-anchored on
 * entering the mode or changing key. Starting guess. */
#define TRANSPOSE_FLASH_LETTER_MS 900u
#define TRANSPOSE_FLASH_CROSS_MS 500u
#define TRANSPOSE_LETTER_LEVEL 0.9f
/* Amber cross so the sharp flash reads as a separate signal, not a glitch. */
#define TRANSPOSE_CROSS_R 1.0f
#define TRANSPOSE_CROSS_G 0.55f
#define TRANSPOSE_CROSS_B 0.0f
/* The cross is a plus inside a 4x4 box (the font's glyph size): vertical
 * arm = columns 3-4, all 4 rows; horizontal arm = row 2, columns 2-5. */
#define TRANSPOSE_CROSS_ROW 2u
#define TRANSPOSE_CROSS_COL_A 3u
#define TRANSPOSE_CROSS_COL_B 4u
#define TRANSPOSE_CROSS_HBAR_COL_MIN 2u
#define TRANSPOSE_CROSS_HBAR_COL_MAX 5u

static bool s_prev_minus_pressed;
static bool s_prev_plus_pressed;
/* Set once the CURRENT press of that button has been part of both_held;
 * stops its release from also firing a solo step. Cleared on its next
 * press edge. */
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
    /* Claims/releases the pad grid the same way standby and game mode do. */
    tiles_lighting_set_standby_active(s_transpose_mode);
}

/* Centers a 4-row glyph in the 6-column grid via the standby pad-RGB path.
 * Buttons aren't drawn here. */
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
        /* Another module owns "-"/"+" (game mode, the manual screensaver's
         * scrolling, the expression menu, or op_mode's menus/sequencer/bass guitar
         * fret shift; see the header). Keep edge tracking current and do nothing
         * else, so those presses don't also step the octave or key. */
        s_prev_minus_pressed = minus_pressed;
        s_prev_plus_pressed = plus_pressed;
        s_combo_was_held = minus_pressed && plus_pressed;
        return;
    }

    bool both_held = minus_pressed && plus_pressed;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Solo steps fire on RELEASE, and only if this press never became part of
     * both_held. A human never presses both in the same tick, so firing on
     * press made the first button of the combo step the octave on the way in
     * and out of transpose mode. */
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
        /* Same now_ms, so both LEDs pulse in phase. */
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
