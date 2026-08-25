#include "boot_sequence.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "hall.h"
#include "lighting.h"

#include "pico/time.h"

#include <math.h>

#define BOOT_FRAME_INTERVAL_MS 30u

/* ---- Phase 1: white ripple rising from bottom-center, underglow off ---- */

#define RIPPLE_DURATION_MS 900u
#define RIPPLE_ORIGIN_ROW 4.0f /* bottom row */
#define RIPPLE_ORIGIN_COL 3.5f /* bottom CENTER, between columns 3 and 4 */
/* Covers the farthest corner (row 1, col 1 or col 6) plus the button
 * row, which sits even further out -- so buttons are naturally the last
 * thing the rising ripple reaches, with no special-casing needed. */
#define RIPPLE_MAX_RADIUS 4.6f
#define RIPPLE_EDGE_WIDTH 1.0f /* soft leading edge, not a hard on/off line */
#define RIPPLE_STEADY_LEVEL 0.9f /* brightness behind the wavefront, once revealed */

/* ---- Phase 2: fade the ripple's end state to complete dark ---- */

#define FADE_DURATION_MS 500u

/* ---- Phase 3: single Sentia Instruments Magenta pulse ---- */

#define PULSE_RISE_MS 150u
#define PULSE_HOLD_MS 120u
#define PULSE_FALL_MS 350u
#define PULSE_TOTAL_MS (PULSE_RISE_MS + PULSE_HOLD_MS + PULSE_FALL_MS)

#define MAGENTA_R 1.0f
#define MAGENTA_G 0.0f
#define MAGENTA_B 1.0f

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* 0 before the ripple's leading edge reaches (row, col); ramps up to
 * RIPPLE_STEADY_LEVEL over RIPPLE_EDGE_WIDTH right as the edge passes,
 * and stays there behind it -- a "fill up" reveal, not a thin ring that
 * leaves darkness behind it. */
static float ripple_level(uint8_t row, uint8_t col, uint32_t elapsed_ms) {
    float t = (float)elapsed_ms / (float)RIPPLE_DURATION_MS;
    if (t > 1.0f) {
        t = 1.0f;
    }
    float radius = t * RIPPLE_MAX_RADIUS;

    float dr = (float)row - RIPPLE_ORIGIN_ROW;
    float dc = (float)col - RIPPLE_ORIGIN_COL;
    float dist = sqrtf(dr * dr + dc * dc);

    float edge = radius - dist;
    return clamp01(edge / RIPPLE_EDGE_WIDTH) * RIPPLE_STEADY_LEVEL;
}

static float pulse_envelope(uint32_t elapsed_ms) {
    if (elapsed_ms < PULSE_RISE_MS) {
        return (float)elapsed_ms / (float)PULSE_RISE_MS;
    }
    uint32_t after_rise = elapsed_ms - PULSE_RISE_MS;
    if (after_rise < PULSE_HOLD_MS) {
        return 1.0f;
    }
    uint32_t after_hold = after_rise - PULSE_HOLD_MS;
    if (after_hold < PULSE_FALL_MS) {
        return 1.0f - (float)after_hold / (float)PULSE_FALL_MS;
    }
    return 0.0f;
}

typedef float (*white_level_fn_t)(uint8_t row, uint8_t col, uint32_t elapsed_ms);

static void write_frame_white(white_level_fn_t pad_level_fn, uint32_t elapsed_ms) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float v = pad_level_fn(0u, col, elapsed_ms);
        tiles_buttons_set_standby_led(board_button_for_col(col), v);
    }
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            float v = pad_level_fn(row, col, elapsed_ms);
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), v, v, v);
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        /* Underglow off through both phase 1 and phase 2 -- "without the
         * underglow" per the requested ripple, and there is nothing
         * meaningful for it to fade from if it was never lit. */
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static float phase1_level(uint8_t row, uint8_t col, uint32_t elapsed_ms) {
    return ripple_level(row, col, elapsed_ms);
}

/* Fades phase 1's end state (i.e. every cell already at
 * RIPPLE_STEADY_LEVEL, since phase 1 always runs to completion first)
 * down to 0 over FADE_DURATION_MS. */
static float phase2_level(uint8_t row, uint8_t col, uint32_t elapsed_ms) {
    (void)row;
    (void)col;
    float t = (float)elapsed_ms / (float)FADE_DURATION_MS;
    if (t > 1.0f) {
        t = 1.0f;
    }
    return RIPPLE_STEADY_LEVEL * (1.0f - t);
}

static void run_phase1_ripple(void) {
    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;
        bool done = elapsed >= RIPPLE_DURATION_MS;
        if (done) {
            elapsed = RIPPLE_DURATION_MS;
        }
        write_frame_white(phase1_level, elapsed);
        if (done) {
            break;
        }
        sleep_ms(BOOT_FRAME_INTERVAL_MS);
    }
}

static void run_phase2_fade(void) {
    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;
        bool done = elapsed >= FADE_DURATION_MS;
        if (done) {
            elapsed = FADE_DURATION_MS;
        }
        write_frame_white(phase2_level, elapsed);
        if (done) {
            break;
        }
        sleep_ms(BOOT_FRAME_INTERVAL_MS);
    }
}

static void run_phase3_magenta_pulse(void) {
    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;
        bool done = elapsed >= PULSE_TOTAL_MS;
        if (done) {
            elapsed = PULSE_TOTAL_MS;
        }
        float env = pulse_envelope(elapsed);

        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            tiles_buttons_set_standby_led(board_button_for_col(col), env);
        }
        for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
            for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
                tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), MAGENTA_R * env,
                                                    MAGENTA_G * env, MAGENTA_B * env);
            }
        }
        for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
            tiles_lighting_set_standby_underglow_rgb(i, MAGENTA_R * env, MAGENTA_G * env, MAGENTA_B * env);
        }

        if (done) {
            break;
        }
        sleep_ms(BOOT_FRAME_INTERVAL_MS);
    }
}

bool tiles_boot_sequence_run(void) {
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);

    run_phase1_ripple();
    run_phase2_fade();
    run_phase3_magenta_pulse();

    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);

    /* Use the couple of seconds this just took: re-capture the rest
     * baseline now that power/thermals have had a moment to settle,
     * instead of only ever trusting tiles_hall_init()'s very-first-
     * instant capture. See the file header. */
    return tiles_hall_recapture_baseline();
}
