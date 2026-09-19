#include "cv_gate.h"

#include "board_pins.h"
#include "dac80502.h"
#include "power.h"

#include "hardware/gpio.h"

/* Generous ceiling for "how many independent note-on events could be
 * simultaneously outstanding system-wide before any of them gets a
 * matching note-off" -- live touch (up to 24 pads) plus the regular
 * sequencer's 4 lanes plus Song mode's 9 concurrent slots plus chord
 * mode's own 4-voice-per-strike plus the hidden game melody voice is
 * already well under this; not a hard architectural limit, just a
 * fixed-size table sized comfortably above anything this instrument
 * can actually produce at once. */
#define CV_GATE_MAX_ACTIVE_NOTES 48u

typedef struct {
    bool active;
    uint8_t note;
    uint32_t claim_seq;
} cv_gate_active_note_t;

static cv_gate_active_note_t s_active_notes[CV_GATE_MAX_ACTIVE_NOTES];
static uint32_t s_next_claim_seq = 1u;
static bool s_gate_high;
static bool s_enabled;

static tiles_cv_pitch_calibration_t s_pitch_cal = {
    .volts_per_semitone = 1.0f / 12.0f, /* standard 1V/octave */
    .reference_note = 0,               /* MIDI note 0 = 0V, standard convention */
    .zero_trim_volts = 0.0f,           /* identity until measured -- see this file's own header comment */
    .gain_trim = 1.0f,
};

static tiles_cv_pressure_calibration_t s_pressure_cal = {
    .full_scale_volts = 10.0f, /* the jack's own nominal full-scale range at pressure=127 */
    .zero_trim_volts = 0.0f,
    .gain_trim = 1.0f,
};

/* OPA2990's fixed gain-of-4 stage between the DAC and the jack, per
 * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's own "CV" line -- a
 * hardware fact, not a calibration value, so it's a plain constant
 * rather than part of either calibration struct above. */
#define CV_OUTPUT_AMP_GAIN 4.0f

/* Real-world hardware fact used by both write_pitch_dac()/
 * write_pressure_dac() below: the DAC80502 accepts a 16-bit code across
 * its own internal 2.5V reference (with tiles_dac80502_init()'s own
 * GAIN-register write establishing that exact 0-2.5V span -- see that
 * driver's own comment). */
#define CV_DAC_FULL_SCALE_VOLTS 2.5f
#define CV_DAC_FULL_SCALE_CODE 65535.0f

static uint16_t volts_to_dac_code(float pre_amp_volts) {
    if (pre_amp_volts < 0.0f) {
        pre_amp_volts = 0.0f;
    }
    if (pre_amp_volts > CV_DAC_FULL_SCALE_VOLTS) {
        pre_amp_volts = CV_DAC_FULL_SCALE_VOLTS;
    }
    return (uint16_t)((pre_amp_volts / CV_DAC_FULL_SCALE_VOLTS) * CV_DAC_FULL_SCALE_CODE + 0.5f);
}

static void write_pitch_dac(uint8_t note) {
    float post_amp_volts = (float)((int16_t)note - s_pitch_cal.reference_note) * s_pitch_cal.volts_per_semitone;
    post_amp_volts = post_amp_volts * s_pitch_cal.gain_trim + s_pitch_cal.zero_trim_volts;
    tiles_dac80502_write(TILES_DAC80502_CHANNEL_A, volts_to_dac_code(post_amp_volts / CV_OUTPUT_AMP_GAIN));
}

static void write_pressure_dac(uint8_t pressure) {
    float post_amp_volts = ((float)pressure / 127.0f) * s_pressure_cal.full_scale_volts;
    post_amp_volts = post_amp_volts * s_pressure_cal.gain_trim + s_pressure_cal.zero_trim_volts;
    tiles_dac80502_write(TILES_DAC80502_CHANNEL_B, volts_to_dac_code(post_amp_volts / CV_OUTPUT_AMP_GAIN));
}

/* -1 if nothing is currently active. Ties (identical claim_seq) can't
 * happen -- s_next_claim_seq is strictly increasing and handed out once
 * per note-on. */
static int8_t find_priority_index(void) {
    int8_t best = -1;
    uint32_t best_seq = 0u;
    for (uint8_t i = 0; i < CV_GATE_MAX_ACTIVE_NOTES; i++) {
        if (s_active_notes[i].active && (best < 0 || s_active_notes[i].claim_seq > best_seq)) {
            best = (int8_t)i;
            best_seq = s_active_notes[i].claim_seq;
        }
    }
    return best;
}

bool tiles_cv_gate_is_active(void) {
    return s_enabled && tiles_power_get_state().cv_gate_permitted;
}

/* Shared by the power-permission callback and tiles_cv_gate_set_
 * enabled(false) below -- whichever gate (power or software) drops,
 * the safe response is identical: gate low immediately, forget every
 * currently-tracked note (so a note-off that arrives later, after
 * whatever caused this, can't act on stale state), and zero both DAC
 * channels back to their own init-time safe value. Doesn't literally
 * matter electrically once external power is actually gone (no amp
 * rail to output onto), but restores the exact same clean state either
 * way, and matters for real for the software-disable case, which can
 * happen with power still present. */
