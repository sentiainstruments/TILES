#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "cv_gate.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "mpe_alloc.h"
#include "haptics.h"
#include "expression_control.h"
#include "game_mode.h"
#include "octave_control.h"
#include "op_mode.h"
#include "light_effects.h"
#include "pedal.h"
#include "bend_core.h"
#include "diagnostics/capture.h"
#include "strike_settle.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* Melodic harmonics: hold one pad, lightly touch others to pluck its
 * harmonics (setting features.melodic_harmonics, default on). */
static bool s_harmonics_enabled = true;

/* Strike, velocity, key and touch timing: live values (defaults and why:
 * tune.h). */
#define TOUCH_DROPOUT_GRACE_MS (tiles_tune()->touch_dropout_ms)
#define PRESSED_DROPOUT_GRACE_MS (tiles_tune()->pressed_dropout_ms)
#define STRIKE_FROM_REST_MS (tiles_tune()->strike_from_rest_ms)
/* A touch that comes back within this gap gets no new touch tick (the
 * haptic pulse on touch-down): it's the same finger flickering. First
 * guess, under the lift of a fast repeated tap. */
#define TOUCH_PULSE_MIN_GAP_MS 60u
#define MIN_STRIKE_DEPTH_DELTA TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA
#define RETRIGGER_ARM_DEPTH_DELTA (tiles_tune()->retrigger_depth)
#define RETRIGGER_GRACE_MS (tiles_tune()->retrigger_grace_ms)

/* ---- Velocity: depth over a short follow-through, plus a little speed ----
 * Depth reached in the follow-through (strike_settle.h) separates a light
 * tap from a hard hit; time to the threshold adds a bias. A touch released
 * early commits at once. */
#define STRIKE_TIME_MAX_VELOCITY_MS (tiles_tune()->time_fast_ms)
#define STRIKE_TIME_MIN_VELOCITY_MS (tiles_tune()->time_slow_ms)
#define VELOCITY_CURVE_EXPONENT (tiles_tune()->velocity_curve)
#define STRIKE_DEPTH_OVERSHOOT_FULL_SCALE TILES_EXPRESSION_STRIKE_OVERSHOOT_FULL_SCALE
#define STRIKE_DEPTH_WEIGHT (tiles_tune()->depth_weight)
#define MIN_VELOCITY (tiles_tune()->min_velocity)

/* Depth for full pressure (127): unit 2's average strong strike (4 pads
 * sampled). Changed at runtime by the Feel menu's aftertouch row. */
static uint16_t s_depth_to_aftertouch_full_scale = 1450u;

#define AFTERTOUCH_SMOOTHING_ALPHA (tiles_tune()->pressure_smoothing)

/* ---- Release velocity from lift-off speed -------------------------------
 * The smoothed depth fall rate at release approximates lift speed with no
 * added latency (live: tune.h). */
#define RELEASE_FALL_RATE_SMOOTHING_ALPHA (tiles_tune()->release_smoothing)
#define RELEASE_VELOCITY_MIN_FALL_RATE (tiles_tune()->release_rate_min)
#define RELEASE_VELOCITY_MAX_FALL_RATE (tiles_tune()->release_rate_max)
#define RELEASE_VELOCITY_MIN 1u
#define RELEASE_VELOCITY_MAX 127u

/* A fall rate >= 0 means depth wasn't falling: return the floor. Otherwise
 * map the rate's magnitude linearly onto 1-127. */
static uint8_t release_velocity_from_fall_rate(float fall_rate) {
    if (fall_rate >= 0.0f) {
        return RELEASE_VELOCITY_MIN;
    }
    float magnitude = -fall_rate;
    float t = (magnitude - RELEASE_VELOCITY_MIN_FALL_RATE) / (RELEASE_VELOCITY_MAX_FALL_RATE - RELEASE_VELOCITY_MIN_FALL_RATE);
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return (uint8_t)((float)RELEASE_VELOCITY_MIN + t * (float)(RELEASE_VELOCITY_MAX - RELEASE_VELOCITY_MIN));
}

/* ---- Pitch bend from tilt: bend_core.h ------------------------------------ */
#define PITCH_BEND_CENTER TILES_BEND_CENTER

/* Cosine delta for full bend; smaller = more sensitive (Feel menu). 0.065
 * from captured comfortable tilts (median 0.053, max 0.085). Predates the
 * depth compensation: may need a fresh capture. */
static float s_pitch_bend_max_cosine_deviation = 0.065f;

/* tiles_bend_mode_t (bend_core.h), default PATH. */
static uint8_t s_pitch_bend_mode = TILES_BEND_MODE_PATH;

typedef enum {
    PAD_STATE_IDLE = 0,
    PAD_STATE_AWAITING_STRIKE,
    PAD_STATE_NOTE_ON,
    /* Channel stolen while the finger stayed down: silent until a real release
     * (re-awaiting a strike read the pressed key as a max-velocity hit). */
    PAD_STATE_STOLEN,
} pad_expr_state_t;

typedef struct {
    pad_expr_state_t state;
    uint32_t touch_start_ms;

    /* Touch start on the Hall sample clock (strike_time_ms is measured from
     * here). */
    uint32_t touch_start_sample_ms;

    /* Highest raw depth since touch began, first reading included: a hard hit
     * can be fully pressed before touch registers. */
    float peak_depth;

    /* Time from touch start to peak_depth first crossing
     * MIN_STRIKE_DEPTH_DELTA; ~0 if already past it at touch-down.
     * threshold_crossed makes it a one-time capture. */
    uint32_t strike_time_ms;
    bool threshold_crossed;
    /* The follow-through after the crossing (strike_settle.h). */
    tiles_strike_settle_t settle;

    uint32_t last_seen_sample_time_ms;

    /* When the note fired (for RETRIGGER_GRACE_MS). */
    uint32_t note_on_ms;

    /* EMA of depth for pressure (AFTERTOUCH_SMOOTHING_ALPHA), seeded at
     * note-on so pressure doesn't ramp up from 0. */
    float smoothed_depth;

    /* Smoothed depth rate while touched (negative = lifting): the release
     * velocity source. last_depth_sample_ms 0 = no sample yet. */
    float depth_fall_rate;
    float prev_raw_depth;
    uint32_t last_depth_sample_ms;

    /* Note cached at note-on and used for pressure and note-off, so a scale,
     * key or octave change mid-hold can't turn off a different note. */
    uint8_t active_note;
    uint8_t last_sent_aftertouch; /* 0xFF forces the first send */

    /* Touch dropout bridging (TOUCH_DROPOUT_GRACE_MS): updated whenever the
     * raw reading is touched. last_touched_valid guards the time before the
     * first touch. */
    uint32_t last_touched_ms;
    bool last_touched_valid;

    /* Last scan the key read up (< TILES_HALL_KEY_UP_DEPTH), for
     * STRIKE_FROM_REST_MS. */
    uint32_t last_key_up_ms;
    /* AWAITING_STRIKE that began with the key already pressed and no recent
     * rest: waiting for the key to come up before detecting a strike. */
    bool awaiting_rest;

    /* This note's channel: a Member Channel with MPE on, the Master Channel
     * (ch 1) with MPE off. Valid in PAD_STATE_NOTE_ON. */
    uint8_t midi_channel;
    /* Strike order (s_next_mpe_claim_seq), set on every note-on; used with
     * MPE off to find the most recently struck held pad
     * (s_non_mpe_owner_pad). */
    uint32_t touch_claim_seq;

    /* Per-note pitch bend. pitch_bend_active = whether bend was enabled when
     * THIS note fired (toggling mid-hold doesn't change it). */
    bool pitch_bend_active;
    tiles_bend_t bend;
    uint16_t pitch_bend_last_sent;
    /* The bend's centre for this note: 8192, or above it for a quarter tone
     * (bend_center_for()). */
    uint16_t bend_center;
    /* This pad's press path, learned from every strike's down-stroke and
     * kept across notes (PATH mode). */
    tiles_press_path_t path;

    /* HOLD sustain style: this pad's released note, kept on until the pedal
     * lifts (one per pad); held_* captured at release. */
    bool held;
    uint8_t held_channel;
    uint8_t held_note;
    uint8_t held_release_velocity;
} pad_expr_t;

