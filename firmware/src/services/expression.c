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
#include "pedal.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* Melodic harmonics (see "Melodic harmonics" below): hold ONE pad with a
 * real press, then lightly touch others to pluck its harmonics. Setting
 * `features.melodic_harmonics`, default on. */
static bool s_harmonics_enabled = true;

/* ============================================================================
 * Strike detection: a note needs real depth travel
 * (MIN_STRIKE_DEPTH_DELTA), not just touch. Velocity comes mainly from how
 * deep the strike goes (see "Velocity").
 *
 * From a capture of ~140 real touches: Hall depth moves in 16-count steps;
 * bare contact and settling read 32 (the most common value) up to ~96,
 * while deliberate presses reached 192-736 (full press ~900-1450).
 * ========================================================================== */

/* Treat touch as still down for this long after the last raw "touched"
 * reading. A hard, percussive hit can bounce the finger off the surface for
 * a few ms; read as a release, that cancelled the strike just before its
 * peak and the note never fired. Short, so real releases stay snappy
 * (the MPR121 release threshold was tuned for that). Unmeasured. */
#define TOUCH_DROPOUT_GRACE_MS 12u

/* Depth travel since touch-down that makes it a press (and the point where
 * velocity measurement starts). A touch that never gets here plays
 * nothing, like a key that's touched but not pressed. 150 sits in the gap
 * the capture found (contact <= ~96, presses >= ~192). 300 was tried to
 * stop fast shallow flicks reading hard, but made light, slow playing
 * impossible; velocity now handles the flick case (see "Velocity"). */
#define MIN_STRIKE_DEPTH_DELTA TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA

/* Retrigger without lifting: while a note is held, depth easing back to
 * near rest (<= this) arms a new strike. Kept close to rest so ordinary
 * pressure changes during a held note don't retrigger; a slow deliberate
 * fade-out can still dip below it. Unmeasured. */
#define RETRIGGER_ARM_DEPTH_DELTA 40.0f

/* Ignore retrigger for this long after a note fires, so the post-impact
 * rebound of a fast hit doesn't cause an instant note-off + retrigger.
 * Unmeasured. */
#define RETRIGGER_GRACE_MS 50u

/* ---- Velocity: depth over a short follow-through, plus a little speed ----
 * Not acceleration: differencing 3 coarse (16-count) depth samples was
 * noise, so hard hits often failed and light presses read randomly hard.
 *
 * Two measurements:
 *   - strike_time_ms: time from touch-down to crossing
 *     MIN_STRIKE_DEPTH_DELTA (like a keybed's two-contact timing; ~0 for a
 *     very fast hit).
 *   - peak_depth: how far the strike keeps going during
 *     VELOCITY_FOLLOWTHROUGH_MS after the crossing.
 * Time alone can't tell a quick light tap from a quick hard hit (both
 * cross the threshold at once), and depth read at the instant of crossing
 * can be faked by a fast graze. Over the short follow-through, a light
 * touch (fast or slow) plateaus near the threshold while a real hit keeps
 * going, much like a piano hammer needs real force to travel fast. So
 * depth dominates (STRIKE_DEPTH_WEIGHT 0.85) with a small speed bias
 * (0.15). A touch released before the follow-through ends commits at once
 * with the depth it reached, so short taps add no latency.
 *
 * Time mapping: <= STRIKE_TIME_MAX_VELOCITY_MS = fastest, >=
 * STRIKE_TIME_MIN_VELOCITY_MS = slowest, linear in between
 * (VELOCITY_CURVE_EXPONENT 1.0; a steeper 1.8 compressed soft playing).
 * The slow end is 300 ms so ordinary taps don't crowd the top. These
 * constants are guesses; the `[expression]` print logs strike_time_ms and
 * depth for calibration. */
#define STRIKE_TIME_MAX_VELOCITY_MS 10u
#define STRIKE_TIME_MIN_VELOCITY_MS 300u
#define VELOCITY_CURVE_EXPONENT 1.0f

/* Follow-through window after crossing the threshold before committing:
 * long enough for a light touch to plateau, short enough to stay under
 * perceptible onset latency (~10-20 ms). First guess. */
#define VELOCITY_FOLLOWTHROUGH_MS 20u

/* Overshoot past MIN_STRIKE_DEPTH_DELTA (by the end of the follow-through)
 * at which the depth signal maxes out. Unmeasured. */
#define STRIKE_DEPTH_OVERSHOOT_FULL_SCALE 550.0f

/* Depth's share of the velocity blend (see "Velocity"). */
#define STRIKE_DEPTH_WEIGHT 0.85f

/* Floor, so even the weakest qualifying strike is audible. */
#define MIN_VELOCITY 8u

/* Depth that maps to full pressure (127). 1450 = unit 2's average strong
 * strike (corner pads 1697, 1488, 1328, 1280). An earlier unit's full press
 * bottomed out around 918 (784-1184); using 900 against unit 2 maxed
 * pressure too early and felt twitchy. One value for all pads (no per-pad
 * curve yet); only 4 pads of one unit were sampled.
 * Runtime: the expression menu's aftertouch row changes it
 * (tiles_expression_set_aftertouch_sensitivity()). */
static uint16_t s_depth_to_aftertouch_full_scale = 1450u;

/* Smoothing (EMA) on the depth that feeds pressure, so it reads as
 * continuous pressure, not sensor noise, without noticeable lag. Not
 * applied to velocity, which needs the raw transient. First guess. */
#define AFTERTOUCH_SMOOTHING_ALPHA 0.35f

/* ---- Release velocity from lift-off speed -------------------------------
 * MPE treats release velocity as a per-note dimension; TILES used to send
 * 0 ("none"). Release is detected when touch ends, and depth isn't read
 * after that, so a true post-release measurement would delay every
 * Note-Off. Instead depth_fall_rate (depth units/ms, smoothed) is tracked
 * while the finger is still down, and its value at release approximates
 * how fast the finger was lifting: real data, zero added latency. Only
 * end_held_note() sends a non-zero value. The rate range is a first guess
 * from this file's depth scale, not measured lifts. */
#define RELEASE_FALL_RATE_SMOOTHING_ALPHA 0.5f
#define RELEASE_VELOCITY_MIN_FALL_RATE 2.0f  /* depth units/ms; at or below: minimum release velocity */
#define RELEASE_VELOCITY_MAX_FALL_RATE 25.0f /* depth units/ms; at or above: maximum release velocity */
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