static void force_safe_off(void) {
    for (uint8_t i = 0; i < CV_GATE_MAX_ACTIVE_NOTES; i++) {
        s_active_notes[i].active = false;
    }
    if (s_gate_high) {
        gpio_put(TILES_GPIO_GATE_PWM, false);
        s_gate_high = false;
    }
    tiles_dac80502_write(TILES_DAC80502_CHANNEL_A, 0u);
    tiles_dac80502_write(TILES_DAC80502_CHANNEL_B, 0u);
}

static void on_power_state_changed(tiles_power_state_t new_state) {
    /* Real feedback (services/power.h's own design intent, exercised
     * here for the first time): CV/gate must tri-state the instant
     * external power disappears, not on this module's next poll --
     * that's the entire reason tiles_power_register_callback() exists. */
    if (!new_state.cv_gate_permitted && s_gate_high) {
        force_safe_off();
    }
}

void tiles_cv_gate_init(void) {
    for (uint8_t i = 0; i < CV_GATE_MAX_ACTIVE_NOTES; i++) {
        s_active_notes[i].active = false;
    }
    s_next_claim_seq = 1u;
    s_gate_high = false;
    s_enabled = false; /* real feedback: "keep sustain pedal as the default" -- same "fail off by default" rule applies here per docs/architecture/defaults-and-safeguards.md's own "CV range" section */
    tiles_dac80502_init();
    tiles_power_register_callback(on_power_state_changed);
}

void tiles_cv_gate_scan(void) {
    /* Deliberately empty -- see this file's own header comment. */
}

void tiles_cv_gate_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity; /* no velocity CV channel on this hardware -- VOUTB is pressure, not velocity; see this file's own header comment */
    if (!tiles_cv_gate_is_active()) {
        return;
    }
    for (uint8_t i = 0; i < CV_GATE_MAX_ACTIVE_NOTES; i++) {
        if (!s_active_notes[i].active) {
            s_active_notes[i].active = true;
            s_active_notes[i].note = note;
            s_active_notes[i].claim_seq = s_next_claim_seq++;
            break;
        }
        /* Table full (CV_GATE_MAX_ACTIVE_NOTES simultaneous outstanding
         * note-ons, see that constant's own comment on how generous a
         * ceiling that already is) -- silently drops this one from
         * priority tracking rather than overwriting an existing entry.
         * The corresponding note-off will simply find no matching
         * active entry and no-op, same as if this note-on had never
         * happened for CV/gate's purposes -- MIDI itself is
         * unaffected, only this instrument's own CV/gate mirror of it. */
    }
    /* Newest claim is always the highest claim_seq, so this note is
     * unconditionally the new priority note. */
    write_pitch_dac(note);
    if (!s_gate_high) {
        gpio_put(TILES_GPIO_GATE_PWM, true);
        s_gate_high = true;
    }
}

void tiles_cv_gate_note_off(uint8_t note) {
    if (!tiles_cv_gate_is_active()) {
        return;
    }
    for (uint8_t i = 0; i < CV_GATE_MAX_ACTIVE_NOTES; i++) {
        if (s_active_notes[i].active && s_active_notes[i].note == note) {
            s_active_notes[i].active = false;
            break; /* removes exactly one matching instance -- see this
                     * file's own header comment on why the SAME note
                     * value can have more than one independent hold
                     * outstanding at once (e.g. a live touch and a
                     * sequencer lane both playing the same note). */
        }
    }
    int8_t next = find_priority_index();
    if (next >= 0) {
        /* Real feedback: "as it would work standard" -- an immediate
         * legato jump to whichever note is now the priority, gate
         * staying high throughout since at least one note is still
         * held. No portamento/glide, no retrigger pulse -- the basic,
         * standard behavior, not the more elaborate optional variant
         * some interfaces also offer. */
        write_pitch_dac(s_active_notes[next].note);
    } else if (s_gate_high) {
        /* Nothing left held -- gate low. Pitch CV is deliberately left
         * exactly where it was (a real analog sequencer/interface
         * "samples and holds" the last pitch rather than snapping to
         * 0V on release) -- the receiving envelope/filter reads the
         * gate edge, not a pitch change, as "note off." */
        gpio_put(TILES_GPIO_GATE_PWM, false);
        s_gate_high = false;
    }
}

void tiles_cv_gate_channel_pressure(uint8_t note, uint8_t pressure) {
    if (!tiles_cv_gate_is_active()) {
        return;
    }
    int8_t priority = find_priority_index();
    if (priority >= 0 && s_active_notes[priority].note == note) {
        /* Only the currently CV-driving note's own pressure updates
         * the shared pressure channel -- same "one physical channel,
         * one owner at a time" rule this session's non-MPE pitch-bend
         * mode already established for its own shared MIDI channel,
         * applied here to a genuinely separate CV output instead. */
        write_pressure_dac(pressure);
    }
}

void tiles_cv_gate_set_enabled(bool enabled) {
    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    if (!enabled) {
        force_safe_off();
    }
}

bool tiles_cv_gate_is_enabled(void) {
    return s_enabled;
}

void tiles_cv_gate_set_pitch_calibration(tiles_cv_pitch_calibration_t calibration) {
    s_pitch_cal = calibration;
}

tiles_cv_pitch_calibration_t tiles_cv_gate_get_pitch_calibration(void) {
    return s_pitch_cal;
}

void tiles_cv_gate_set_pressure_calibration(tiles_cv_pressure_calibration_t calibration) {
    s_pressure_cal = calibration;
}

tiles_cv_pressure_calibration_t tiles_cv_gate_get_pressure_calibration(void) {
    return s_pressure_cal;
}
