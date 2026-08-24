#include "pedal.h"

#include "board_pins.h"
#include "midi_out.h"

#include "hardware/adc.h"
#include "pico/time.h"

#define ADC_MAX 4095u

/* Hysteresis band around midscale for the binary sustain decision. A
 * footswitch swings nearly rail-to-rail (open circuit near ADC_MAX vs a
 * hard short near 0 through the pedal's switch), so a wide band well
 * clear of both rails is robust without needing the fine tuning a
 * continuous signal would -- unlike expression, which does need real
 * hardware to calibrate, and stays disabled until that happens. */
#define SUSTAIN_PRESS_THRESHOLD (ADC_MAX / 4u)
#define SUSTAIN_RELEASE_THRESHOLD (ADC_MAX * 3u / 4u)

#define SUSTAIN_DEBOUNCE_MS 10u /* matches services/buttons.c's default */

#define MIDI_CC_SUSTAIN 64u
#define MIDI_CC_EXPRESSION 11u

static tiles_pedal_polarity_t s_polarity = TILES_PEDAL_DEFAULT_POLARITY;
static bool s_expression_enabled = TILES_PEDAL_DEFAULT_EXPRESSION_ENABLED;

static uint16_t s_raw;
static bool s_raw_low;       /* most recent sample's side of the hysteresis band */
static bool s_debounced_low; /* stable-for-N-ms version of the above */
static uint32_t s_last_change_ms;
static bool s_last_sent_sustained;
static uint8_t s_last_sent_expression_cc;

void tiles_pedal_init(void) {
    adc_init();
    adc_gpio_init(TILES_GPIO_PEDAL_ADC);
    adc_select_input(TILES_PEDAL_ADC_CHANNEL);

    s_raw = adc_read();
    s_raw_low = s_raw < SUSTAIN_PRESS_THRESHOLD;
    s_debounced_low = s_raw_low;
    s_last_change_ms = to_ms_since_boot(get_absolute_time());
    s_last_sent_sustained = false;
    s_last_sent_expression_cc = 0xFFu; /* out of MIDI CC range -- forces the first real send */
}

static bool low_side_means_pressed(void) {
    return s_polarity == TILES_PEDAL_POLARITY_NORMALLY_OPEN;
}

void tiles_pedal_scan(void) {
    s_raw = adc_read();

    bool raw_low = s_raw_low;
    if (s_raw_low && s_raw > SUSTAIN_RELEASE_THRESHOLD) {
        raw_low = false;
    } else if (!s_raw_low && s_raw < SUSTAIN_PRESS_THRESHOLD) {
        raw_low = true;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (raw_low != s_raw_low) {
        s_raw_low = raw_low;
        s_last_change_ms = now_ms;
    } else if (raw_low != s_debounced_low && (now_ms - s_last_change_ms) >= SUSTAIN_DEBOUNCE_MS) {
        s_debounced_low = raw_low;
    }

    bool pressed = low_side_means_pressed() ? s_debounced_low : !s_debounced_low;
    if (pressed != s_last_sent_sustained) {
        s_last_sent_sustained = pressed;
        tiles_midi_send_cc(MIDI_CC_SUSTAIN, pressed ? 127u : 0u);
    }

    if (s_expression_enabled) {
        uint8_t cc = (uint8_t)(((uint32_t)s_raw * 127u) / ADC_MAX);
        if (cc != s_last_sent_expression_cc) {
            s_last_sent_expression_cc = cc;
            tiles_midi_send_cc(MIDI_CC_EXPRESSION, cc);
        }
    }
}

void tiles_pedal_set_polarity(tiles_pedal_polarity_t polarity) {
    s_polarity = polarity;
}

tiles_pedal_polarity_t tiles_pedal_get_polarity(void) {
    return s_polarity;
}

void tiles_pedal_set_expression_enabled(bool enabled) {
    s_expression_enabled = enabled;
    if (!enabled) {
        /* Leave the pedal in a known state if expression gets disabled
         * mid-hold -- next enable will re-send whatever the pedal is
         * actually at (forced by resetting the "last sent" tracker),
         * rather than silently sitting on a stale value. */
        s_last_sent_expression_cc = 0xFFu;
    }
}

bool tiles_pedal_get_expression_enabled(void) {
    return s_expression_enabled;
}

bool tiles_pedal_is_sustained(void) {
    return s_last_sent_sustained;
}

uint16_t tiles_pedal_get_raw(void) {
    return s_raw;
}
