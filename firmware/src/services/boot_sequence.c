#include "boot_sequence.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "hall.h"
#include "lighting.h"

#include "pico/time.h"

#include <math.h>

#define BOOT_FRAME_INTERVAL_MS 30u

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* Ease-in/ease-out -- used everywhere below instead of a linear ramp.
 * The original version's edges were linear and fairly narrow relative
 * to how fast they crossed the grid, which read as "jumpy" rather than
 * flowing; smoothstep plus wider edges/longer durations (below) is the
 * fix. */
static float smoothstep01(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

/* ---- Phase 1: white "rain" flooding down from the function buttons ---- */

/* Slower and with a much wider soft edge than the original version --
 * both were the likely source of "feels jumpy". Buttons (row 0) are the
 * source and light first; the flood works its way down through pad rows
 * 1-4, underglow off throughout (nothing to flood down into yet). */
#define RAIN_DURATION_MS 1600u
#define RAIN_ORIGIN_ROW 0.0f /* the button row -- where the flood starts */
#define RAIN_TARGET_ROW 4.0f /* the bottom pad row -- where it ends */
#define RAIN_EDGE_WIDTH 1.6f /* wide, soft leading edge */
#define RAIN_STEADY_LEVEL 0.85f
/* Small per-column timing offset so the flood doesn't look like a single
 * robotic row-by-row wipe -- deterministic (not real randomness, same
 * "looks organic without an RNG" trick standby.c's animations use), so
 * every boot looks the same rather than needing a seed this early. */
#define RAIN_COL_JITTER_MS 200.0f

static float rain_col_jitter_ms(uint8_t col) {
    return sinf((float)col * 2.3f) * RAIN_COL_JITTER_MS;
}

/* 0 before the flood front reaches (row, col); ramps up to
 * RAIN_STEADY_LEVEL over RAIN_EDGE_WIDTH as the front passes, and stays
 * there behind it -- a level rising/flooding downward, not a thin band
 * that leaves darkness behind it. */
static float rain_level(uint8_t row, uint8_t col, uint32_t elapsed_ms) {
    float local_elapsed = (float)elapsed_ms - rain_col_jitter_ms(col);
    if (local_elapsed < 0.0f) {
        local_elapsed = 0.0f;
    }
    float t = smoothstep01(local_elapsed / (float)RAIN_DURATION_MS);

    float front = RAIN_ORIGIN_ROW + t * (RAIN_TARGET_ROW - RAIN_ORIGIN_ROW + RAIN_EDGE_WIDTH);
    float edge = front - (float)row;
    return clamp01(edge / RAIN_EDGE_WIDTH) * RAIN_STEADY_LEVEL;
}

/* ---- Phase 2: fade phase 1's end state to complete dark ---- */

#define FADE_DURATION_MS 700u

static float fade_level(uint32_t elapsed_ms) {
    float t = smoothstep01((float)elapsed_ms / (float)FADE_DURATION_MS);
    return RAIN_STEADY_LEVEL * (1.0f - t);
}

/* ---- Phase 3: single, slow, elegant Sentia Instruments Magenta pulse ---- */

/* Pads + underglow only -- function-button LEDs are plain monochrome
 * PWM, not addressable RGB, so they can't show magenta; including them
 * (even just pulsing their own brightness) read as visually wrong once
 * actually seen on hardware. Slower and smoothstep-eased on both the
 * rise and fall (not linear) for the "elegant" feel asked for, instead
 * of the original quick, mechanical-feeling ramp. */
#define PULSE_RISE_MS 700u
#define PULSE_HOLD_MS 500u
#define PULSE_FALL_MS 1200u
#define PULSE_TOTAL_MS (PULSE_RISE_MS + PULSE_HOLD_MS + PULSE_FALL_MS)

#define MAGENTA_R 1.0f
#define MAGENTA_G 0.0f
#define MAGENTA_B 1.0f

static float pulse_envelope(uint32_t elapsed_ms) {
    if (elapsed_ms < PULSE_RISE_MS) {
        return smoothstep01((float)elapsed_ms / (float)PULSE_RISE_MS);
    }
    uint32_t after_rise = elapsed_ms - PULSE_RISE_MS;
    if (after_rise < PULSE_HOLD_MS) {
        return 1.0f;
    }
    uint32_t after_hold = after_rise - PULSE_HOLD_MS;
    if (after_hold < PULSE_FALL_MS) {
        return 1.0f - smoothstep01((float)after_hold / (float)PULSE_FALL_MS);
    }
    return 0.0f;
}

typedef float (*white_level_fn_t)(uint8_t row, uint8_t col, uint32_t elapsed_ms);

/* Function buttons (row 0, monochrome PWM) ARE part of the rain and fade
 * -- they're the flood's source row, so they light first and fade last
 * along with everything else. They're only excluded from phase 3's
 * magenta pulse, which is RGB-only and gets its own explicit button
 * black-out right before it runs (see run_phase3_magenta_pulse()). */
static void write_frame_white(white_level_fn_t level_fn, uint32_t elapsed_ms) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float button_v = level_fn(0u, col, elapsed_ms);
        tiles_buttons_set_standby_led(board_button_for_col(col), button_v);
    }
    for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            float v = level_fn(row, col, elapsed_ms);
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), v, v, v);
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        /* Underglow off through both phase 1 and phase 2 -- "without the
         * underglow" per the requested rain/flood, and there is nothing
         * meaningful for it to fade from if it was never lit. */
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static float phase1_level(uint8_t row, uint8_t col, uint32_t elapsed_ms) {
    return rain_level(row, col, elapsed_ms);
}

static float phase2_level(uint8_t row, uint8_t col, uint32_t elapsed_ms) {
    (void)row;
    (void)col;
    return fade_level(elapsed_ms);
}

static void run_phase1_rain(void) {
    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;
        bool done = elapsed >= RAIN_DURATION_MS;
        if (done) {
            elapsed = RAIN_DURATION_MS;
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
    /* Buttons are plain monochrome PWM, not addressable RGB, so they
     * can't show magenta -- explicitly black them out here (phase 2's
     * fade should already have brought them to 0, this just guarantees
     * it) and never touch them again for the rest of this phase. */
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_buttons_set_standby_led(board_button_for_col(col), 0.0f);
    }

    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - start_ms;
        bool done = elapsed >= PULSE_TOTAL_MS;
        if (done) {
            elapsed = PULSE_TOTAL_MS;
        }
        float env = pulse_envelope(elapsed);

        /* Buttons deliberately untouched here -- they're already at 0
         * from phase 2's fade, and stay there for the whole pulse. See
         * the file header on why they don't participate. */
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

    run_phase1_rain();
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