static pad_expr_t s_pads[TILES_NUM_PADS];

/* The player's pitch bend on/off preference; each note latches it in
 * pitch_bend_active. */
static bool s_pitch_bend_enabled;


/* Member Channel table, one slot per pool channel; mpe_alloc.h picks. */
/* Sized for the zone's maximum (8, services/midi_channels.h). */
static tiles_mpe_slot_t s_mpe_channels[TILES_MIDI_SHARED_POOL_SIZE];
static uint32_t s_next_mpe_claim_seq = 1u;

/* ---- MPE on/off -------------------------------------------------------------
 * Default on. With MPE off, the most compatible plain-MIDI layout (see
 * tiles_expression_set_mpe_enabled()). */
static bool s_mpe_enabled = true;
/* Stamps each slot's release_seq as it's freed. */
static uint32_t s_next_mpe_release_seq = 1u;

/* The only place in_use changes, so midi_channels.h always mirrors it (Song
 * must never claim a live note's channel). */
static void set_channel_in_use(uint8_t index, bool in_use) {
    s_mpe_channels[index].in_use = in_use;
    uint8_t channel = (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + index);
    if (in_use) {
        tiles_midi_channels_note_channel_claimed(channel);
    } else {
        s_mpe_channels[index].release_seq = s_next_mpe_release_seq++;
        tiles_midi_channels_note_channel_released(channel);
    }
}

/* Claims slot `index` for `note` (owner_pad 0 = harmonic pluck). The one
 * place a slot is filled, so the same-note rule has the right note. */
static void claim_slot(uint8_t index, uint8_t owner_pad, uint8_t note) {
    set_channel_in_use(index, true);
    s_mpe_channels[index].owner_pad = owner_pad;
    s_mpe_channels[index].note = note;
    s_mpe_channels[index].claim_seq = s_next_mpe_claim_seq++;
}

/* True if the slot is inside the declared zone and Song isn't using it
 * (they can briefly differ; see services/midi_channels.h). */
static bool slot_in_live_zone(uint8_t index) {
    return index < tiles_midi_channels_declared_zone_size() &&
           !tiles_midi_channels_song_holds((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + index));
}

/* ---- pedal.sustain_style HOLD: TILES holds released notes itself ---------
 * So the pedal sustains real notes but not harmonics. Every end-of-note path
 * holds; a re-strike closes the pad's held note first. Off by default. */

/* Sends a held note's Note-Off and frees its channel. No-op if none. */
static void release_held_note(pad_expr_t *s) {
    if (!s->held) {
        return;
    }
    s->held = false;
    tiles_midi_note_off(s->held_channel, s->held_note, s->held_release_velocity);
    if (s->held_channel != TILES_MIDI_MPE_MASTER_CHANNEL) {
        uint8_t idx = (uint8_t)(s->held_channel - TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL);
        if (idx < TILES_MIDI_SHARED_POOL_SIZE) {
            set_channel_in_use(idx, false);
        }
    }
}

static void release_all_held_notes(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        release_held_note(&s_pads[i]);
    }
}

/* Edge of tiles_pedal_is_holding_notes(): release every held note once,
 * when holding stops. */
static bool s_prev_holding_notes;

/* Close any held note of this pitch before it sounds again: with MPE off a
 * second Note-On would leave one without a Note-Off; with MPE on it would
 * stack identical notes (MPE 3.2 warns against it). */
static void release_held_notes_of_pitch(uint8_t note) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].held && s_pads[i].held_note == note) {
            release_held_note(&s_pads[i]);
        }
    }
}

/* True if slot `idx` is a note held for the pedal (no finger on it). */
static bool slot_is_held_note(uint8_t idx) {
    uint8_t owner = s_mpe_channels[idx].owner_pad;
    if (owner == 0u || owner > TILES_NUM_PADS) {
        return false;
    }
    const pad_expr_t *o = &s_pads[owner - 1u];
    return o->held && o->held_channel == (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + idx);
}

/* ============================================================================
 * Melodic harmonics: with the pedal down and one note held, lightly touching
 * other pads plucks its harmonics 2-6 by touch order (not pad position), at
 * a fixed velocity, ringing a fixed time. A real strike becomes a note; two
 * held notes end the session. Melodic and Duet's melody pads only. */

/* Harmonics 2-6 in semitones above the fundamental (rounded to equal
 * temperament): octave, octave + fifth, 2 octaves, 2 octaves + major
 * third, 2 octaves + fifth. Index 0 = the first other pad touched. */
#define HARMONIC_MAX_VOICES 5u
static const uint8_t HARMONIC_SEMITONES[HARMONIC_MAX_VOICES] = {12u, 19u, 24u, 28u, 31u};

/* Gentle, fixed: an overtone under the real note. Unmeasured. */
#define HARMONIC_VELOCITY 40u

/* How long a pluck rings, touched or not: long enough to feel like a real
 * overtone, short enough that an accidental brush doesn't leave several
 * ringing. Unmeasured. */
#define HARMONIC_PLUCK_DURATION_MS 300u

/* Chord vs. harmonic: plucks start once the note has been held alone for
 * arm_ms; a new touch waits confirm_ms and is dropped if it presses past
 * press_depth. Defaults from a traced session (harmonic touches peak at
 * 47-144): arm 150 ms, confirm 40 ms, depth 128. Tune by ear. */
#define HARMONIC_ARM_MS_DEFAULT 150u
#define HARMONIC_CONFIRM_MS_DEFAULT 40u
#define HARMONIC_PRESS_DEPTH_DEFAULT 128u
static uint16_t s_harmonic_arm_ms = HARMONIC_ARM_MS_DEFAULT;
static uint16_t s_harmonic_confirm_ms = HARMONIC_CONFIRM_MS_DEFAULT;
static uint16_t s_harmonic_press_depth = HARMONIC_PRESS_DEPTH_DEFAULT;

typedef struct {
    bool active;
    uint8_t pad;          /* 1..TILES_NUM_PADS, valid while active */
    uint8_t note;
    uint8_t midi_channel;
    uint32_t ends_at_ms;  /* auto-decay deadline, valid while active */
} harmonic_voice_t;