/* ---- Pitch bend from tilt ------------------------------------------------
 * Bend is only computed while a note is held (strike detection never uses
 * X/Y). Pitch bend on/off is a global preference (square click).
 *
 * The math: raw X changes with press depth even with no tilt (the field
 * gets stronger closer to the magnet), so pressing harder would bend.
 * Instead use the field's DIRECTION: a direction cosine (component / |B|,
 * |B| = sqrt(x^2+y^2+z^2)) depends on the angle, not the distance, so it
 * cancels depth to first order (the same idea 3-axis Hall joysticks use).
 * In practice that isn't perfect; see PITCH_BEND_SETTLE_MS and the depth
 * compensation below.
 *
 * Both X and Y feed the MAGNITUDE (sqrt(dx^2 + dy^2)): a real tilt moves
 * both, and small vibrato wiggles need the stronger signal. The SIGN comes
 * from X alone, keeping the tuned left/right direction. (Not a true 2D
 * bend.)
 *
 * Bend is per note: with MPE each held note has its own channel
 * (claim_mpe_channel()), so pads bend independently like a Seaboard. A
 * channel is recentered by tiles_midi_send_note_setup() before its next
 * Note-On, not at Note-Off (which snapped release tails). */
#define PITCH_BEND_CENTER 8192u

/* Cosine delta that maps to full bend (+/-8191). SMALLER = more sensitive.
 * Runtime: the expression menu's pitch bend row
 * (tiles_expression_set_pitch_bend_sensitivity()).
 *
 * 0.065 comes from two captures: a deliberate, comfortable tilt on a held
 * pad gave deltas min 0.0323, median 0.0526, p90 0.0626, max 0.0851. The
 * old 0.15 was beyond anything a real tilt produces ("requires extreme
 * bend"); 0.065 puts most real tilts at half to full swing, above the
 * 0.04 deadzone. (Earlier values 0.15/0.30/0.20 were guesses stacked on
 * compensating logic that has since been simplified.)
 * Note: the other bend constants were tuned before the depth compensation
 * below was added and may need a fresh capture. */
static float s_pitch_bend_max_cosine_deviation = 0.065f;

/* Deltas below this read as centered, applied as a soft knee (subtracted
 * before scaling), so bend ramps from zero instead of jumping.
 *
 * A capture of a straight press with no tilt (1070 samples: median 0.0257,
 * p90 0.0373, p99 0.051, max 0.0945) showed pressing straight down really
 * moves the cosine on this assembly. Compared with the deliberate-tilt
 * capture, 0.04 rejected 94% of rest noise and lost only 0.6% of real
 * tilt (0.045 lost 5.6%). Both captures predate the depth-rate-gated
 * recentering (PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW), which removes most
 * of that pressure noise at the source, so it was lowered to 0.025 (and
 * felt better). Changed alone, so a fresh capture can tell what to revert
 * if plain presses start bending again. */
#define PITCH_BEND_DEADZONE_COSINE_DELTA 0.025f

/* Wait this long after the note starts before capturing the bend
 * baseline, letting the two-stage smoothing settle; bend stays centered
 * meanwhile. A baseline taken from the noisy strike instant is wrong for
 * the whole note, and no downstream filtering can fix a wrong reference.
 * 25 ms was far too short for two filter stages (the settling transient
 * itself read as bend on nearly every note); 250 ms made bends start too
 * late; 120 ms is the middle ground. Unmeasured. */
#define PITCH_BEND_SETTLE_MS 120u

/* Slow re-centering of the baseline toward the (smoothed) current X/Y,
 * only while no bend run is active (so a held tilt doesn't fade). Handles
 * a steady one-sided offset over a long straight hold, which a fixed
 * baseline can't tell from a held tilt. The target is the smoothed X/Y,
 * not raw: chasing raw samples erased a real but small (~13-15 unit)
 * lean hidden under hand tremor.
 *
 * The rate adapts to depth movement: pressure only couples into tilt
 * WHILE depth is changing, so the baseline snaps fast (..._FAST) while
 * pressing or releasing and creeps slowly (this rate) while depth is
 * steady, when a real bend must be protected. Like a One Euro Filter, or
 * motion-artifact rejection in biosignals: gate on an independent
 * detector of the confound instead of modeling it. It needs no
 * calibration capture (a static depth-vs-tilt curve was tried and didn't
 * hold up). Tradeoff: a bend held for many seconds could slowly relax.
 * Unmeasured. */
#define PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW 0.002f
/* Baseline rate while depth is actively changing (depth_activity ~1): a
 * bit faster than one smoothing stage (0.08), so it keeps up with a strike
 * without being as noisy as raw samples. Unmeasured. */
#define PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST 0.25f
/* Smoothed SIGNED depth rate that counts as fully "pressing"
 * (depth_activity = 1). Signed and smoothed because depth also wobbles
 * during a real tilt; only consistent movement should count (against the
 * raw per-tick delta, 8.0 saturated during tilting and killed bend).
 * The threshold against the smoothed signal is still a first guess. */
#define PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE 8.0f

/* Confirmation window: a deviation past the deadzone ramps from 0 to full
 * weight over this many ms, so brief wobbles stay tiny while a held tilt
 * reaches full weight quickly. The ramp restarts when the deviation falls
 * back inside the deadzone (a real return to center, not a sign flicker).
 * A capture of a straight hold showed mechanical "give" overlapping real
 * tilt in size (give: median 0.030, p90 0.069, max 0.106), but give
 * flickers within ~100-200 ms while a tilt holds one direction. 120 ms
 * exploited that; with the two-stage smoothing and depth-gated
 * recentering removing most of the noise upstream it went to 60, then 30
 * ms ("less trigger time"). Unmeasured; lower only with a fresh capture. */
#define PITCH_BEND_ARM_MS 30u

/* EMA smoothing on X/Y/magnitude, applied as a TWO-STAGE cascade. Hand
 * tremor sits at 6-15 Hz; one EMA stage at this alpha (~1.7-2 Hz cutoff,
 * -6 dB/octave) leaves ~20% of it. Two identical stages double the
 * rolloff (-12 dB/octave) for only ~1.4x the settling time. 0.08 per stage
 * (down from 0.35/0.15, to damp jitter on an active bend).
 * Invariant: the live delta and the baseline recenter target must read
 * the SAME, fully cascaded stage. Splitting or dropping the second stage
 * was tried twice and brought back pressure-triggered bend and jitter. */
#define PITCH_BEND_SMOOTHING_ALPHA 0.08f

typedef enum {
    PAD_STATE_IDLE = 0,
    PAD_STATE_AWAITING_STRIKE,
    PAD_STATE_NOTE_ON,
    /* The pad's channel was stolen by a newer note while the finger stayed
     * down. Not IDLE: re-awaiting a strike would read the pad's already
     * pressed depth as an instant max-velocity hit (seen with 8+ keys held).
     * It stays silent until a real release; stolen voices never come back on
     * their own (like haptics). */
    PAD_STATE_STOLEN,
} pad_expr_state_t;

