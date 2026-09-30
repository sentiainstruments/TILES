#include "pedal.h"

#include "board_pins.h"
#include "midi_channels.h"
#include "midi_out.h"

#include "hardware/adc.h"
#include "pico/time.h"

#define ADC_MAX 4095u

/* Hysteresis band for the binary sustain decision. A footswitch swings
 * nearly rail to rail, so a wide band clear of both rails needs no tuning. */
#define SUSTAIN_PRESS_THRESHOLD (ADC_MAX / 4u)
#define SUSTAIN_RELEASE_THRESHOLD (ADC_MAX * 3u / 4u)

/* Asymmetric debounce (time the new state must hold before it is accepted).
 *   - PRESS 10 ms: sustain must engage at once, or a note released right
 *     after pressing slips through un-sustained.
 *   - RELEASE 80 ms: longer than the contact-chatter dropouts measured on
 *     a real rig (up to 55 ms mid-hold; each one released every sustained
 *     note), shorter than the quickest deliberate lift (~150 ms). A late
 *     release is inaudible; a spurious one cannot be undone. */
#define SUSTAIN_PRESS_DEBOUNCE_MS 10u
#define SUSTAIN_RELEASE_DEBOUNCE_MS 80u

#define MIDI_CC_SUSTAIN 64u
#define MIDI_CC_EXPRESSION 11u

static tiles_pedal_polarity_t s_polarity = TILES_PEDAL_DEFAULT_POLARITY;
static tiles_pedal_mode_t s_mode = TILES_PEDAL_DEFAULT_MODE;
static tiles_pedal_sustain_style_t s_sustain_style = TILES_PEDAL_DEFAULT_SUSTAIN_STYLE;

static uint16_t s_raw;
static bool s_raw_low;       /* last sample's side of the hysteresis band */
static bool s_debounced_low; /* s_raw_low after it held for the debounce time */
static uint32_t s_last_change_ms;
/* Pedal DOWN (debounced; read by harmonics and HOLD) vs. synth TOLD
 * (CC64 on, SYNTH style only). */
static bool s_sustain_pressed;
static bool s_cc64_on;
static uint8_t s_last_sent_expression_cc;

void tiles_pedal_init(void) {
    adc_init();
    adc_gpio_init(TILES_GPIO_PEDAL_ADC);
    adc_select_input(TILES_PEDAL_ADC_CHANNEL);

    s_raw = adc_read();
    s_raw_low = s_raw < SUSTAIN_PRESS_THRESHOLD;
    s_debounced_low = s_raw_low;
    s_last_change_ms = to_ms_since_boot(get_absolute_time());
    s_sustain_pressed = false;
    s_cc64_on = false;
    s_last_sent_expression_cc = 0xFFu; /* out of CC range: forces the first send */
}

static bool low_side_means_pressed(void) {
    return s_polarity == TILES_PEDAL_POLARITY_NORMALLY_OPEN;
}

/* Where pedal CCs go (strict standard MIDI):
 *   - the Zone Master Channel (ch 1). MPE (RP-053 2.3.1) puts pedals on the
 *     Master Channel only; with MPE off every note is on ch 1 anyway.
 *   - the fixed single-channel parts (chord, game, sequencer lanes), which
 *     sit outside the zone.
 * Never the Member/Song pool (ch 2-9) or ch 10. Do not broaden this to a
 * 16-channel broadcast: a host that only sustains on the note's own channel
 * is not set up for MPE and should use non-MPE mode. Background:
 * services/HISTORY.md. */
static void send_pedal_cc(uint8_t controller, uint8_t value) {
    static const uint8_t k_fixed_part_channels[] = {
        TILES_MIDI_CH_CHORD,      TILES_MIDI_CH_GAME,       TILES_MIDI_CH_SEQ_LANE_0,
        TILES_MIDI_CH_SEQ_LANE_1, TILES_MIDI_CH_SEQ_LANE_2, TILES_MIDI_CH_SEQ_LANE_3,
    };
    tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, controller, value);
    for (uint8_t i = 0u; i < sizeof(k_fixed_part_channels); i++) {
        tiles_midi_send_cc(k_fixed_part_channels[i], controller, value);
    }
}

/* Sustain: hysteresis on the raw reading, then debounce, then CC64. Shared
 * with tiles_pedal_set_mode() so both use the same tracker state. No
 * printf here: a blocked USB-CDC print once delayed CC64=0 enough to read
 * as a stuck pedal. */