static harmonic_voice_t s_harmonic_voices[HARMONIC_MAX_VOICES];
/* 0 = no session. Set when exactly one pad is held; cleared (all voices
 * torn down) when that changes. */
static uint8_t s_harmonic_fundamental_pad;
/* This section's own touch-edge memory, separate from the strike state
 * machine. */
static bool s_harmonic_prev_touched[TILES_NUM_PADS];
/* A touch waiting out the confirm time (see HARMONIC_ARM_MS_DEFAULT).
 * Cleared when the session ends. */
static bool s_harmonic_pending[TILES_NUM_PADS];
static uint32_t s_harmonic_pending_ms[TILES_NUM_PADS];

/* True for channels reserved for harmonics: up to HARMONIC_MAX_VOICES at
 * the top of the current zone, never all of it (zone_size - 1), so a real
 * note always finds a channel. claim_mpe_channel() skips them. */
static bool harmonic_channel_is_reserved(uint8_t channel) {
    /* Reserve only during a session (one note held): always reserving capped
     * polyphony at 3, and the 4th note stole and retriggered. */
    if (!s_harmonics_enabled || s_harmonic_fundamental_pad == 0u) {
        return false;
    }
    uint8_t zone_size = tiles_midi_channels_declared_zone_size();
    if (zone_size == 0u) {
        return false; /* nothing to reserve */
    }
    uint8_t reserved_count = (uint8_t)(zone_size - 1u); /* always leave at least one for a real note */
    if (reserved_count > HARMONIC_MAX_VOICES) {
        reserved_count = HARMONIC_MAX_VOICES;
    }
    return channel > (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + zone_size - 1u - reserved_count);
}

/* The one pad in PAD_STATE_NOTE_ON, or 0 if none or several. */
static uint8_t find_sole_held_pad(void) {
    uint8_t found = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].state == PAD_STATE_NOTE_ON) {
            if (found != 0u) {
                return 0u; /* second held pad: not sole */
            }
            found = (uint8_t)(i + 1u);
        }
    }
    return found;
}

/* Touched, with brief dropouts bridged (TOUCH_DROPOUT_GRACE_MS). Shared by
 * the scan loop and harmonics; the scan refreshes last_touched_ms for all
 * pads first. */
static bool pad_touch_bridged(const pad_expr_t *s, uint32_t now_ms) {
    return s->last_touched_valid && (now_ms - s->last_touched_ms) < TOUCH_DROPOUT_GRACE_MS;
}

static void end_harmonic_voice(uint8_t idx) {
    harmonic_voice_t *v = &s_harmonic_voices[idx];
    if (!v->active) {
        return;
    }
    tiles_midi_note_off(v->midi_channel, v->note, 0u); /* timed pluck ending: release velocity 0 */
    /* No CV/gate for harmonics: CV/gate is monophonic (last note priority),
     * and a harmonic would steal it from the fundamental. */
    /* Free the channel claim_harmonic_channel() took. (Forgetting this once
     * leaked a reserved channel per pluck until harmonics went silent.) */
    uint8_t mpe_index = (uint8_t)(v->midi_channel - TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL);
    if (mpe_index < TILES_MIDI_SHARED_POOL_SIZE) {
        set_channel_in_use(mpe_index, false);
    }
    v->active = false;
}

static void end_all_harmonic_voices(void) {
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        end_harmonic_voice(i);
    }
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_harmonic_pending[i] = false; /* a pending touch belongs to the ended session */
    }
    s_harmonic_fundamental_pad = 0u;
}

/* Only reserved channels, no stealing either way: returns 0xFF (no voice)
 * if all are busy. Within them, the MPE channel order (mpe_alloc.h): the
 * same note's old channel, else the longest idle. */
static uint8_t claim_harmonic_channel(uint8_t note) {
    /* Scans only the current zone, so a harmonic can never land on a fixed
     * part's channel (a sequencer lane's, say) and bend or press its notes. */
    bool eligible[TILES_MIDI_SHARED_POOL_SIZE];
    for (uint8_t i = 0; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        eligible[i] =
            slot_in_live_zone(i) && harmonic_channel_is_reserved((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i));
    }
    int idx = tiles_mpe_alloc_pick_free(s_mpe_channels, eligible, TILES_MIDI_SHARED_POOL_SIZE, note);
    if (idx < 0) {
        return 0xFFu;
    }
    /* owner_pad 0 = not a real pad, so the steal path never picks it. */
    claim_slot((uint8_t)idx, 0u, note);
    return (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + idx);
}

/* Plucks `pad` into harmonic slot `slot` (a new pad, or a re-pluck of a
 * ringing one): a re-pluck sends Note-Off first, never a second Note-On
 * for the same note on the same channel. */
static void fire_harmonic_pluck(uint8_t slot, uint8_t pad, uint8_t note, uint8_t channel, uint32_t now_ms) {
    harmonic_voice_t *v = &s_harmonic_voices[slot];
    if (v->active) {
        tiles_midi_note_off(v->midi_channel, v->note, 0u); /* re-pluck: release velocity 0 */
    }
    v->active = true;
    v->pad = pad;
    v->note = note;
    v->midi_channel = channel;
    v->ends_at_ms = now_ms + HARMONIC_PLUCK_DURATION_MS;
    /* Same rules as a played note: a held note of this pitch closes first, and
     * a Member Channel gets its per-note setup (not the shared channel with
     * MPE off, whose bend/pressure belong to the fundamental). */
    release_held_notes_of_pitch(note);
    if (channel != TILES_MIDI_MPE_MASTER_CHANNEL) {
        tiles_midi_send_note_setup(channel);
    }
    tiles_midi_note_on(channel, note, HARMONIC_VELOCITY);
}

/* True once this touch became a real press: a note, a threshold crossing,
 * or movement past the harmonic press depth. peak_depth/threshold_crossed
 * reset at every touch. */
static bool harmonic_pad_is_pressing(const pad_expr_t *ps) {
    return ps->state == PAD_STATE_NOTE_ON || ps->threshold_crossed || ps->peak_depth > (float)s_harmonic_press_depth;
}

/* Plucks `pad`'s harmonic over `fundamental_note`: re-pluck in its own
 * slot if ringing, else the next free slot (touch order) and a reserved
 * channel. Does nothing if all are busy or the note would pass 127. */