typedef struct {
    pad_expr_state_t state;
    uint32_t touch_start_ms;

    /* Touch start on the Hall sample clock (strike_time_ms is measured from
     * here). */
    uint32_t touch_start_sample_ms;

    /* Highest RAW depth since touch began, including the very first reading.
     * Raw (hall.c's depth is already baseline-relative): a hard hit can finish
     * before capacitive touch registers, so the first reading can already be
     * fully pressed (880-1040 seen), and a per-touch reference would have
     * thrown that away. The peak (not the current value) also catches a
     * strike that springs back quickly. */
    float peak_depth;

    /* Time from touch start to peak_depth first crossing
     * MIN_STRIKE_DEPTH_DELTA; ~0 if already past it at touch-down.
     * threshold_crossed makes it a one-time capture. */
    uint32_t strike_time_ms;
    bool threshold_crossed;

    uint32_t last_seen_sample_time_ms;

    /* When the note fired (for RETRIGGER_GRACE_MS). */
    uint32_t note_on_ms;

    /* EMA of depth for pressure (AFTERTOUCH_SMOOTHING_ALPHA), seeded at
     * note-on so pressure doesn't ramp up from 0. */
    float smoothed_depth;

    /* Smoothed depth RATE (units/ms) while touched, negative when lifting:
     * the release velocity source (see "Release velocity"). prev_raw_depth /
     * last_depth_sample_ms feed it; last_depth_sample_ms == 0 = no sample yet
     * this hold. */
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

    /* This note's channel: a Member Channel with MPE on, the Master Channel
     * (ch 1) with MPE off. Valid in PAD_STATE_NOTE_ON. */
    uint8_t midi_channel;
    /* Strike order (s_next_mpe_claim_seq), set on every note-on; used with
     * MPE off to find the most recently struck held pad
     * (s_non_mpe_owner_pad). */
    uint32_t touch_claim_seq;

    /* Per-note pitch bend state. pitch_bend_active = whether bend was enabled
     * when THIS note fired (toggling mid-hold doesn't change it). */
    bool pitch_bend_active;
    /* Bend baseline X/Y, taken after PITCH_BEND_SETTLE_MS and then kept
     * tracking the smoothed X/Y while no bend run is active, at a rate that
     * adapts to depth movement (see PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW).
     * This keeps pressure-related drift from reading as tilt without any
     * calibration data. (A self-learning two-zone model and a fixed captured
     * depth-vs-Y curve were tried first and dropped.) */
    float pitch_bend_baseline_x;
    float pitch_bend_baseline_y;
    /* Previous tick's smoothed depth, for pitch_bend_smoothed_depth_rate
     * (which drives depth_activity and the adaptive baseline rate). */
    float pitch_bend_prev_depth;
    /* EMA of the SIGNED depth delta. Depth also swings a lot during a real
     * tilt (pressure shifts across the pad), but back and forth; a press or
     * release keeps one sign. Averaging the signed delta cancels the wobble,
     * so only real pressing reads as activity (the raw magnitude kept the
     * baseline snapping and swallowed real tilts). */
    float pitch_bend_smoothed_depth_rate;
    /* First smoothing stage of X/Y: used for the initial baseline and as the
     * medium-speed "where the pad is settling" signal. */
    float pitch_bend_smoothed_x;
    float pitch_bend_smoothed_y;
    /* Second cascade stage. Both the live delta (current_cosine_x/y) and the
     * baseline recenter target read THIS stage (see PITCH_BEND_SMOOTHING_ALPHA
     * for why they must match). */
    float pitch_bend_smoothed_x2;
    float pitch_bend_smoothed_y2;
    /* One stage: magnitude is read identically by both cosine terms, so its
     * lag cancels out. */
    float pitch_bend_smoothed_magnitude;
    /* Magnitude used by both cosine terms; frozen while a bend run is active,
     * like the baseline (see the scan loop). */
    float pitch_bend_run_magnitude;
    /* The compensated delta for this tick. Set directly (no third smoothing
     * stage; see the scan loop). */
    float pitch_bend_smoothed_delta;
    bool pitch_bend_baseline_settled;
    uint32_t pitch_bend_claim_ms;
    uint16_t pitch_bend_last_sent;
    /* Run tracking for PITCH_BEND_ARM_MS: run_active = the delta is outside
     * the deadzone; run_positive = which side (a sign flip starts a new run);
     * run_start_ms = when this run began. */
    bool pitch_bend_run_active;
    bool pitch_bend_run_positive;
    uint32_t pitch_bend_run_start_ms;
    /* Separate fast-wiggle vibrato detector (see VIBRATO_ENERGY_NOISE_FLOOR):
     * wiggle_energy = smoothed gap between the two cascade stages;
     * wiggle_active/_start_ms confirm it the same way as the run fields. */
    float pitch_bend_wiggle_energy;
    bool pitch_bend_wiggle_active;
    uint32_t pitch_bend_wiggle_start_ms;

    /* pedal.sustain_style HOLD: this pad's released note, kept ON until the
     * pedal lifts. At most one per pad (its next strike closes it first; see
     * release_held_note()). With MPE its channel stays claimed until then.
     * held_* are captured at release, since the pad moves on to new notes. */
    bool held;
    uint8_t held_channel;
    uint8_t held_note;
    uint8_t held_release_velocity;
} pad_expr_t;

static pad_expr_t s_pads[TILES_NUM_PADS];

/* The player's pitch bend on/off preference; each note latches it in
 * pitch_bend_active. */
static bool s_pitch_bend_enabled;


/* Member Channel table, one slot per shared-pool channel. Which channel a
 * note gets and which note is stolen is services/mpe_alloc.h's job (MPE
 * spec 3.2: same note's old channel, else the one idle longest; steal the
 * oldest). See claim_mpe_channel(). */
/* Sized for the zone's maximum (8, services/midi_channels.h). */
static tiles_mpe_slot_t s_mpe_channels[TILES_MIDI_SHARED_POOL_SIZE];
static uint32_t s_next_mpe_claim_seq = 1u;

/* ---- MPE on/off -------------------------------------------------------------
 * Default on. With MPE off, the most compatible plain-MIDI layout (see
 * tiles_expression_set_mpe_enabled()). */
static bool s_mpe_enabled = true;
/* Stamps each slot's release_seq as it's freed. */
static uint32_t s_next_mpe_release_seq = 1u;