static void scan_sustain(void) {
    bool raw_low = s_raw_low;
    if (s_raw_low && s_raw > SUSTAIN_RELEASE_THRESHOLD) {
        raw_low = false;
    } else if (!s_raw_low && s_raw < SUSTAIN_PRESS_THRESHOLD) {
        raw_low = true;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    /* The direction of the pending change picks the debounce time. Compared as
     * pressed/released, since polarity decides which raw side is which. */
    bool candidate_pressed = low_side_means_pressed() ? raw_low : !raw_low;
    uint32_t required_ms = candidate_pressed ? SUSTAIN_PRESS_DEBOUNCE_MS : SUSTAIN_RELEASE_DEBOUNCE_MS;
    if (raw_low != s_raw_low) {
        s_raw_low = raw_low;
        s_last_change_ms = now_ms;
    } else if (raw_low != s_debounced_low && (now_ms - s_last_change_ms) >= required_ms) {
        s_debounced_low = raw_low;
    }

    s_sustain_pressed = low_side_means_pressed() ? s_debounced_low : !s_debounced_low;
    /* HOLD style never sends CC64; expression.c holds the notes itself.
     * Reconciled every scan, so a style change with the pedal down applies at
     * once. */
    bool cc64_on = s_sustain_pressed && s_sustain_style == TILES_PEDAL_SUSTAIN_SYNTH;
    if (cc64_on != s_cc64_on) {
        s_cc64_on = cc64_on;
        /* Master Channel plus the fixed parts; see send_pedal_cc(). */
        send_pedal_cc(MIDI_CC_SUSTAIN, cc64_on ? 127u : 0u);
    }
}

/* Expression: standard TRS convention, heel (low reading) = 0, toe = 127,
 * linear. Not yet verified with a real expression pedal on this circuit. */
static void scan_expression(void) {
    uint8_t cc = (uint8_t)(((uint32_t)s_raw * 127u) / ADC_MAX);
    if (cc != s_last_sent_expression_cc) {
        s_last_sent_expression_cc = cc;
        send_pedal_cc(MIDI_CC_EXPRESSION, cc);
    }
}

void tiles_pedal_scan(void) {
    s_raw = adc_read();
    /* One physical signal, so only one mode is scanned: sustain's debounce on
     * a continuous sweep, or a footswitch through the expression mapping,
     * would each produce spurious output. */
    if (s_mode == TILES_PEDAL_MODE_SUSTAIN) {
        scan_sustain();
    } else {
        scan_expression();
    }
}

void tiles_pedal_set_polarity(tiles_pedal_polarity_t polarity) {
    s_polarity = polarity;
}

tiles_pedal_polarity_t tiles_pedal_get_polarity(void) {
    return s_polarity;
}

/* Winds down the mode being left, since the synth doesn't know the jack
 * changed function: sustain sends CC64=0 (else notes stay held forever),
 * expression resets CC11 to 127. Reseeds the sustain trackers from the
 * current reading so a later switch back doesn't compare against a stale
 * one. */
void tiles_pedal_set_mode(tiles_pedal_mode_t mode) {
    if (mode == s_mode) {
        return;
    }
    if (s_mode == TILES_PEDAL_MODE_SUSTAIN) {
        if (s_cc64_on) {
            s_cc64_on = false;
            send_pedal_cc(MIDI_CC_SUSTAIN, 0u);
        }
        s_sustain_pressed = false; /* also ends any HOLD */
    } else if (s_mode == TILES_PEDAL_MODE_EXPRESSION) {
        /* 127, the MIDI default for CC11 (full expression). Leaving the last value
         * would cap everything played afterwards. */
        send_pedal_cc(MIDI_CC_EXPRESSION, 127u);
    }
    s_last_sent_expression_cc = 0xFFu; /* forces a fresh send on re-entering expression mode */
    s_raw_low = s_raw < SUSTAIN_PRESS_THRESHOLD;
    s_debounced_low = s_raw_low;
    s_last_change_ms = to_ms_since_boot(get_absolute_time());
    s_mode = mode;
}

tiles_pedal_mode_t tiles_pedal_get_mode(void) {
    return s_mode;
}

void tiles_pedal_set_sustain_style(tiles_pedal_sustain_style_t style) {
    s_sustain_style = style;
}

tiles_pedal_sustain_style_t tiles_pedal_get_sustain_style(void) {
    return s_sustain_style;
}

bool tiles_pedal_is_sustained(void) {
    return s_mode == TILES_PEDAL_MODE_SUSTAIN && s_sustain_pressed;
}

bool tiles_pedal_is_holding_notes(void) {
    return tiles_pedal_is_sustained() && s_sustain_style == TILES_PEDAL_SUSTAIN_HOLD;
}

uint16_t tiles_pedal_get_raw(void) {
    return s_raw;
}