static void try_harmonic_pluck(uint8_t pad, uint8_t fundamental_note, uint32_t now_ms) {
    int8_t slot = -1;
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        if (s_harmonic_voices[i].active && s_harmonic_voices[i].pad == pad) {
            slot = (int8_t)i; /* already ringing on this pad: re-pluck in place */
            break;
        }
    }
    bool reuse = slot >= 0;
    if (!reuse) {
        for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
            if (!s_harmonic_voices[i].active) {
                slot = (int8_t)i;
                break;
            }
        }
        if (slot < 0) {
            return; /* every slot ringing on other pads */
        }
    }
    /* Range check BEFORE claiming a channel: checking after leaked a channel
     * for every skipped pluck above note 96. */
    int note = (int)fundamental_note + (int)HARMONIC_SEMITONES[slot];
    if (note > 127) {
        return; /* out of MIDI range: skip rather than play a wrong pitch */
    }
    uint8_t channel;
    if (reuse) {
        channel = s_harmonic_voices[slot].midi_channel; /* reuse the ringing voice's channel */
    } else if (!s_mpe_enabled) {
        /* MPE off: the shared channel 1, where a plain synth hears it (overtones
         * are different note numbers from the fundamental, so no clash). */
        channel = TILES_MIDI_MPE_MASTER_CHANNEL;
    } else {
        channel = claim_harmonic_channel((uint8_t)note);
        if (channel == 0xFFu) {
            return; /* every reserved channel busy */
        }
    }
    /* No printf here (blocking stdio before a MIDI send once made the sustain
     * pedal stick; see services/pedal.c). */
    fire_harmonic_pluck((uint8_t)slot, pad, (uint8_t)note, channel, now_ms);
}

/* Once per scan: off unless the pedal sustains and the mode allows; one
 * held pad is the fundamental; expired voices decay; new light touches
 * pluck once armed and confirmed. */
static void scan_melodic_harmonics(uint32_t now_ms) {
    if (!tiles_op_mode_melodic_harmonics_may_play() || !tiles_pedal_is_sustained()) {
        if (s_harmonic_fundamental_pad != 0u) {
            end_all_harmonic_voices();
        }
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            s_harmonic_prev_touched[i] = false;
            s_harmonic_pending[i] = false;
        }
        return;
    }

    uint8_t fundamental = find_sole_held_pad();
    if (fundamental != s_harmonic_fundamental_pad) {
        end_all_harmonic_voices();
        s_harmonic_fundamental_pad = fundamental;
    }

    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        harmonic_voice_t *v = &s_harmonic_voices[i];
        if (v->active && now_ms >= v->ends_at_ms) {
            end_harmonic_voice(i);
        }
    }

    if (fundamental == 0u) {
        /* No single fundamental: keep tracking touch edges (so a touch held
         * through the gap isn't a "new" touch later), but pluck nothing. */
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            s_harmonic_prev_touched[pad - 1u] = pad_touch_bridged(&s_pads[pad - 1u], now_ms);
        }
        return;
    }
    uint8_t fundamental_note = s_pads[fundamental - 1u].active_note;
    /* Armed once the fundamental NOTE has been held alone long enough. */
    bool armed = (now_ms - s_pads[fundamental - 1u].note_on_ms) >= s_harmonic_arm_ms;

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        /* Dropout-bridged touch, like the main loop. Raw edges let a flickering
         * resting finger re-pluck ghost notes, which the pedal then held. */
        bool touched = pad_touch_bridged(&s_pads[pad - 1u], now_ms);
        bool was_touched = s_harmonic_prev_touched[pad - 1u];
        s_harmonic_prev_touched[pad - 1u] = touched;
        pad_expr_t *ps = &s_pads[pad - 1u];

        if (pad == fundamental) {
            s_harmonic_pending[pad - 1u] = false;
            continue;
        }
        /* Only pads that play notes here pluck: not chord mode's chord strip
         * (op_mode plays it, and its presses aren't tracked here, so a chord
         * would read as a light touch) nor a pad under an open menu. */
        if (tiles_op_mode_owns_pad(pad)) {
            s_harmonic_pending[pad - 1u] = false;
            continue;
        }

        /* Pending touch: a press cancels it; staying light for the confirm time
         * (or lifting early) plucks. */
        if (s_harmonic_pending[pad - 1u]) {
            if (harmonic_pad_is_pressing(ps)) {
                s_harmonic_pending[pad - 1u] = false; /* a real press: never pluck under it */
            } else if (!touched || (now_ms - s_harmonic_pending_ms[pad - 1u]) >= s_harmonic_confirm_ms) {
                s_harmonic_pending[pad - 1u] = false;
                try_harmonic_pluck(pad, fundamental_note, now_ms);
            }
            continue;
        }

        if (!touched || was_touched) {
            continue; /* not a fresh touch */
        }
        if (ps->state == PAD_STATE_NOTE_ON) {
            continue; /* already a real note */
        }
        if (!armed) {
            continue; /* a chord still landing: consume this edge */
        }
        s_harmonic_pending[pad - 1u] = true;
        s_harmonic_pending_ms[pad - 1u] = now_ms;
    }
}

/* Turning harmonics off ends every ringing voice (with Note-Offs) and
 * clears touch memory, so turning back on doesn't treat resting fingers
 * as new touches. */
void tiles_expression_set_melodic_harmonics_enabled(bool enabled) {
    if (enabled == s_harmonics_enabled) {
        return;
    }
    if (!enabled) {
        end_all_harmonic_voices();
    }
    s_harmonics_enabled = enabled;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_harmonic_prev_touched[i] = false;
    }
}

bool tiles_expression_is_melodic_harmonics_enabled(void) {
    return s_harmonics_enabled;
}

/* Take effect next scan; a pending touch keeps its start time. */
void tiles_expression_set_harmonics_arm_ms(uint16_t ms) {
    s_harmonic_arm_ms = ms;
}
uint16_t tiles_expression_get_harmonics_arm_ms(void) {
    return s_harmonic_arm_ms;
}
void tiles_expression_set_harmonics_confirm_ms(uint16_t ms) {
    s_harmonic_confirm_ms = ms;
}
uint16_t tiles_expression_get_harmonics_confirm_ms(void) {
    return s_harmonic_confirm_ms;
}
void tiles_expression_set_harmonics_press_depth(uint16_t depth) {
    s_harmonic_press_depth = depth;
}
uint16_t tiles_expression_get_harmonics_press_depth(void) {
    return s_harmonic_press_depth;
}

/* MPE off: the held pad driving channel 1's bend and pressure (0 = none).
 * The newest strike takes over; on release the previous one takes back. */
static uint8_t s_non_mpe_owner_pad;

void tiles_expression_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pads[i] = (pad_expr_t){0};
        s_pads[i].state = PAD_STATE_IDLE;
        s_pads[i].bend_center = PITCH_BEND_CENTER;
    }
    for (uint8_t i = 0; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        if (s_mpe_channels[i].in_use) {
            tiles_midi_channels_note_channel_released((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i));
        }
        s_mpe_channels[i] = (tiles_mpe_slot_t){0};
    }
    s_next_mpe_claim_seq = 1u;
    s_next_mpe_release_seq = 1u;
    s_mpe_enabled = true;
    s_non_mpe_owner_pad = 0u;
    s_prev_holding_notes = false; /* held_* were zeroed above */
    s_pitch_bend_enabled = false;
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        s_harmonic_voices[i] = (harmonic_voice_t){0};
    }
    s_harmonic_fundamental_pad = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_harmonic_prev_touched[i] = false;
        s_harmonic_pending[i] = false;
    }
}

/* Max tilt = the declared range on the receiver's default 48-semitone
 * scale (bend_core.c). */
#define PITCH_BEND_WIRE_RANGE_COMPENSATION \
    ((float)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES / TILES_BEND_RECEIVER_DEFAULT_RANGE_SEMITONES)

/* Centers every pad that is bending (with MPE, several can be) and turns
 * their bend off. Used when pitch bend is switched off. */
