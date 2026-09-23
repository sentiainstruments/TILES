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
static tiles_pedal_mode_t s_mode = TILES_PEDAL_DEFAULT_MODE;

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

/* Sustain-only: hysteresis + debounce + broadcast. Factored out of
 * tiles_pedal_scan() so tiles_pedal_set_mode() can't accidentally drift
 * out of sync with it -- both now read/write the exact same s_raw_low/
 * s_debounced_low/s_last_change_ms state through this one function.
 *
 * Real feedback: "theres a bug on the sustain pedal that makes it
 * stick even when released sometimes." Re-read this whole function
 * against the real hysteresis/debounce math and found nothing wrong on
 * paper -- s_raw_low tracks the current threshold-crossing state
 * immediately (with hysteresis), s_debounced_low only ever catches up
 * to it after SUSTAIN_DEBOUNCE_MS of s_raw_low staying put, a standard
 * pattern. Also confirmed tiles_pedal_scan() runs unconditionally every
 * single main-loop iteration (main.c), never skipped by any other
 * feature owning control the way some other scans can be -- and pedal.c
 * is the ONLY caller of the ADC in this entire firmware (grepped), so
 * there's no other module's adc_select_input() that could occasionally
 * leave this reading from the wrong channel either.
 *
 * A first round added printf() tracing at all three transitions here
 * (raw threshold crossing, debounce settling, the final CC send) to
 * gather real evidence instead of guessing. Real feedback narrowed the
 * repro precisely: "if i lift pedal before note [it's fine], then if i
 * lift pedal after note it sticks but if i play a new note it does
 * register as sustain released" -- release order-dependence, "fixed" by
 * unrelated later MIDI traffic, is the SAME signature this file's own
 * "Full device freeze during real Ableton MIDI clock playback" and "A
 * second real-hardware freeze" README entries already root-caused
 * TWICE before (services/haptics.c's per-kick printf(), then main.c's
 * periodic I2C scan dump): the Pico SDK's USB-CDC stdio driver
 * busy-waits the ENTIRE calling thread for up to
 * PICO_STDIO_USB_STDOUT_TIMEOUT_US (500ms, confirmed reading pico-sdk/
 * src/rp2_common/pico_stdio_usb/stdio_usb.c directly) every time its
 * output buffer fills faster than the host drains it -- the ordinary
 * case once TILES is plugged into a DAW/synth rig instead of a dev
 * machine with a serial terminal open. The "sustain -> %s" print sat
 * directly BEFORE tiles_midi_send_cc_broadcast() below: a blocked print
 * there delays the ACTUAL release message by up to half a second,
 * easily read as "stuck" by anyone not waiting that long, and exactly
 * explains "playing a new note releases it" -- the earlier, delayed
 * send had usually already gone out by the time a next note got played,
 * making the new note look like the cause rather than a coincidence of
 * timing. Fixed the same way both prior rounds were: deleted the
 * tracing outright now that it's served its purpose, rather than
 * throttle or reorder it -- the LOGIC comments explaining the hysteresis/
 * debounce math above are kept, only the print statements (and the
 * prose that existed solely to justify keeping them) came out. */
static void scan_sustain(void) {
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
        /* Broadcast, not a single channel -- under MPE (see
         * midi/midi_out.h) every currently-held note lives on its own
         * Member Channel, and sustain needs to hold ALL of them, not
         * just whichever channel happened to be "the" one before MPE
         * existed. */
        tiles_midi_send_cc_broadcast(MIDI_CC_SUSTAIN, pressed ? 127u : 0u);
    }
}

/* Expression-only: standard TRS expression-pedal convention, heel-down
 * (low raw reading) = 0, toe-down (high raw reading) = 127, linear
 * across the ADC's full range -- see this file's own header comment
 * for why this is "implemented to the standard," not "confirmed
 * against real hardware" yet. */
static void scan_expression(void) {
    uint8_t cc = (uint8_t)(((uint32_t)s_raw * 127u) / ADC_MAX);
    if (cc != s_last_sent_expression_cc) {
        s_last_sent_expression_cc = cc;
        tiles_midi_send_cc_broadcast(MIDI_CC_EXPRESSION, cc);
    }
}

void tiles_pedal_scan(void) {
    s_raw = adc_read();
    /* Real feedback: "enable those two as how they would work
     * standard" -- each mode implemented to its own real MIDI
     * standard, but never both from the same scan: this is one
     * physical signal, and running sustain's hysteresis/debounce
     * against an expression pedal's continuous sweep (or feeding a
     * footswitch's rail-to-rail swing through the expression mapping)
     * would each spuriously trigger the OTHER function's output --
     * see this file's own header comment for the full reasoning. */
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

/* Real feedback: "lets keep sustain pedal as the default but we can
 * edit this in control software later." Cleanly winds down whichever
 * mode is being LEFT, rather than just silently stopping its scan --
 * a synth doesn't know this jack changed function, so from ITS side,
 * sustain simply stops updating (if it was held down, it would stay
 * held down forever, no different from a stuck note) or expression
 * simply stops updating (stuck at whatever level the pedal was last
 * physically at, quietly capping how loud/expressive every future
 * note can sound for no reason the player can see). Reseeds the
 * sustain hysteresis trackers off the CURRENT raw reading either way
 * (not just when entering sustain mode) so a later switch back to
 * sustain doesn't compare against a reading that's now stale by
 * however long expression mode was active. */
void tiles_pedal_set_mode(tiles_pedal_mode_t mode) {
    if (mode == s_mode) {
        return;
    }
    if (s_mode == TILES_PEDAL_MODE_SUSTAIN && s_last_sent_sustained) {
        s_last_sent_sustained = false;
        tiles_midi_send_cc_broadcast(MIDI_CC_SUSTAIN, 0u);
    } else if (s_mode == TILES_PEDAL_MODE_EXPRESSION) {
        /* 127, not 0 -- the MIDI-spec default for CC11 (and what a
         * synth already assumes before ever receiving one) is FULL
         * expression, not silence. Leaving this jack's last value
         * behind on a mode switch would otherwise quietly cap
         * everything played afterward at whatever level the pedal
         * happened to be sitting at. */
        tiles_midi_send_cc_broadcast(MIDI_CC_EXPRESSION, 127u);
    }
    s_last_sent_expression_cc = 0xFFu; /* out of MIDI CC range -- forces a fresh send next time expression mode is entered */
    s_raw_low = s_raw < SUSTAIN_PRESS_THRESHOLD;
    s_debounced_low = s_raw_low;
    s_last_change_ms = to_ms_since_boot(get_absolute_time());
    s_mode = mode;
}

tiles_pedal_mode_t tiles_pedal_get_mode(void) {
    return s_mode;
}

bool tiles_pedal_is_sustained(void) {
    return s_mode == TILES_PEDAL_MODE_SUSTAIN && s_last_sent_sustained;
}

uint16_t tiles_pedal_get_raw(void) {
    return s_raw;
}
