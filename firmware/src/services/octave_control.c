#include "octave_control.h"

#include "buttons.h"
#include "note_map.h"

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
 * pulse evenly with each other. */
#define OCTAVE_PULSE_UNIT_MS 450.0f
#define OCTAVE_PULSE_REST_MS 650u
#define OCTAVE_PULSE_REST_LEVEL 0.15f
#define OCTAVE_PULSE_PEAK_LEVEL 1.0f

static float pulse_unit_level(float phase01) {
    float raw = 0.5f * (1.0f - cosf(2.0f * OCTAVE_CONTROL_PI * phase01));
    return OCTAVE_PULSE_REST_LEVEL + (OCTAVE_PULSE_PEAK_LEVEL - OCTAVE_PULSE_REST_LEVEL) * raw;
}

/* Magnitude 1: the same unit pulse repeating back to back forever --
 * "pulses even," no burst/rest structure at all, just a steady regular
 * breathing. Replaces the earlier flat-solid magnitude-1 look per real
 * feedback that one click should read as pulsing too, not just lit. */
static float magnitude1_level(uint32_t now_ms) {
    float phase = fmodf((float)now_ms / OCTAVE_PULSE_UNIT_MS, 1.0f);
    return pulse_unit_level(phase);
}

/* Magnitude 2 and 3: `magnitude` unit pulses back to back, then a dim
 * (not fully dark) rest, then the whole burst repeats. Magnitude 3 is
 * literally magnitude 2's shape plus one more pulse appended before the
 * same rest -- not a separately-tuned animation -- per real feedback
 * that the two should be "the same, just with an additional pulse
 * followed by a rest in dim." Unmeasured -- a starting guess at pacing. */
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

static bool s_prev_minus_pressed;
static bool s_prev_plus_pressed;

void tiles_octave_control_init(void) {
    s_prev_minus_pressed = false;
    s_prev_plus_pressed = false;
    tiles_buttons_set_override_active(BUTTON_ID_MINUS, true);
    tiles_buttons_set_override_active(BUTTON_ID_PLUS, true);
}

void tiles_octave_control_scan(void) {
    bool minus_pressed = tiles_button_is_pressed(BUTTON_ID_MINUS);
    bool plus_pressed = tiles_button_is_pressed(BUTTON_ID_PLUS);

    /* Rising edge only -- one shift per physical press, not continuous
     * while held. */
    if (minus_pressed && !s_prev_minus_pressed) {
        tiles_note_map_set_octave_shift((int8_t)(tiles_note_map_get_octave_shift() - 1));
    }
    if (plus_pressed && !s_prev_plus_pressed) {
        tiles_note_map_set_octave_shift((int8_t)(tiles_note_map_get_octave_shift() + 1));
    }
    s_prev_minus_pressed = minus_pressed;
    s_prev_plus_pressed = plus_pressed;

    int8_t shift = tiles_note_map_get_octave_shift();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

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