static void center_and_deactivate_all_bending_pads(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        pad_expr_t *s = &s_pads[i];
        if (s->state == PAD_STATE_NOTE_ON && s->pitch_bend_active) {
            tiles_midi_send_pitch_bend(s->midi_channel, s->bend_center);
            s->pitch_bend_last_sent = s->bend_center;
            s->pitch_bend_active = false;
        }
    }
}

/* Square click toggles pitch bend. Turning it off recenters every
 * bending note at once. */
void tiles_expression_toggle_pitch_bend(void) {
    s_pitch_bend_enabled = !s_pitch_bend_enabled;
    printf("[expression] pitch bend %s\n", s_pitch_bend_enabled ? "enabled" : "disabled");
    if (!s_pitch_bend_enabled) {
        center_and_deactivate_all_bending_pads();
    }
}

bool tiles_expression_is_pitch_bend_enabled(void) {
    return s_pitch_bend_enabled;
}

float tiles_expression_get_pad_bend_amount(uint8_t logical_pad) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return 0.0f;
    }
    const pad_expr_t *s = &s_pads[logical_pad - 1u];
    if (s->state != PAD_STATE_NOTE_ON || !s->pitch_bend_active) {
        return 0.0f;
    }
    float amount = fabsf(((float)s->pitch_bend_last_sent - (float)s->bend_center) /
                         (8191.0f * PITCH_BEND_WIRE_RANGE_COMPENSATION));
    return amount > 1.0f ? 1.0f : amount;
}

void tiles_expression_set_pitch_bend_sensitivity(float max_cosine_deviation) {
    s_pitch_bend_max_cosine_deviation = max_cosine_deviation;
    printf("[expression] pitch bend sensitivity (max cosine deviation) now %.3f\n",
           (double)s_pitch_bend_max_cosine_deviation);
}

float tiles_expression_get_pitch_bend_sensitivity(void) {
    return s_pitch_bend_max_cosine_deviation;
}

void tiles_expression_set_pitch_bend_mode(uint8_t mode) {
    s_pitch_bend_mode = mode < TILES_BEND_MODE_COUNT ? mode : (uint8_t)TILES_BEND_MODE_PATH;
}

uint8_t tiles_expression_get_pitch_bend_mode(void) {
    return s_pitch_bend_mode;
}

bool tiles_expression_get_press_path(uint8_t logical_pad, float *bx, float *by, uint16_t *strokes) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return false;
    }
    const tiles_press_path_t *p = &s_pads[logical_pad - 1u].path;
    *bx = p->bx;
    *by = p->by;
    *strokes = p->strokes;
    return true;
}

void tiles_expression_reset_press_paths(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        tiles_press_path_reset(&s_pads[i].path);
    }
}

void tiles_expression_set_aftertouch_sensitivity(uint16_t depth_full_scale) {
    /* Never 0 (aftertouch_from_depth() divides by it). */
    s_depth_to_aftertouch_full_scale = depth_full_scale > 0u ? depth_full_scale : 1u;
    printf("[expression] aftertouch sensitivity (full-scale depth) now %u\n", s_depth_to_aftertouch_full_scale);
}

uint16_t tiles_expression_get_aftertouch_sensitivity(void) {
    return s_depth_to_aftertouch_full_scale;
}

/* Notes already held keep the channel they have until their Note-Off; only
 * new notes use the new routing (like pitch_bend_active).
 * s_non_mpe_owner_pad is left alone (unused while MPE is on). */
void tiles_expression_set_mpe_enabled(bool enabled) {
    /* Close notes held for the pedal (HOLD style) first: receivers stop
     * every note on a zone change anyway (MPE 2.1.4). */
    release_all_held_notes();
    s_mpe_enabled = enabled;
    printf("[expression] MPE mode %s\n", enabled ? "enabled" : "disabled (single-channel, standard MIDI)");
    /* Tell the receiver: MPE off withdraws the zone (MCM with 0 members), MPE
     * on declares its real size. Immediate here, unlike Song-driven size
     * changes (which wait for idle): this IS the player changing the zone. */
    tiles_expression_announce_mpe_zone();
}

bool tiles_expression_is_mpe_enabled(void) {
    return s_mpe_enabled;
}

/* The only place an MPE Configuration Message is sent from (see
 * expression.h), so the declaration always matches the setting (main.c
 * once announced a full zone on every mount even with MPE saved off). */
void tiles_expression_announce_mpe_zone(void) {
    tiles_midi_mpe_init(s_mpe_enabled ? tiles_midi_channels_declare_zone() : 0u);
}

static void begin_awaiting_strike(pad_expr_t *s, uint8_t pad, uint32_t now_ms) {
    s->state = PAD_STATE_AWAITING_STRIKE;
    s->touch_start_ms = now_ms;
    s->touch_start_sample_ms = now_ms;
    s->last_seen_sample_time_ms = 0;

    /* Check the first reading immediately: a hard hit can be fully pressed
     * before touch registers, and that's an instant, maximum-speed strike --
     * unless the key hasn't been at rest lately (STRIKE_FROM_REST_MS). */
    float initial_depth = (float)tiles_hall_get_depth(pad);
    tiles_press_path_stroke_begin(&s->path);
    tiles_capture_stroke_begun(pad);
    s->peak_depth = initial_depth;
    s->awaiting_rest = initial_depth >= MIN_STRIKE_DEPTH_DELTA && (now_ms - s->last_key_up_ms) > STRIKE_FROM_REST_MS;
    s->threshold_crossed = !s->awaiting_rest && initial_depth >= MIN_STRIKE_DEPTH_DELTA;
    s->strike_time_ms = 0;
    if (s->threshold_crossed) {
        tiles_strike_settle_begin(&s->settle, initial_depth, now_ms);
    }
}

/* Time score 0..1 (fast = 1), shaped by VELOCITY_CURVE_EXPONENT (1.0 =
 * linear). */
static float time_score_from_strike_time(uint32_t strike_time_ms) {
    if (strike_time_ms <= STRIKE_TIME_MAX_VELOCITY_MS) {
        return 1.0f;
    }
    if (strike_time_ms >= STRIKE_TIME_MIN_VELOCITY_MS) {
        return 0.0f;
    }
    float normalized = (float)(STRIKE_TIME_MIN_VELOCITY_MS - strike_time_ms) /
                        (float)(STRIKE_TIME_MIN_VELOCITY_MS - STRIKE_TIME_MAX_VELOCITY_MS);
    return powf(normalized, VELOCITY_CURVE_EXPONENT);
}

/* Depth score 0..1: how far peak_depth passed MIN_STRIKE_DEPTH_DELTA by
 * commit time (the follow-through). The dominant signal; see "Velocity". */
static float depth_score_from_peak(float peak_depth) {
    float overshoot = peak_depth - MIN_STRIKE_DEPTH_DELTA;
    if (overshoot < 0.0f) {
        overshoot = 0.0f;
    }
    float score = overshoot / STRIKE_DEPTH_OVERSHOOT_FULL_SCALE;
    if (score > 1.0f) {
        score = 1.0f;
    }
    return score;
}