/* The ONLY place a pool channel's in_use changes, so it's always mirrored
 * into services/midi_channels.h (tiles_midi_channels_note_channel_
 * claimed()/_released()); otherwise Song mode could claim a channel a
 * live note is using. */
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
 * MIDI sustain holds every note on the channel/zone, harmonic plucks
 * included, so to sustain real notes but not harmonics TILES must hold
 * them itself (services/pedal.h). Rules that make it safe: every
 * end-of-note path holds (retrigger included); a pad that strikes again
 * closes its held note first, like re-striking a piano key; off by
 * default. */

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
 * Melodic harmonics: while ONE note is held with the sustain pedal down,
 * lightly touching other pads plucks that note's harmonics (like a piano's
 * sympathetic resonance with the dampers up). No hardware convention for
 * this exists.
 *   - By TOUCH ORDER, not pad position: the 1st other pad touched plays
 *     the octave, the 2nd the octave + fifth, ... (HARMONIC_SEMITONES).
 *     Position would shift with the scale/key and doesn't land on true
 *     harmonic ratios.
 *   - Harmonics 2-6 only; higher ones detune and crowd in equal
 *     temperament.
 *   - Fixed gentle velocity (HARMONIC_VELOCITY).
 *   - A PLUCK: fires on the touch edge and rings a fixed time
 *     (HARMONIC_PLUCK_DURATION_MS) whether or not the finger stays.
 *     Touching the same pad again re-plucks it.
 *   - Pedal required: a resting palm never plays harmonics, so no palm
 *     rejection is needed.
 *   - A touched pad pressed into a real strike becomes a real note; with
 *     two notes held the session ends and every harmonic stops ("not in
 *     polyphony").
 *   - Melodic and chord mode (the melody grid), not bass guitar
 *     (tiles_op_mode_melodic_harmonics_may_play()).
 *   - Channels: harmonics never take a real note's channel; see
 *     harmonic_channel_is_reserved(). */

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

/* Chord vs. harmonic separation (chord fingers land a few ms apart, so
 * later fingers' touches used to pluck before they pressed):
 *
 * ARM (features.harmonics.arm_ms): plucking starts only once the
 * fundamental NOTE has been held alone this long (from its note_on_ms);
 * touch edges inside the window are consumed. Measured from the note, so
 * pressing the pedal over an already-held note plucks at once.
 *
 * CONFIRM (features.harmonics.confirm_ms): after arming, a new touch waits
 * this long and is cancelled if the key moves past PRESS DEPTH
 * (features.harmonics.press_depth, raw Hall units) or strikes. A pressing
 * finger moves the key almost at once; a light harmonic touch barely does.
 * A quick tap that ends inside the window still plucks.
 *
 * Defaults from a traced session (334 harmonic touches, 10 chord fingers
 * over a held note):
 *   - Harmonic touches peak at 47-63 depth (p90 96, max 144): press depth
 *     128 plucks 331/334 (40 lost 249).
 *   - Chord fingers strike within ~60 ms, or (2 of 10) press slowly and
 *     look like light touches for 80 ms; those leak at any depth >= 80.
 *     A leaked pluck ends when that finger strikes, and isn't sustained
 *     in HOLD style.
 *   - Confirm 40 ms: 60/80 ms caught nothing more; 20 ms leaked 5/10.
 *   - Arm 150 ms: only 1 of 334 harmonic touches came sooner.
 * Small chord sample; the settings are there to tune by ear. */
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
    /* Reserve only while a session is active (exactly one real note held).
     * Reserving whenever the feature was on capped real polyphony at 3 of 8
     * channels, so a 4th finger stole the oldest note, which then retriggered
     * (MPE glitch plus a haptic re-kick). A second real note ends the session
     * the same scan, freeing the whole zone. */
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

/* Once per scan, after the pad loop:
 *   0. Off unless the pedal holds sustain and the mode allows harmonics:
 *      then every voice stops and touch memory is forced false, so
 *      pressing the pedal over pads already touched plucks them at once.
 *   1. Exactly one held pad = the fundamental; otherwise end everything.
 *   2. Voices past HARMONIC_PLUCK_DURATION_MS decay.
 *   3. Other pads whose touch just began pluck (once armed and confirmed
 *      light; see HARMONIC_ARM_MS_DEFAULT): a new pad takes the next
 *      slot, a ringing one re-plucks its own. */
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

/* With MPE off: which held pad drives channel 1's bend and pressure (0 =
 * none). The newest strike takes over; when it releases, the most recent
 * other held pad takes back over (like a bend wheel that keeps working
 * under a second finger). Unused with MPE on. */
static uint8_t s_non_mpe_owner_pad;

void tiles_expression_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pads[i] = (pad_expr_t){0};
        s_pads[i].state = PAD_STATE_IDLE;
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

/* Returns X, Y and |B| = sqrt(x^2+y^2+z^2) separately: the depth
 * compensation needs the current magnitude and the baseline X/Y per axis. */
static void hall_xy_and_magnitude(int16_t x, int16_t y, int16_t z, float *x_out, float *y_out, float *magnitude_out) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    *x_out = fx;
    *y_out = fy;
    *magnitude_out = sqrtf(fx * fx + fy * fy + fz * fz);
}

/* Direction cosine: component / |B| (depth-invariant to first order; see
 * "Pitch bend from tilt"). A near-zero |B| returns 0. */
static float direction_cosine_from(float x_component, float magnitude) {
    if (magnitude < 1.0f) {
        return 0.0f;
    }
    return x_component / magnitude;
}

/* Confidence ramp: 0..1 over PITCH_BEND_ARM_MS of a run, then 1.
 * `hold_ms` = how long the current run has lasted (0 = none). */
static float pitch_bend_confidence_multiplier(uint32_t hold_ms) {
    if (hold_ms >= PITCH_BEND_ARM_MS) {
        return 1.0f;
    }
    return (float)hold_ms / (float)PITCH_BEND_ARM_MS;
}

/* The MPE spec's default bend range: what a receiver uses if it ignores
 * RPN 0. */
#define PITCH_BEND_MPE_SPEC_DEFAULT_RANGE_SEMITONES 48.0f

/* Scale the wire value so max tilt = 12 semitones even on a receiver stuck
 * at 48: Equator and Serum both ignored the RPN 0 declaration (Equator's
 * range is set by the user; negotiation isn't done in practice), and max
 * tilt swung 4 octaves. So max output is 12/48 = 25% of full scale. The
 * RPNs are still sent. Tradeoff: a receiver that does honor RPN 0 gets
 * only ~3 semitones; none has been found yet. */
#define PITCH_BEND_WIRE_RANGE_COMPENSATION \
    ((float)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES / PITCH_BEND_MPE_SPEC_DEFAULT_RANGE_SEMITONES)

/* Linear mapping on purpose. An ease-out curve for big tilts was tried and
 * reverted: it was applied after the confidence ramp (amplifying the ramp
 * itself), and any curve from (0,0) to (1,1) that calms the top must be
 * steeper lower down, where most playing and residual noise live. If the
 * top needs smoothing, reshape only a narrow region near the clamp. */

