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

/* Magnitude 2: a smooth, slow breathing pulse -- distinct from
 * magnitude 1's flat solid and magnitude 3's crisp blink pattern below.
 * Never fully dark (min > 0) so it still reads clearly as "the active
 * direction," not as flickering off. Unmeasured -- a starting guess. */
#define PULSE_PERIOD_MS 1600.0f
#define PULSE_MIN_LEVEL 0.15f
#define PULSE_MAX_LEVEL 1.0f

static float magnitude2_level(uint32_t now_ms) {
    float phase = (float)now_ms / PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * OCTAVE_CONTROL_PI * phase);
    return PULSE_MIN_LEVEL + (PULSE_MAX_LEVEL - PULSE_MIN_LEVEL) * raw;
}

/* Magnitude 3: three quick blinks, then a solid hold, then the whole
 * cycle repeats -- deliberately busier/faster-reading than magnitude
 * 2's plain breathing pulse. Unmeasured -- a starting guess. */
#define BLINK_ON_MS 150u
#define BLINK_OFF_MS 150u
#define BLINK_COUNT 3u
#define HOLD_MS 800u
#define BLINK_PHASE_MS (BLINK_COUNT * (BLINK_ON_MS + BLINK_OFF_MS))
#define CYCLE_MS (BLINK_PHASE_MS + HOLD_MS)

static float magnitude3_level(uint32_t now_ms) {
    uint32_t t = now_ms % CYCLE_MS;
    if (t < BLINK_PHASE_MS) {
        uint32_t within_blink = t % (BLINK_ON_MS + BLINK_OFF_MS);
        return (within_blink < BLINK_ON_MS) ? 1.0f : 0.0f;
    }
    return 1.0f; /* the hold */
}

static float level_for_magnitude(uint8_t magnitude, uint32_t now_ms) {
    switch (magnitude) {
    case 1u:
        return 1.0f; /* solid */
    case 2u:
        return magnitude2_level(now_ms);
    case 3u:
        return magnitude3_level(now_ms);
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