/* Velocity = STRIKE_DEPTH_WEIGHT x depth score + the rest x time score,
 * floored at MIN_VELOCITY. See "Velocity". */
static uint8_t velocity_from_strike(uint32_t strike_time_ms, float peak_depth) {
    float time_score = time_score_from_strike_time(strike_time_ms);
    float depth_score = depth_score_from_peak(peak_depth);
    float combined = STRIKE_DEPTH_WEIGHT * depth_score + (1.0f - STRIKE_DEPTH_WEIGHT) * time_score;

    int vel = (int)((float)MIN_VELOCITY + (float)(127u - MIN_VELOCITY) * combined);
    if (vel < (int)MIN_VELOCITY) {
        vel = (int)MIN_VELOCITY;
    }
    if (vel > 127) {
        vel = 127;
    }
    return (uint8_t)vel;
}

/* Public wrapper for op_mode.c's chord pads (see expression.h). */
uint8_t tiles_expression_velocity_from_strike(uint32_t strike_time_ms, float peak_depth) {
    return velocity_from_strike(strike_time_ms, peak_depth);
}

static uint8_t aftertouch_from_depth(uint16_t depth) {
    uint32_t scaled = ((uint32_t)depth * 127u) / s_depth_to_aftertouch_full_scale;
    return (uint8_t)(scaled > 127u ? 127u : scaled);
}

/* Quarter tones (note_map.h): the pad's note sits `cents` up, so its bend
 * is centred there, in the wire's 48-semitone scale. MPE only: with MPE
 * off the bend is channel 1's, every note's. */
static uint16_t bend_center_for(uint8_t pad) {
    uint8_t cents = tiles_note_map_get_pad_cents(pad);
    if (cents == 0u || !s_mpe_enabled) {
        return PITCH_BEND_CENTER;
    }
    float wire = 8192.0f * ((float)cents / 100.0f) / TILES_BEND_RECEIVER_DEFAULT_RANGE_SEMITONES;
    return (uint16_t)((int32_t)PITCH_BEND_CENTER + (int32_t)(wire + 0.5f));
}

/* Starts pitch bend tracking for `pad`'s new note, latching whether bend
 * is enabled now. The baseline is taken later (bend_core.c). */
static void init_pitch_bend_for_pad(pad_expr_t *s, uint8_t pad, uint32_t now_ms) {
    s->pitch_bend_active = s_pitch_bend_enabled;
    if (!s->pitch_bend_active) {
        return;
    }
    tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
    /* A fresh depth read: s->smoothed_depth is still the previous note's. */
    tiles_bend_start(&s->bend, (tiles_bend_mode_t)s_pitch_bend_mode, &s->path, hs.x, hs.y, hs.z,
                     (float)tiles_hall_get_depth(pad), s->bend_center, now_ms);
    s->pitch_bend_last_sent = s->bend_center;
    tiles_capture_bend_started(pad, s->bend_center);
}

/* The most recently struck held pad other than `exclude_pad` (0 if none).
 * Used when the pad driving channel 1 (MPE off) releases. */
static uint8_t find_most_recent_held_pad(uint8_t exclude_pad) {
    uint8_t best_pad = 0u;
    uint32_t best_seq = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        if (pad == exclude_pad) {
            continue;
        }
        if (s_pads[i].state == PAD_STATE_NOTE_ON && (best_pad == 0u || s_pads[i].touch_claim_seq > best_seq)) {
            best_pad = pad;
            best_seq = s_pads[i].touch_claim_seq;
        }
    }
    return best_pad;
}

/* Ends `pad`'s note: pressure 0, Note-Off, haptic and CV/gate off, channel
 * freed; the single place this happens. Bend isn't recentred (that snapped
 * release tails). The Note-Off goes out even with the pedal down: sustain
 * is the synth's job (RP-053); withholding Note-Offs hung notes, don't
 * bring it back. player_release: the finger let go (release velocity).
 * may_hold: HOLD style may keep the note until release_held_note(). */
static void end_held_note(pad_expr_t *s, uint8_t pad, bool player_release, bool may_hold) {
    tiles_cv_gate_note_off(s->active_note);
    tiles_haptics_stop(pad);
    tiles_fx_release(pad, to_ms_since_boot(get_absolute_time()));
    s->pitch_bend_active = false;
    if (s->midi_channel != TILES_MIDI_MPE_MASTER_CHANNEL && s->last_sent_aftertouch != 0u) {
        /* MPE: pressure 0 right before Note-Off, even if the Note-Off waits for
         * the pedal. Not on channel 1 with MPE off (its pressure belongs to the
         * pad still being played). */
        tiles_midi_send_channel_pressure(s->midi_channel, 0u);
        s->last_sent_aftertouch = 0u;
    }
    uint8_t release_velocity = player_release ? release_velocity_from_fall_rate(s->depth_fall_rate) : 0u;
    tiles_capture_note_off(pad, release_velocity);
    bool hold = may_hold && tiles_pedal_is_holding_notes();
    if (hold) {
        s->held = true;
        s->held_channel = s->midi_channel;
        s->held_note = s->active_note;
        s->held_release_velocity = release_velocity;
    } else {
        tiles_midi_note_off(s->midi_channel, s->active_note, release_velocity);
    }
    if (s->midi_channel == TILES_MIDI_MPE_MASTER_CHANNEL) {
        /* Channel 1 has no pool slot. Hand its bend and pressure to the latest
         * other held pad and resend its bend. */
        if (s_non_mpe_owner_pad == pad) {
            uint8_t next_owner = find_most_recent_held_pad(pad);
            s_non_mpe_owner_pad = next_owner;
            if (next_owner != 0u) {
                pad_expr_t *next = &s_pads[next_owner - 1u];
                tiles_midi_send_pitch_bend(TILES_MIDI_MPE_MASTER_CHANNEL, next->pitch_bend_last_sent);
            }
        }
    } else if (!hold) {
        /* A held note keeps its channel until release_held_note(). */
        uint8_t idx = (uint8_t)(s->midi_channel - TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL);
        set_channel_in_use(idx, false);
    }
}

/* Game mode takes over: end every note, reset pads awaiting a strike, and
 * release pedal-held notes. */
void tiles_expression_force_release_all(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        pad_expr_t *s = &s_pads[i];
        if (s->state == PAD_STATE_NOTE_ON) {
            end_held_note(s, pad, false, false);
        }
        s->state = PAD_STATE_IDLE;
    }
    release_all_held_notes();
}

/* Member Channel for `pad`'s new `note`, in mpe_alloc.h's order; a stolen
 * channel goes to the new pad. Eligible: in the zone, not Song's, not
 * reserved for harmonics. */
