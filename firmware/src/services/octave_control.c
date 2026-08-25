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
 * magnitude 1's flat solid and magnitude 3's faster triple-pulse below.
 * Never fully dark (min > 0) so it still reads clearly as "the active
 * direction," not as flickering off. Slowed from an initial 1600ms
 * period after real feedback that it read as too fast -- unmeasured,
 * still a starting guess at the new period. */
#define PULSE_PERIOD_MS 3200.0f
#define PULSE_MIN_LEVEL 0.15f
#define PULSE_MAX_LEVEL 1.0f

static float magnitude2_level(uint32_t now_ms) {
    float phase = (float)now_ms / PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * OCTAVE_CONTROL_PI * phase);
    return PULSE_MIN_LEVEL + (PULSE_MAX_LEVEL - PULSE_MIN_LEVEL) * raw;
}

/* Magnitude 3: three smooth pulses (a raised-cosine bump each -- rises
 * and falls smoothly, no instant on/off edge) in quick succession, then
 * a brief dark rest, then the whole burst repeats -- reworked from an
 * original hard on/off blink-then-solid-hold pattern after real
 * feedback that it read as "flashing too hard, not pulsing": the fix is
 * the shape of each transition (smooth raised-cosine instead of a
 * square wave), not just the timing. Still meant to read as busier/
 * faster than magnitude 2's single slow breath -- three quick pulses
 * per burst instead of one long one. Unmeasured -- a starting guess. */
#define PULSE3_ONE_MS 260.0f
#define PULSE3_COUNT 3u
#define PULSE3_REST_MS 700u
#define PULSE3_BURST_MS ((uint32_t)(PULSE3_COUNT * PULSE3_ONE_MS))
#define PULSE3_CYCLE_MS (PULSE3_BURST_MS + PULSE3_REST_MS)

static float magnitude3_level(uint32_t now_ms) {
    uint32_t t = now_ms % PULSE3_CYCLE_MS;
    if (t >= PULSE3_BURST_MS) {
        return 0.0f; /* the rest between bursts */
    }
    float within = (float)(t % (uint32_t)PULSE3_ONE_MS);
    float phase = within / PULSE3_ONE_MS; /* 0..1 across one pulse */
    /* Raised cosine: 0 at the edges, 1 at the pulse's midpoint, smooth
     * the whole way -- no hard edge anywhere in the waveform. */
    return 0.5f * (1.0f - cosf(2.0f * OCTAVE_CONTROL_PI * phase));
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
