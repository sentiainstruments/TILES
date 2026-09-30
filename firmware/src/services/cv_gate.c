#include "cv_gate.h"

#include "board_pins.h"
#include "dac80502.h"
#include "power.h"

#include "hardware/gpio.h"

/* Room for every note that could be outstanding at once (24 pads, 4
 * lanes, 8 Song tracks, 4-voice chords, the game melody), with margin. */
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
    .volts_per_semitone = 1.0f / 12.0f, /* 1 V/octave */
    .reference_note = 0,               /* MIDI note 0 = 0 V */
    .zero_trim_volts = 0.0f,           /* identity until measured */
    .gain_trim = 1.0f,
};

static tiles_cv_pressure_calibration_t s_pressure_cal = {
    .full_scale_volts = 10.0f, /* the jack's nominal full range at pressure 127 */
    .zero_trim_volts = 0.0f,
    .gain_trim = 1.0f,
};

/* The OPA2990 x4 stage between DAC and jack (handoff "CV" line): hardware,
 * not calibration. */
#define CV_OUTPUT_AMP_GAIN 4.0f

/* DAC80502 16-bit code across its internal 2.5 V reference (the span
 * tiles_dac80502_init() sets up). */
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

/* -1 if nothing is active. No ties: claim_seq strictly increases. */
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

/* Used when either gate (power or software) drops: gate low, forget every
 * tracked note (so a late note-off can't act on stale state), both DACs
 * to zero. Matters mainly for the software-disable case, which can happen
 * with power still present. */
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
    /* Drop immediately when external power goes, not on the next poll. */
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
    s_enabled = false; /* off at boot; see header */
    tiles_dac80502_init();
    tiles_power_register_callback(on_power_state_changed);
}

void tiles_cv_gate_scan(void) {
    /* Intentionally empty; see the header. */
}

void tiles_cv_gate_note_on(uint8_t note, uint8_t velocity) {
    (void)velocity; /* no velocity CV on this hardware (VOUTB is pressure) */
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
        /* Table full: this note isn't tracked for CV (its note-off will find
         * nothing and no-op). MIDI is unaffected. */
    }
    /* Newest note has the highest claim_seq, so it is the priority note. */
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
            break; /* removes one instance: the same note can be held twice (e.g. a pad and a lane) */
        }
    }
    int8_t next = find_priority_index();
    if (next >= 0) {
        /* Legato jump to the new priority note; the gate stays high. No glide or
         * retrigger. */
        write_pitch_dac(s_active_notes[next].note);
    } else if (s_gate_high) {
        /* Nothing held: gate low. Pitch CV holds its last value (sample and
         * hold), since the receiver reads note-off from the gate. */
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
        /* Only the note driving the CV updates the pressure channel. */
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