static uint8_t claim_mpe_channel(uint8_t pad, uint8_t note) {
    bool eligible[TILES_MIDI_SHARED_POOL_SIZE];
    bool pedal_held[TILES_MIDI_SHARED_POOL_SIZE];
    for (uint8_t i = 0; i < TILES_MIDI_SHARED_POOL_SIZE; i++) {
        eligible[i] =
            slot_in_live_zone(i) && !harmonic_channel_is_reserved((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i));
        pedal_held[i] = slot_is_held_note(i);
    }

    int idx = tiles_mpe_alloc_pick_free(s_mpe_channels, eligible, TILES_MIDI_SHARED_POOL_SIZE, note);
    if (idx >= 0) {
        claim_slot((uint8_t)idx, pad, note);
        return (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + idx);
    }

    /* Never steals a harmonic pluck (owner_pad 0); a ringing harmonic can
     * fall outside the reserved range when the zone resizes, and would index
     * s_pads[-1]. */
    idx = tiles_mpe_alloc_pick_steal(s_mpe_channels, eligible, pedal_held, TILES_MIDI_SHARED_POOL_SIZE);
    if (idx < 0) {
        /* Nothing stealable (all reserved, or Song holds the zone): the first member
         * channel, which may collide with Song. Accepted. */
        return TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL;
    }
    uint8_t stolen_pad = s_mpe_channels[idx].owner_pad;
    /* No printf here: it would sit right before a Note-Off (blocking stdio),
     * and this path runs whenever play reaches the voice limit. */
    pad_expr_t *stolen = &s_pads[stolen_pad - 1u];
    if (pedal_held[idx]) {
        /* A pedal-held note (HOLD style): just close it; its pad has moved on. */
        release_held_note(stolen);
    } else {
        end_held_note(stolen, stolen_pad, false, false);
        /* STOLEN, not IDLE: a finger still on the pad would read as an instant
         * max-velocity strike on the next scan (a spurious note and haptic kick).
         * It stays silent until released and touched again. */
        stolen->state = PAD_STATE_STOLEN;
    }
    claim_slot((uint8_t)idx, pad, note);
    return (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + idx);
}

/* Silence needed on TILES's side before a Song-driven zone change is
 * declared (a re-declaration stops every note); covers release tails. */
#define MPE_ZONE_REDECLARE_IDLE_MS 2000u
static uint32_t s_mpe_zone_last_busy_ms;

static bool mpe_zone_is_idle(uint32_t now_ms) {
    bool busy = tiles_pedal_is_sustained();
    for (uint8_t i = 0; i < TILES_NUM_PADS && !busy; i++) {
        busy = s_pads[i].state == PAD_STATE_NOTE_ON || s_pads[i].held;
    }
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES && !busy; i++) {
        busy = s_harmonic_voices[i].active;
    }
    if (busy) {
        s_mpe_zone_last_busy_ms = now_ms;
        return false;
    }
    return (now_ms - s_mpe_zone_last_busy_ms) >= MPE_ZONE_REDECLARE_IDLE_MS;
}