/* Cosine delta (depth-compensated, sign-corrected by the caller) -> 14-bit
 * wire value. The deadzone is a soft knee: centered inside it, a
 * continuous ramp past it, full swing still at
 * s_pitch_bend_max_cosine_deviation; then the confidence ramp applies.
 * Updates this pad's own run tracking. */
static uint16_t pitch_bend_14bit_from_cosine_delta(pad_expr_t *s, float delta, uint32_t now_ms) {
    float magnitude = fabsf(delta);
    bool positive = delta >= 0.0f;
    float sign = positive ? 1.0f : -1.0f;
    if (magnitude <= PITCH_BEND_DEADZONE_COSINE_DELTA) {
        /* Inside the deadzone: no run. */
        s->pitch_bend_run_active = false;
        magnitude = 0.0f;
    } else {
        /* A run starts on the first deviation after the deadzone, NOT on every
         * sign flip: a held tilt's angle wobbles (X's sign flipped almost every
         * sample during a real sustained tilt), and resetting on each flip kept
         * confidence at zero and the baseline chasing (run_active gates
         * recentering), erasing the tilt. Only returning to the deadzone ends a
         * run. The output sign still comes from this tick. */
        if (!s->pitch_bend_run_active) {
            s->pitch_bend_run_active = true;
            s->pitch_bend_run_start_ms = now_ms;
        }
        s->pitch_bend_run_positive = positive;
        magnitude -= PITCH_BEND_DEADZONE_COSINE_DELTA;
    }

    float usable_range = s_pitch_bend_max_cosine_deviation - PITCH_BEND_DEADZONE_COSINE_DELTA;
    if (usable_range < 0.01f) {
        /* If the menu ever sets sensitivity at or below the deadzone, keep a
         * small usable range instead of dividing by zero. */
        usable_range = 0.01f;
    }
    float normalized = sign * (magnitude / usable_range);

    uint32_t hold_ms = s->pitch_bend_run_active ? (now_ms - s->pitch_bend_run_start_ms) : 0u;
    normalized *= pitch_bend_confidence_multiplier(hold_ms);

    if (normalized > 1.0f) {
        normalized = 1.0f;
    }
    if (normalized < -1.0f) {
        normalized = -1.0f;
    }
    int32_t bend = (int32_t)PITCH_BEND_CENTER +
                   (int32_t)(normalized * 8191.0f * PITCH_BEND_WIRE_RANGE_COMPENSATION);
    if (bend < 0) {
        bend = 0;
    }
    if (bend > 16383) {
        bend = 16383;
    }
    return (uint16_t)bend;
}

/* Separate fast-wiggle vibrato detector. It only READS the two cascade
 * stages and adds its output on top of the final wire value, so it can't
 * disturb the tilt/pressure path (two attempts that lightened the shared
 * signal both broke pressure stability).
 *
 * Why it's needed: the two-stage smoothing that fights pressure coupling
 * also kills a fast wiggle (a captured tilt reached 94% bend, a fast
 * wiggle of similar raw amplitude never passed 12%). Fine-grained sensors
 * (LinnStrument, Continuum) don't need this; this one does.
 *
 * Signal: the gap between the light first stage and the fully cascaded
 * second stage. Settled signals (steady press, held tilt) keep it small;
 * a real wiggle keeps it large. Capture on one pad (smoothed gap):
 *   - holding still:   mean 7.5, sustained <= ~9
 *   - deliberate tilt: mean 8.5, one single-tick blip to ~12 at release
 *   - fast wiggle:     mean 16.2, above 11 for 93% of the gesture
 * Strike onsets stayed under 7. */
#define VIBRATO_ENERGY_NOISE_FLOOR 8.0f
/* Energy for full vibrato depth (a real wiggle sustains ~16). 8..16 is a
 * soft knee, so a borderline gesture fades in rather than clicking on. */
#define VIBRATO_ENERGY_FULL_SCALE 16.0f
/* Wiggle energy must stay above the floor this long before vibrato fades
 * in. The only non-wiggle gesture that came close was a single tick when
 * releasing a held tilt, and release tails keep their bend (so it would
 * be heard); a real wiggle stays above the floor 93% of the time. */
#define VIBRATO_ARM_MS 80u
/* Fixed rate, not tracked from the gesture (too little signal for a
 * stable estimate). 5.5 Hz is inside natural string/voice vibrato
 * (4-8 Hz). */
#define VIBRATO_RATE_HZ 5.5f
/* Peak added depth in wire units: ~0.9 semitone (after the wire
 * compensation, 12 semitones ~ 2047 units, ~171 per semitone). A design
 * choice; tune by ear. */
#define VIBRATO_MAX_WIRE_DEPTH 150.0f
/* Local pi: M_PI isn't guaranteed on this newlib target. */
#define TILES_EXPRESSION_VIBRATO_PI 3.14159265358979323846f

/* Adds the vibrato to an already computed bend value, after
 * pitch_bend_14bit_from_cosine_delta(), so it can't affect the tilt path. */
static uint16_t pitch_bend_apply_vibrato(pad_expr_t *s, uint16_t bend, uint32_t now_ms) {
    bool above_floor = s->pitch_bend_wiggle_energy > VIBRATO_ENERGY_NOISE_FLOOR;
    if (above_floor) {
        if (!s->pitch_bend_wiggle_active) {
            s->pitch_bend_wiggle_active = true;
            s->pitch_bend_wiggle_start_ms = now_ms;
        }
    } else {
        s->pitch_bend_wiggle_active = false;
    }

    uint32_t hold_ms = s->pitch_bend_wiggle_active ? (now_ms - s->pitch_bend_wiggle_start_ms) : 0u;
    float confirm = (hold_ms >= VIBRATO_ARM_MS) ? 1.0f : (float)hold_ms / (float)VIBRATO_ARM_MS;

    float energy_above_floor = s->pitch_bend_wiggle_energy - VIBRATO_ENERGY_NOISE_FLOOR;
    if (energy_above_floor < 0.0f) {
        energy_above_floor = 0.0f;
    }
    float depth = energy_above_floor / (VIBRATO_ENERGY_FULL_SCALE - VIBRATO_ENERGY_NOISE_FLOOR);
    if (depth > 1.0f) {
        depth = 1.0f;
    }
    depth *= confirm;
    if (depth <= 0.0f) {
        return bend;
    }

    /* Phase via integer modulo before converting to float: a raw uptime in ms
     * cast to float loses precision and would drift the LFO over long uptimes. */
    uint32_t period_ms = (uint32_t)(1000.0f / VIBRATO_RATE_HZ);
    uint32_t phase_ms = now_ms % period_ms;
    float lfo = sinf(2.0f * TILES_EXPRESSION_VIBRATO_PI * (float)phase_ms / (float)period_ms);

    int32_t with_vibrato = (int32_t)bend + (int32_t)(depth * VIBRATO_MAX_WIRE_DEPTH * lfo);
    if (with_vibrato < 0) {
        with_vibrato = 0;
    }
    if (with_vibrato > 16383) {
        with_vibrato = 16383;
    }
    return (uint16_t)with_vibrato;
}