void tiles_expression_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* HOLD style: when holding stops (pedal up, style changed to SYNTH, or
     * the jack switched to expression), send every held Note-Off. Before the
     * pad loop, so a freed channel is available to a strike this scan. */
    bool holding = tiles_pedal_is_holding_notes();
    if (s_prev_holding_notes && !holding) {
        release_all_held_notes();
    }
    s_prev_holding_notes = holding;

    /* Declare Song's zone change only once idle; meanwhile allocate within the
     * declared size minus Song's channels. */
    bool zone_idle = mpe_zone_is_idle(now_ms);
    if (s_mpe_enabled && zone_idle && tiles_midi_channels_zone_redeclare_pending()) {
        tiles_expression_announce_mpe_zone();
    }

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        pad_expr_t *s = &s_pads[i];

        bool raw_touched = tiles_touch_is_touched(pad);
        /* Untouched this long before this scan (UINT32_MAX: never touched). */
        uint32_t touch_gap_ms = s->last_touched_valid ? now_ms - s->last_touched_ms : UINT32_MAX;
        if (raw_touched) {
            s->last_touched_ms = now_ms;
            s->last_touched_valid = true;
        }
        /* Dropout-bridged touch (a raw touch always reads true here). */
        bool touched = pad_touch_bridged(s, now_ms);
        float depth_now = (float)tiles_hall_get_depth(pad);
        if (depth_now < (float)TILES_HALL_KEY_UP_DEPTH) {
            s->last_key_up_ms = now_ms;
        }
        if (tiles_capture_running() && pad == tiles_capture_pad()) {
            tiles_hall_sample_t cs = tiles_hall_get_sample(pad);
            tiles_capture_pass_begin(pad, now_ms, cs.x, cs.y, cs.z, cs.valid, raw_touched, (uint16_t)depth_now,
                                     (uint8_t)s->state);
        }

        if (s->state == PAD_STATE_STOLEN) {
            /* Stolen pad: silent until a real release, then IDLE. */
            if (!touched) {
                s->state = PAD_STATE_IDLE;
            }
            continue;
        }

        if (s->state == PAD_STATE_IDLE) {
            /* No new strike while something else owns the pad; a pad past IDLE
             * finishes. */
            if (touched && !tiles_expression_control_owns_pad_grid() && !tiles_octave_control_is_transpose_active() &&
                !tiles_op_mode_owns_pad(pad) && !tiles_game_mode_is_active() && tiles_note_map_pad_has_note(pad)) {
                begin_awaiting_strike(s, pad, now_ms);
                /* Touch-only haptic tick, whether or not it becomes a press; not for
                 * a finger that never left (see PRESSED_DROPOUT_GRACE_MS). */
                if (!s->awaiting_rest && touch_gap_ms >= TOUCH_PULSE_MIN_GAP_MS) {
                    tiles_haptics_trigger_touch_pulse(pad);
                }
            }
            continue;
        }

        if (s->state == PAD_STATE_AWAITING_STRIKE && s->awaiting_rest) {
            /* The key was already down when this touch began: silent until it
             * comes up, then a strike is detected from there. */
            if (!touched) {
                s->state = PAD_STATE_IDLE;
            } else if (depth_now < (float)TILES_HALL_KEY_UP_DEPTH) {
                begin_awaiting_strike(s, pad, now_ms);
            }
            continue;
        }

        if (s->state == PAD_STATE_AWAITING_STRIKE) {
            /* New Hall samples only while touched; after release the commit uses
             * what was measured. */
            if (touched) {
                tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
                if (hs.valid && hs.sample_time_ms != s->last_seen_sample_time_ms) {
                    s->last_seen_sample_time_ms = hs.sample_time_ms;
                    tiles_press_path_stroke_sample(&s->path, hs.x, hs.y, hs.z);
                    tiles_capture_stroke_sampled(pad);
                    float depth = (float)tiles_hall_get_depth(pad);
                    if (depth > s->peak_depth) {
                        s->peak_depth = depth;
                    }
                    if (!s->threshold_crossed && s->peak_depth >= MIN_STRIKE_DEPTH_DELTA) {
                        /* First sample past the threshold: record strike_time_ms. */
                        s->threshold_crossed = true;
                        s->strike_time_ms = hs.sample_time_ms - s->touch_start_sample_ms;
                        tiles_strike_settle_begin(&s->settle, s->peak_depth, hs.sample_time_ms);
                    } else if (s->threshold_crossed) {
                        tiles_strike_settle_sample(&s->settle, depth, hs.sample_time_ms);
                    }
                }
            }

            /* Uses the PEAK (threshold_crossed): a fast hit can spring back before
             * the current reading would show it. */
            bool pressed = s->threshold_crossed;

            /* ready: pressed and follow-through done. commit_on_release: touch ended
             * first, so commit with the depth reached. */
            bool ready = touched && pressed &&
                         tiles_strike_settle_ready(&s->settle, s->peak_depth, TILES_EXPRESSION_FULL_STRIKE_DEPTH, now_ms);
            bool commit_on_release = !touched && pressed;

            if (ready || commit_on_release) {
                s->active_note = tiles_note_map_get_note(pad);
                uint8_t velocity = velocity_from_strike(s->strike_time_ms, s->peak_depth);
                /* The down-stroke is over: learn it before this note's bend starts. */
                tiles_press_path_stroke_end(&s->path, &tiles_tune()->bend);
                tiles_capture_note_on(pad, velocity, s->strike_time_ms, s->peak_depth);
                /* Claim the note's channel, then send its per-note setup (bend center,
                 * pressure 0), so a Note-On never inherits the previous note's bend on
                 * that channel. */
                /* MPE off: every note on channel 1, no Member Channel. */
                /* HOLD style: close this pad's held note and any held note of the same
                 * pitch before the new Note-On (re-striking a held key), keeping one
                 * Note-Off per Note-On. No-op in SYNTH style. */
                /* One USB transfer for the Note-Offs, the setup and the Note-On
                 * (midi_out.h's batch): one by one, the Note-On left a pass behind. */
                tiles_midi_out_batch_begin();
                release_held_note(s);
                release_held_notes_of_pitch(s->active_note);
                s->midi_channel =
                    s_mpe_enabled ? claim_mpe_channel(pad, s->active_note) : TILES_MIDI_MPE_MASTER_CHANNEL;
                s->bend_center = bend_center_for(pad);
                init_pitch_bend_for_pad(s, pad, now_ms);
                s->touch_claim_seq = s_next_mpe_claim_seq++;
                if (!s_mpe_enabled) {
                    /* MPE off: the new strike takes over channel 1's bend/pressure; the setup
                     * below resets what the previous owner left. */
                    s_non_mpe_owner_pad = pad;
                }
                tiles_midi_send_note_setup(s->midi_channel);
                if (s->bend_center != PITCH_BEND_CENTER) {
                    tiles_midi_send_pitch_bend(s->midi_channel, s->bend_center); /* a quarter tone up */
                    s->pitch_bend_last_sent = s->bend_center;
                }
                tiles_midi_note_on(s->midi_channel, s->active_note, velocity);
                tiles_midi_out_batch_end();
                tiles_cv_gate_note_on(s->active_note, velocity);
                tiles_fx_hit(pad, velocity, now_ms); /* light effects */
                /* Same velocity for the haptic kick and the MIDI note. */
                tiles_haptics_trigger_kick(pad, velocity);
                s->last_sent_aftertouch = 0u; /* the setup just set it */
                /* Seed the pressure smoother with the real depth, not 0. */
                s->smoothed_depth = (float)tiles_hall_get_depth(pad);
                /* Seed the release-rate tracker so the first scan after note-on computes
                 * a real (zero) rate. */
                s->prev_raw_depth = s->smoothed_depth;
                s->last_depth_sample_ms = now_ms;
                s->depth_fall_rate = 0.0f;
                s->note_on_ms = now_ms;
                s->state = PAD_STATE_NOTE_ON;
                continue;
            }

            if (!touched) {
                /* Released without a real press: a light tap, no note. */
                s->state = PAD_STATE_IDLE;
            }
            continue;
        }

        /* PAD_STATE_NOTE_ON */
        float raw_depth = depth_now;
        /* A dropout while the key is still pressed keeps the note (see
         * PRESSED_DROPOUT_GRACE_MS). */
        bool key_still_down = raw_depth >= MIN_STRIKE_DEPTH_DELTA && touch_gap_ms < PRESSED_DROPOUT_GRACE_MS;
        if (!touched && !key_still_down) {
            /* Touch ended: the finger let go (the one player_release=true call). */
            end_held_note(s, pad, true, true);
            s->state = PAD_STATE_IDLE;
            continue;
        }

        /* Retrigger without lifting: depth back near rest (after the grace time)
         * ends the note and starts strike detection again; the next real press
         * fires a fresh note with its own velocity. */
        if ((now_ms - s->note_on_ms) >= RETRIGGER_GRACE_MS && raw_depth <= RETRIGGER_ARM_DEPTH_DELTA) {
            /* No release velocity (not a lift), but it may be held for the pedal. */
            end_held_note(s, pad, false, true);
            begin_awaiting_strike(s, pad, now_ms);
            continue;
        }

        /* Pressure smoothing toward this scan's depth. */
        s->smoothed_depth += AFTERTOUCH_SMOOTHING_ALPHA * (raw_depth - s->smoothed_depth);

        /* Release rate, updated every scan while touched (so its value at release
         * is the trend just before lifting). Skips a scan with dt 0. */
        uint32_t dt_ms = now_ms - s->last_depth_sample_ms;
        if (dt_ms > 0u) {
            float instant_rate = (raw_depth - s->prev_raw_depth) / (float)dt_ms;
            s->depth_fall_rate += RELEASE_FALL_RATE_SMOOTHING_ALPHA * (instant_rate - s->depth_fall_rate);
        }
        s->prev_raw_depth = raw_depth;
        s->last_depth_sample_ms = now_ms;

        uint8_t at = aftertouch_from_depth((uint16_t)s->smoothed_depth);
        tiles_capture_pressure(pad, s->smoothed_depth, at);
        if (at != s->last_sent_aftertouch) {
            s->last_sent_aftertouch = at;
            /* MPE off: only the owner pad sends pressure on the shared channel (else
             * pads would fight over it). No resync on owner change: a briefly stale
             * pressure is cosmetic, unlike a wrong pitch bend. */
            if (s_mpe_enabled || s_non_mpe_owner_pad == pad) {
                tiles_midi_send_channel_pressure(s->midi_channel, at);
            }
            tiles_cv_gate_channel_pressure(s->active_note, at);
            tiles_haptics_set_sustain_level(pad, at);
        }

        /* Pitch bend, per pad. With MPE off every pad still computes its bend
         * (keeping each pad's filters warm), but only the owner's is sent. */
        if (s->pitch_bend_active) {
            tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
            const tiles_bend_config_t bend_cfg = {s_pitch_bend_max_cosine_deviation,
                                                  (float)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES,
                                                  &tiles_tune()->bend};
            uint16_t bend = 0u;
            if (hs.valid) {
                bool produced =
                    tiles_bend_step(&s->bend, &bend_cfg, hs.x, hs.y, hs.z, s->smoothed_depth, now_ms, &bend);
                tiles_capture_bend_stepped(pad, produced, bend);
                if (produced && bend != s->pitch_bend_last_sent) {
                    /* This pad's current bend, for resyncing channel 1 (MPE off). */
                    s->pitch_bend_last_sent = bend;
                    if (s_mpe_enabled || s_non_mpe_owner_pad == pad) {
                        tiles_midi_send_pitch_bend(s->midi_channel, bend);
                    }
                }
            }
        }
    }
    tiles_capture_pass_end();
    if (s_harmonics_enabled) {
        scan_melodic_harmonics(now_ms);
    }
}