/* Centers every pad that is bending (with MPE, several can be) and turns
 * their bend off. Used when pitch bend is switched off. */
static void center_and_deactivate_all_bending_pads(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        pad_expr_t *s = &s_pads[i];
        if (s->state == PAD_STATE_NOTE_ON && s->pitch_bend_active) {
            tiles_midi_send_pitch_bend(s->midi_channel, PITCH_BEND_CENTER);
            s->pitch_bend_last_sent = PITCH_BEND_CENTER;
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

void tiles_expression_set_pitch_bend_sensitivity(float max_cosine_deviation) {
    s_pitch_bend_max_cosine_deviation = max_cosine_deviation;
    printf("[expression] pitch bend sensitivity (max cosine deviation) now %.3f\n",
           (double)s_pitch_bend_max_cosine_deviation);
}

float tiles_expression_get_pitch_bend_sensitivity(void) {
    return s_pitch_bend_max_cosine_deviation;
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
     * before touch registers, and that's an instant, maximum-speed strike. */
    float initial_depth = (float)tiles_hall_get_depth(pad);
    s->peak_depth = initial_depth;
    s->threshold_crossed = (initial_depth >= MIN_STRIKE_DEPTH_DELTA);
    s->strike_time_ms = 0;
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

/* Starts pitch bend tracking for `pad`'s new note, latching whether bend
 * is enabled now. The baseline is captured later (PITCH_BEND_SETTLE_MS). */
static void init_pitch_bend_for_pad(pad_expr_t *s, uint8_t pad, uint32_t now_ms) {
    s->pitch_bend_active = s_pitch_bend_enabled;
    if (!s->pitch_bend_active) {
        return;
    }
    tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
    float x, y, magnitude;
    hall_xy_and_magnitude(hs.x, hs.y, hs.z, &x, &y, &magnitude);
    /* Seed every smoothing stage with the current reading, so there's no fake
     * ramp up from 0 on each note. */
    s->pitch_bend_smoothed_x = x;
    s->pitch_bend_smoothed_x2 = x;
    s->pitch_bend_smoothed_y = y;
    s->pitch_bend_smoothed_y2 = y;
    s->pitch_bend_smoothed_magnitude = magnitude;
    s->pitch_bend_run_magnitude = magnitude;
    /* Fresh depth read, not s->smoothed_depth (still the previous note's
     * value at this point), so depth_activity doesn't start with a false
     * jump. */
    s->pitch_bend_prev_depth = (float)tiles_hall_get_depth(pad);
    /* Reset, so the previous note's rate can't bias the first ticks. */
    s->pitch_bend_smoothed_depth_rate = 0.0f;
    s->pitch_bend_smoothed_delta = 0.0f;
    s->pitch_bend_baseline_settled = false;
    s->pitch_bend_claim_ms = now_ms;
    s->pitch_bend_last_sent = PITCH_BEND_CENTER;
    s->pitch_bend_run_active = false;
    /* Reset, so the previous note's energy can't bias the vibrato detector. */
    s->pitch_bend_wiggle_energy = 0.0f;
    s->pitch_bend_wiggle_active = false;
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

/* Ends `pad`'s note: pressure 0, Note-Off, haptic stop, CV/gate off, and
 * frees its MPE channel. The single place this happens (note-off,
 * retrigger, steal). Callers set the next state.
 *
 * Bend is NOT recentered here: control of a note ends at its Note-Off
 * (MPE spec), and recentering snapped every bent release tail and
 * pedal-sustained note. The next note on the channel is set up by
 * tiles_midi_send_note_setup() before its Note-On.
 *
 * The Note-Off goes out now even with the sustain pedal down: sustaining
 * is the synth's job (pedal.c sends CC64 on the Master Channel), as on
 * every keyboard and MPE controller. The spec (RP-053 1.3, 3.3) has the
 * controller send Note-Off while the damper keeps the note sounding.
 * Withholding Note-Offs until pedal release was tried and caused hung
 * notes (a re-strike sent a second Note-On with no Note-Off; with MPE off
 * one Note-Off covered two Note-Ons) and stacked identical notes on two
 * channels. Don't bring it back. (HOLD style below is the one deliberate,
 * safe exception.)
 *
 * player_release: true only when the finger actually let go (touch
 * ended), the only case with a lift speed to report; everything else
 * sends release velocity 0.
 *
 * may_hold: with pedal.sustain_style HOLD and the pedal down, the
 * Note-Off waits (recorded in held_*) until release_held_note(): pedal
 * up, the pad striking again, or the same pitch sounding again. True for
 * both ways a finger lets go (touch ending, and the retrigger path); false
 * for a steal or reset. Haptics and CV/gate stop immediately either way.
 * With the default SYNTH style nothing is held. */
static void end_held_note(pad_expr_t *s, uint8_t pad, bool player_release, bool may_hold) {
    tiles_cv_gate_note_off(s->active_note);
    tiles_haptics_stop(pad);
    s->pitch_bend_active = false;
    if (s->midi_channel != TILES_MIDI_MPE_MASTER_CHANNEL && s->last_sent_aftertouch != 0u) {
        /* MPE: pressure 0 right before Note-Off, even if the Note-Off waits for
         * the pedal. Not on channel 1 with MPE off (its pressure belongs to the
         * pad still being played). */
        tiles_midi_send_channel_pressure(s->midi_channel, 0u);
        s->last_sent_aftertouch = 0u;
    }
    uint8_t release_velocity = player_release ? release_velocity_from_fall_rate(s->depth_fall_rate) : 0u;
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
        /* Channel 1 has no pool slot (index math would underflow), hence this
         * separate branch. If this pad drove channel 1's bend/pressure, hand it
         * to the most recently struck other held pad and resend its current bend.
         * With nothing else held the bend stays; the next Note-On's setup centers
         * it. */
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

/* Called when game mode takes over: ends every sounding note (no release
 * velocity, no holding), resets pads that were only awaiting a strike
 * (they sent nothing, so no Note-Off), and releases pedal-held notes,
 * since nothing would release them during the game. */
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

/* Member Channel allocator: returns the channel for `pad`'s new `note`.
 * The choice (and which note to steal when full) is services/mpe_alloc.h's
 * MPE-spec order; this builds the eligibility masks and claims. A stolen
 * channel goes straight to the new pad.
 * Eligible = in the declared zone, not held by Song
 * (slot_in_live_zone()), not reserved for harmonics. Fixed parts' channels
 * are outside the zone, so a note can never land on one. */
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
        /* Nothing stealable: every zone channel is reserved for harmonics, or the
         * zone is empty (Song holds all 8). Falls back to the first member
         * channel, which may collide with a Song track in that extreme case.
         * Accepted. */
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

/* How long TILES's side of the zone must be silent (no played, held or
 * plucked note, pedal up) before a Song-driven zone change is declared
 * (a re-declaration stops every note). The extra time covers release
 * tails TILES can't see. */
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

    /* Declare a zone size changed by Song only once idle (a re-declaration
     * stops notes); meanwhile the allocator uses the declared size minus
     * Song's channels. Skipped with MPE off (zone already withdrawn).
     * mpe_zone_is_idle() runs every scan to keep its timestamp current. */
    bool zone_idle = mpe_zone_is_idle(now_ms);
    if (s_mpe_enabled && zone_idle && tiles_midi_channels_zone_redeclare_pending()) {
        tiles_expression_announce_mpe_zone();
    }

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        pad_expr_t *s = &s_pads[i];

        bool raw_touched = tiles_touch_is_touched(pad);
        if (raw_touched) {
            s->last_touched_ms = now_ms;
            s->last_touched_valid = true;
        }
        /* Dropout-bridged touch (a raw touch always reads true here). */
        bool touched = pad_touch_bridged(s, now_ms);

        if (s->state == PAD_STATE_STOLEN) {
            /* Stolen pad: silent until a real release, then IDLE. */
            if (!touched) {
                s->state = PAD_STATE_IDLE;
            }
            continue;
        }

        if (s->state == PAD_STATE_IDLE) {
            /* No NEW strike while something else owns the grid (expression menu,
             * transpose, op_mode menus/pads, game mode); a pad already past IDLE is
             * left to finish. op_mode is asked per pad (tiles_op_mode_owns_pad()):
             * chord mode only claims its strip, the melody pads still play here. */
            if (touched && !tiles_expression_control_owns_pad_grid() && !tiles_octave_control_is_transpose_active() &&
                !tiles_op_mode_owns_pad(pad) && !tiles_game_mode_is_active()) {
                begin_awaiting_strike(s, pad, now_ms);
                /* Touch-only haptic tick, whether or not it becomes a press. */
                tiles_haptics_trigger_touch_pulse(pad);
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
                    float depth = (float)tiles_hall_get_depth(pad);
                    if (depth > s->peak_depth) {
                        s->peak_depth = depth;
                    }
                    if (!s->threshold_crossed && s->peak_depth >= MIN_STRIKE_DEPTH_DELTA) {
                        /* First sample past the threshold: record strike_time_ms. */
                        s->threshold_crossed = true;
                        s->strike_time_ms = hs.sample_time_ms - s->touch_start_sample_ms;
                    }
                }
            }

            /* Uses the PEAK (threshold_crossed): a fast hit can spring back before
             * the current reading would show it. */
            bool pressed = s->threshold_crossed;

            /* ready = pressed and the follow-through window has passed while touched.
             * commit_on_release = touch ended before that: commit now with the depth
             * reached, rather than dropping a real hit or waiting for nothing. */
            uint32_t crossed_at_ms = s->touch_start_sample_ms + s->strike_time_ms;
            bool followthrough_elapsed = pressed && (now_ms - crossed_at_ms) >= VELOCITY_FOLLOWTHROUGH_MS;
            bool ready = touched && followthrough_elapsed;
            bool commit_on_release = !touched && pressed;

            if (ready || commit_on_release) {
                s->active_note = tiles_note_map_get_note(pad);
                uint8_t velocity = velocity_from_strike(s->strike_time_ms, s->peak_depth);
                /* Claim the note's channel, then send its per-note setup (bend center,
                 * pressure 0), so a Note-On never inherits the previous note's bend on
                 * that channel. */
                /* MPE off: every note on channel 1, no Member Channel. */
                /* HOLD style: close this pad's held note and any held note of the same
                 * pitch before the new Note-On (re-striking a held key), keeping one
                 * Note-Off per Note-On. No-op in SYNTH style. */
                release_held_note(s);
                release_held_notes_of_pitch(s->active_note);
                s->midi_channel =
                    s_mpe_enabled ? claim_mpe_channel(pad, s->active_note) : TILES_MIDI_MPE_MASTER_CHANNEL;
                init_pitch_bend_for_pad(s, pad, now_ms);
                s->touch_claim_seq = s_next_mpe_claim_seq++;
                if (!s_mpe_enabled) {
                    /* MPE off: the new strike takes over channel 1's bend/pressure; the setup
                     * below resets what the previous owner left. */
                    s_non_mpe_owner_pad = pad;
                }
                tiles_midi_send_note_setup(s->midi_channel);
                tiles_midi_note_on(s->midi_channel, s->active_note, velocity);
                tiles_cv_gate_note_on(s->active_note, velocity);
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
        if (!touched) {
            /* Touch ended: the finger let go (the one player_release=true call). */
            end_held_note(s, pad, true, true);
            s->state = PAD_STATE_IDLE;
            continue;
        }

        float raw_depth = (float)tiles_hall_get_depth(pad);

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
            if (hs.valid) {
                float x, y, magnitude;
                hall_xy_and_magnitude(hs.x, hs.y, hs.z, &x, &y, &magnitude);

                /* Two-stage cascade for X/Y, one stage for magnitude, every tick. Both the
                 * live delta and the baseline recenter read x2/y2. */
                s->pitch_bend_smoothed_x += PITCH_BEND_SMOOTHING_ALPHA * (x - s->pitch_bend_smoothed_x);
                s->pitch_bend_smoothed_x2 += PITCH_BEND_SMOOTHING_ALPHA * (s->pitch_bend_smoothed_x - s->pitch_bend_smoothed_x2);
                s->pitch_bend_smoothed_y += PITCH_BEND_SMOOTHING_ALPHA * (y - s->pitch_bend_smoothed_y);
                s->pitch_bend_smoothed_y2 += PITCH_BEND_SMOOTHING_ALPHA * (s->pitch_bend_smoothed_y - s->pitch_bend_smoothed_y2);
                s->pitch_bend_smoothed_magnitude += PITCH_BEND_SMOOTHING_ALPHA * (magnitude - s->pitch_bend_smoothed_magnitude);

                /* Vibrato energy from the gap between the two stages (read only), updated
                 * every tick so it's settled when needed. */
                {
                    float wiggle_residual_x = s->pitch_bend_smoothed_x - s->pitch_bend_smoothed_x2;
                    float wiggle_residual_y = s->pitch_bend_smoothed_y - s->pitch_bend_smoothed_y2;
                    float wiggle_residual_magnitude =
                        sqrtf(wiggle_residual_x * wiggle_residual_x + wiggle_residual_y * wiggle_residual_y);
                    s->pitch_bend_wiggle_energy +=
                        PITCH_BEND_SMOOTHING_ALPHA * (wiggle_residual_magnitude - s->pitch_bend_wiggle_energy);
                }

                /* Smoothed SIGNED depth delta (see pitch_bend_smoothed_depth_rate), with
                 * the same alpha as X/Y. */
                s->pitch_bend_smoothed_depth_rate +=
                    PITCH_BEND_SMOOTHING_ALPHA * ((s->smoothed_depth - s->pitch_bend_prev_depth) -
                                                   s->pitch_bend_smoothed_depth_rate);

                /* 0 (depth steady) .. 1 (changing fast), clamped (see
                 * PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE). */
                float depth_activity = fabsf(s->pitch_bend_smoothed_depth_rate) / PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE;
                if (depth_activity > 1.0f) {
                    depth_activity = 1.0f;
                }

                s->pitch_bend_prev_depth = s->smoothed_depth;

                if (!s->pitch_bend_baseline_settled) {
                    /* Stay centered until PITCH_BEND_SETTLE_MS, then take the baseline from
                     * the settled cascade. */
                    if ((now_ms - s->pitch_bend_claim_ms) >= PITCH_BEND_SETTLE_MS) {
                        s->pitch_bend_baseline_x = s->pitch_bend_smoothed_x2;
                        s->pitch_bend_baseline_y = s->pitch_bend_smoothed_y2;
                        s->pitch_bend_baseline_settled = true;
                        s->pitch_bend_smoothed_delta = 0.0f;
                    }
                } else {
                    /* Baseline recenter toward the smoothed X/Y, only while no run is
                     * confirmed (previous tick's state, to avoid a circular dependency), so a
                     * held bend survives pressing harder. Rate blends SLOW..FAST by depth
                     * activity (see PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW). */
                    if (!s->pitch_bend_run_active) {
                        /* Activity is CUBED before blending: FAST is ~125x SLOW, so a linear blend
                         * let everyday sensor noise (activity 0.1-0.3) pull the rate 10-40x above
                         * SLOW and erase real tilts. Cubing (0.3 -> 0.027) stays slow unless
                         * activity is clearly high; the endpoints are unchanged. */
                        float activity_shaped = depth_activity * depth_activity * depth_activity;
                        float recenter_alpha = PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW +
                                                (PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST -
                                                 PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW) *
                                                    activity_shaped;
                        s->pitch_bend_baseline_x += recenter_alpha * (s->pitch_bend_smoothed_x2 - s->pitch_bend_baseline_x);
                        s->pitch_bend_baseline_y += recenter_alpha * (s->pitch_bend_smoothed_y2 - s->pitch_bend_baseline_y);
                        /* Magnitude frozen with the baseline during a run (see below). */
                        s->pitch_bend_run_magnitude = s->pitch_bend_smoothed_magnitude;
                    }

                    float baseline_x_at_depth = s->pitch_bend_baseline_x;
                    float baseline_y_at_depth = s->pitch_bend_baseline_y;

                    /* Depth compensation. The direction cosine assumes a perfectly on-axis
                     * magnet; real assemblies aren't, so comparing against a fixed baseline
                     * COSINE let the misalignment grow as |B| changed with pressure (bend
                     * leaning one way regardless of tilt). Instead, predict the baseline
                     * cosine AT THE CURRENT magnitude: baseline_x / magnitude vs. x2 /
                     * magnitude. With no real tilt (x2 == baseline_x) they're identical by
                     * construction, so a pure depth change gives exactly 0.
                     *
                     * Invariants (each broke things when violated):
                     *   - Both terms divide by the SAME magnitude value this tick (an earlier
                     *     version compared a lagging smoothed cosine against an unsmoothed
                     *     prediction, and the lag itself read as a downward bend).
                     *   - Both X terms use the fully cascaded x2/y2, the same stage the
                     *     baseline recenters from. Using the lighter stage for the live term
                     *     (to let fast wiggles through) brought pressure-bend back; using it
                     *     for both made everything jittery (the deadzone and arm time were
                     *     tuned against x2). Fast wiggles are handled by the separate vibrato
                     *     detector.
                     *   - During a confirmed run the magnitude is frozen too
                     *     (pitch_bend_run_magnitude): with a real tilt, delta = (baseline_x -
                     *     x2) / magnitude still follows pressure, and a captured "steady"
                     *     hold swung depth 527-1059 and the bend by ~600 of ~2047. Tradeoff:
                     *     a deliberately large press change mid-bend reads against the stale
                     *     magnitude until the run ends.
                     *
                     * Sign: predicted - current (the other way bent backwards).
                     * Two axes: Y gets the same compensation; MAGNITUDE = sqrt(dx^2 + dy^2),
                     * SIGN from dx alone (the tuned left/right feel), not a true 2D bend. */
                    float predicted_baseline_cosine_x =
                        direction_cosine_from(baseline_x_at_depth, s->pitch_bend_run_magnitude);
                    float current_cosine_x =
                        direction_cosine_from(s->pitch_bend_smoothed_x2, s->pitch_bend_run_magnitude);
                    float delta_x = predicted_baseline_cosine_x - current_cosine_x;

                    float predicted_baseline_cosine_y =
                        direction_cosine_from(baseline_y_at_depth, s->pitch_bend_run_magnitude);
                    float current_cosine_y =
                        direction_cosine_from(s->pitch_bend_smoothed_y2, s->pitch_bend_run_magnitude);
                    float delta_y = predicted_baseline_cosine_y - current_cosine_y;

                    float combined_magnitude = sqrtf(delta_x * delta_x + delta_y * delta_y);
                    float raw_delta_this_tick = (delta_x >= 0.0f) ? combined_magnitude : -combined_magnitude;

                    /* No third smoothing stage here (X/Y/magnitude are already cascaded; a
                     * third stage made bends too slow to start). smoothed_delta is simply set. */
                    s->pitch_bend_smoothed_delta = raw_delta_this_tick;
                    float delta = s->pitch_bend_smoothed_delta;
                    uint16_t bend = pitch_bend_14bit_from_cosine_delta(s, delta, now_ms);
                    bend = pitch_bend_apply_vibrato(s, bend, now_ms);
                    if (bend != s->pitch_bend_last_sent) {
                        /* Always this pad's current bend (sent or not): used to resync channel 1
                         * when ownership returns to this pad (MPE off). */
                        s->pitch_bend_last_sent = bend;
                        if (s_mpe_enabled || s_non_mpe_owner_pad == pad) {
                            tiles_midi_send_pitch_bend(s->midi_channel, bend);
                        }
                    }
                }
            }
        }
    }
    if (s_harmonics_enabled) {
        scan_melodic_harmonics(now_ms);
    }
}
