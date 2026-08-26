#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "midi_out.h"
#include "haptics.h"
#include "expression_control.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* ============================================================================
 * Strike detection -- gated on real measured depth travel
 * (MIN_STRIKE_DEPTH_DELTA below), not touch alone. Velocity is derived
 * from elapsed TIME to reach that travel, not acceleration -- see the
 * "Velocity" section further below for why that changed and what it
 * replaced.
 *
 * Rebuilt from a real captured session, not a guess: two earlier rounds
 * of guessed constants (15, then 30 for MIN_STRIKE_DEPTH_DELTA) failed
 * on real hardware: "any touch still triggers midi." A debug-console
 * capture of the `[expression]` print below across ~140 real touches
 * gave the actual numbers MIN_STRIKE_DEPTH_DELTA is picked from: Hall
 * depth reads in steps of 16 raw counts (sensor/driver quantization) --
 * 32 (2 steps) was by far the single most common depth_delta observed
 * (66 of 115 fired notes), with a long tail up to 96 -- this is bare
 * capacitive contact / incidental mechanical settling, not an
 * intentional press. Genuine deliberate presses in the same capture
 * reached 192-736 (out of the ~900-unit full-press range from the
 * earlier Hall calibration session) -- a clear gap between "just
 * touched it" and "actually pressed it."
 * ==========================================================================*/

/* Bridges a brief capacitive touch dropout -- real feedback: "hard fast
 * press is not working properly, it won't trigger note." A hard,
 * percussive impact is exactly the scenario most likely to cause a
 * brief real dropout in the MPR121's touched reading (a finger
 * physically bouncing slightly off the sensing surface at the moment of
 * impact, momentarily breaking capacitive contact for a couple of ms
 * before settling back down) -- and this module's state machine treats
 * ANY observed `!touched` as a real release: mid-AWAITING_STRIKE, that
 * either commits early (if already pressed) or cancels to IDLE (if not
 * yet pressed); either way, a bounce arriving before MIN_STRIKE_DEPTH_
 * DELTA is reached restarts strike detection from scratch right as (or
 * after) the strike's true peak, so the freshly-restarted detection
 * window only ever sees the rebound on its way back down, never a rise
 * past threshold -- the note never fires at all. Rather than trust the
 * raw hardware reading at every single scan tick, this module now
 * treats touch as still active for a short window after the last RAW
 * true reading, bridging exactly this kind of brief bounce without
 * meaningfully delaying a genuine release (touch.c's own diagnostic
 * prints still reflect the true, undebounced hardware state -- this
 * tolerance is purely an expression-layer interpretation of it). Kept
 * short deliberately: this session separately narrowed the MPR121
 * release threshold specifically to make real release feel snappy (see
 * drivers/README.md's mpr121.c entry), and this shouldn't meaningfully
 * undo that. Unmeasured -- a first attempt at "long enough to bridge a
 * real bounce, short enough not to be felt as release lag." */
#define TOUCH_DROPOUT_GRACE_MS 12u

/* Minimum real depth travel (Hall units) since touch-down before a
 * touch counts as an actual press worth firing a note for, rather than
 * a light touch/rest with no real key motion -- see this section's own
 * header comment above for the real captured data this is picked from.
 * A touch that never crosses this just sits in PAD_STATE_AWAITING_STRIKE
 * until release cancels it with no note ever sent, matching how a real
 * key requires an actual press, not just contact.
 *
 * Also doubles as the elapsed-time model's actuation checkpoint (see
 * "Velocity: elapsed-time-to-actuation" below) -- how far a strike has
 * to travel before its speed even gets measured. Raised 150 -> 300
 * after real feedback that a fast-but-shallow flick still read as a
 * hard strike: "when I press faster but not deep the reading is still
 * strong." At 150 (comfortably above the ~96 touch-only ceiling but
 * still only ~17% of the ~900-unit full-press range), a light flick
 * needs very little real force to cover that little distance quickly,
 * so "fast" and "hard" weren't well correlated at that depth. 300
 * (~33% of full press) requires enough real travel that covering it
 * quickly takes genuine committed force, not just a flick -- the same
 * physical logic a spring/magnet mechanism already applies to any
 * motion: covering more distance in the same short time needs more
 * initial force, since the spring's return force works against it the
 * whole way. Still leaves ~67% of travel for aftertouch after the note
 * fires, same as a synth-action keybed's actuation point sitting well
 * before its mechanical bottom. Unmeasured against this specific
 * complaint -- the capture that validated the original 150 (see this
 * section's header) only measured "touch vs. press," not "how much
 * depth makes fast-but-light strikes rare"; revisit with a labeled
 * capture (explicit "light touch," "fast shallow flick," "real press"
 * trials) if light-fast still reads too hard or deliberate soft presses
 * stop registering. */
#define MIN_STRIKE_DEPTH_DELTA 300.0f

/* Retrigger threshold for a held note -- real feedback: "contact with
 * pad has to be broken for retrigger, that's bad." Raw depth (already
 * baseline-relative, same as everywhere else in this file) has to ease
 * back down to at or below this -- close to true rest -- before a
 * renewed press is treated as a brand-new strike, not just down from
 * this note's own peak. This is deliberately conservative and close to
 * "as light as touching it at all": a real held note's pressure is
 * expected to fluctuate somewhat for aftertouch's own sake, and a
 * threshold any looser risks
 * cutting off ordinary sustained holds the instant the player eases
 * pressure slightly, not just when they're clearly done with the note
 * and about to strike again. The flip side of that same conservatism:
 * a deliberate pressure fade-out (easing off gradually while still
 * holding the note, rather than releasing sharply) could still ease
 * below this and get cut early -- an inherent tension between "retrigger
 * without lifting" and "don't cut a fade-out short" that only real
 * playing can settle. Unmeasured -- a first attempt, not tuned against
 * either failure mode yet. */
#define RETRIGGER_ARM_DEPTH_DELTA 40.0f

/* Minimum time after a note fires before RETRIGGER_ARM_DEPTH_DELTA is
 * even checked -- a fast, percussive strike's own post-impact rebound
 * (Hall depth springing back toward baseline within a few ms of firing,
 * before the player has done anything else) could otherwise read as an
 * immediate deliberate release, causing a spurious note-off + retrigger
 * cycle milliseconds after the real note-on. Unmeasured -- long enough
 * to guess past a typical mechanical rebound, short enough not to
 * meaningfully delay a genuinely fast intentional retrigger. */
#define RETRIGGER_GRACE_MS 50u

/* ---- Velocity: elapsed-time-to-actuation, not acceleration -----------
 * Two prior attempts at accel-based velocity (double-differencing 3
 * Hall depth samples) both failed on real hardware, most recently:
 * "max sudden push does not trigger notes properly and light low depth
 * presses also trigger randomly hard. The logic and measurement method
 * is not working." That's a fair assessment of the *method*, not just
 * its constants: a double-difference over only 3 samples is extremely
 * sensitive to exactly which samples happen to land where, at exactly
 * what spacing -- and depth itself reads in coarse 16-count steps (see
 * this file's strike-detection header above), so a genuinely fast,
 * hard strike is precisely the case most likely to blow past
 * MIN_STRIKE_DEPTH_DELTA in only 1-2 samples, without ever reaching a
 * stable 3-sample accel estimate at all -- exactly matching "max sudden
 * push does not trigger properly." A slower press, meanwhile, gets
 * whatever accel its particular sample spacing happened to produce,
 * which the captured data showed had no reliable relationship to how
 * hard the press actually felt -- matching "light presses trigger
 * randomly hard."
 *
 * Replaced with the same technique real weighted-action MIDI keyboards
 * and drum pads use: measure the elapsed TIME between two fixed points
 * of travel, and derive velocity from how fast that gap was crossed --
 * a dual-contact-switch timing measurement, not a differentiated
 * position signal. Concretely: touch_start_sample_ms marks the instant
 * touch begins, and strike_time_ms (see pad_expr_t) is set exactly
 * once, the moment peak_depth first crosses MIN_STRIKE_DEPTH_DELTA, as
 * the gap between those two timestamps -- including the degenerate case
 * where depth was already past threshold at touch_start_sample_ms
 * itself (an extremely fast strike), which correctly comes out as
 * strike_time_ms ~= 0. This needs only two timestamps, not a
 * differentiated series -- immune to the per-sample noise/quantization
 * that broke the accel approach, and well-defined even when only one or
 * two samples arrive before the threshold is crossed (a fast, hard
 * strike no longer
 * needs 3 clean samples to register at all -- MIN_STRIKE_SAMPLES and
 * the whole 3-sample accel history are gone, along with the
 * MAX_STRIKE_WINDOW_MS fallback timeout they existed to support: with
 * nothing left to "wait for," a real press now commits the instant it's
 * measured, whether that took 3ms or 300ms).
 *
 * STRIKE_TIME_MAX_VELOCITY_MS/_MIN_VELOCITY_MS bound the curve: at or
 * below the "max" time, velocity pins at 127 -- the same deliberate
 * plateau *below* the fastest strike this hardware could ever produce
 * that the previous accel-based curve also aimed for, per real
 * feedback ("give some flat full velocity... to aid aftertouch"), so a
 * confidently fast hit reliably maxes out. At or above the "min" time,
 * velocity floors at MIN_VELOCITY -- a slow, deliberate push. Between
 * them, VELOCITY_CURVE_EXPONENT (> 1) shapes the curve the same way it
 * did before: suppressing the low end relative to a straight line, so
 * a merely-adequate-speed press reads noticeably quieter than a
 * confidently fast one, closer to how an acoustic action feels than a
 * linear response would. All three constants are first attempts, not
 * measured against real strikes -- there's no equivalent captured data
 * yet for "how many ms does a hard strike actually take to cross
 * MIN_STRIKE_DEPTH_DELTA on this hardware," unlike the depth-delta
 * numbers above. The `[expression]` print below now reports
 * strike_time_ms directly on every commit specifically so the next
 * real-hardware session can calibrate these three constants from real
 * numbers instead of guessing a third velocity model. */
#define STRIKE_TIME_MAX_VELOCITY_MS 10u
#define STRIKE_TIME_MIN_VELOCITY_MS 150u
#define VELOCITY_CURVE_EXPONENT 1.8f

/* Even a strike weak enough to barely clear MIN_STRIKE_DEPTH_DELTA
 * should produce an audible note, not near-silence -- the curve above
 * can push a very slow qualifying strike's raw output below this, so
 * it's still clamped up to a floor rather than left near-silent. */
#define MIN_VELOCITY 8u

/* Real calibration data, not a placeholder: a serial-driven capture
 * session (diagnostics/calibration.h's 'f' command) with all 24 magnets
 * seated measured a normal, regular full press -- which bottoms out the
 * pad's mechanical travel, there's no further "harder" position -- as
 * |raw Z - rest baseline| = 784 to 1184 across all 24 pads, average 918.
 * 900 sits in that range: every pad reaches its own true full press
 * comfortably past this point (127 well before the mechanical stop, not
 * exactly at it), and using the average rather than the low end of the
 * spread keeps real dynamic range across most of a strike's travel
 * instead of every pad capping out early to accommodate the single
 * least-sensitive one. A real per-pad calibration curve (correcting for
 * that spread individually) is still explicitly out of V1 scope -- see
 * hall.h.
 *
 * Runtime, not a fixed #define, so services/expression_control.h's
 * sub-menu (row 4, aftertouch sensitivity) can adjust it live via
 * tiles_expression_set_aftertouch_sensitivity() below. Defaults to
 * exactly this same calibrated 900 value. */
static uint16_t s_depth_to_aftertouch_full_scale = 900u;

/* Aftertouch is meant to read like continuing pressure after the
 * strike, not raw per-sample noise -- an exponential moving average
 * over the depth signal feeding aftertouch_from_depth() below, tuned to
 * be smooth without adding perceptible lag (the professional-feel goal
 * a real weighted-key/wind controller's aftertouch has). Deliberately
 * NOT applied to the velocity measurement above: velocity is a one-shot
 * elapsed-time measurement over the strike itself, where smoothing
 * would blunt the exact transient it's trying to measure; aftertouch is
 * a continuous signal sent for as long as a note is held, where
 * smoothing is what makes it feel like modulation instead of jitter.
 * Unmeasured -- a starting guess at the right amount of smoothing, not
 * derived from the capture session above (that only measured static
 * full-press depth, not how noisy a held reading is). */
#define AFTERTOUCH_SMOOTHING_ALPHA 0.35f

/* ---- Pitch bend from sideways motion -----------------------------------
 * Real feedback: "pitch bend on sideways motion for pads. This is only
 * relevant after the initial velocity and should compensate for
 * vertical movement in magnet and drift from aftertouch. Make sure the
 * math is solid before implementing."
 *
 * Only relevant after the initial velocity: strike detection (above)
 * never touches X/Y at all -- pitch bend is computed and sent only
 * while a note is already held (PAD_STATE_NOTE_ON), the same "only
 * matters once the strike itself is decided" scoping aftertouch already
 * uses. Toggled globally via tiles_expression_toggle_pitch_bend() (see
 * services/expression_control.c's square-button short-click handling).
 *
 * The math: naively using raw X (or X minus a baseline captured once)
 * as "how far sideways" would fail the "compensate for vertical
 * movement... and drift from aftertouch" requirement directly -- a
 * magnetic dipole's field strength changes with distance (Z depth), so
 * X's raw magnitude changes too as a pad is pressed harder or eased off
 * during aftertouch, even with ZERO real lateral motion. That would
 * read as spurious pitch bend drift every time the player simply
 * presses harder or softer, which is exactly the failure mode called
 * out.
 *
 * Fix: work with the field's DIRECTION, not its raw magnitude. For a
 * magnetic dipole, the ratio between an off-axis field component and
 * the total field magnitude (X / |B|, where |B| = sqrt(x^2+y^2+z^2)) is
 * a direction cosine -- it depends only on the angular position
 * relative to the dipole's axis, not on distance from it. Two field
 * readings at the same lateral tilt angle but different Z depths (a
 * harder or softer press, or aftertouch drift) produce the same
 * direction cosine even though X, Y, and Z individually all change
 * together with distance -- dividing by the total magnitude cancels
 * that shared distance-dependence out, leaving (to first order) just
 * the angle. This is the same principle real 3-axis Hall-effect
 * joysticks use to derive tilt independent of plunger depth.
 * hall_x_direction_cosine() below computes this cosine from a raw
 * sample; claim_pitch_bend_owner() seeds a per-note baseline cosine
 * (s_pitch_bend_baseline_cosine) at the exact moment a note fires
 * (mirroring how aftertouch seeds smoothed_depth at note-on, not from
 * 0), and everything sent afterward is the CHANGE in cosine from that
 * baseline -- so a strike landing at a slightly different rest tilt
 * than pad-to-pad manufacturing variance would otherwise imply doesn't
 * matter; only lateral motion *during* the held note does.
 *
 * Single hardware axis (X) chosen as "sideways" -- this project has no
 * hardware documentation on which local Hall axis corresponds to which
 * physical direction on a mounted pad, and MIDI pitch bend is
 * inherently one-dimensional (a single 14-bit value) regardless, so Y
 * is left unused for now rather than guessing how to blend two axes
 * into one bend value. Trivially swappable for Y once seen on real
 * hardware if X turns out to be the wrong physical axis.
 *
 * Real MIDI limitation, not a bug: this project is single-channel (see
 * midi_out.h's own "V1 scope" note, no MPE per-note channel allocation
 * yet), and Pitch Bend Change is a channel-wide message with no per-note
 * addressing in the MIDI spec itself -- there is no way to bend one held
 * note's pitch without also bending every other note currently held on
 * the same channel. Rather than send confusing, undefined-feeling output
 * when multiple pads are held, this module tracks a single "owner" pad
 * (s_pitch_bend_owner_pad, 0 = none) -- only the most recently struck
 * pad drives the shared channel's bend; when ownership changes (a new
 * strike while another pad is still held) or the owner releases, bend is
 * explicitly reset to center (8192) first so a new or otherwise-still-
 * held note never inherits a stale bend offset. Playing one pad at a
 * time behaves exactly as expected; holding a chord and bending is a
 * known, deliberate simplification -- true independent per-note bend
 * needs real MPE channel allocation, out of V1 scope. */
#define PITCH_BEND_CENTER 8192u

/* Cosine delta (see the section comment above) that maps to the full
 * +/-8191 MIDI range. Unmeasured -- there is no captured real-hardware
 * data yet for how much a deliberate sideways push actually moves this
 * ratio on this board's magnet/sensor geometry, unlike the depth-based
 * constants elsewhere in this file. Raised from an initial 0.15 -- real
 * feedback on real hardware: "very jittery and not responding to the
 * sideway tilt as expected... it should be not as sensitive." Doubling
 * it means a given amount of raw sensor noise (see PITCH_BEND_DEADZONE_
 * COSINE_DELTA below for the other half of that same fix) now maps to
 * roughly half the perceived bend it used to, alongside requiring a more
 * deliberate real tilt to reach full swing.
 *
 * Runtime, not a fixed #define, so services/expression_control.h's
 * sub-menu (row 2, pitch bend sensitivity) can adjust it live via
 * tiles_expression_set_pitch_bend_sensitivity() below -- a SMALLER value
 * here means MORE sensitive (less real motion needed to reach full
 * bend). Defaults to exactly this same 0.30 starting value. */
static float s_pitch_bend_max_cosine_deviation = 0.30f;

/* Small deltas this close to baseline are treated as exactly centered
 * (no bend at all) rather than passed through -- real feedback: "very
 * jittery... even with no tilt it jitters." Raw Hall X/Y/Z readings are
 * quantized (~16 raw-count steps -- see this file's strike-detection
 * section and hall.c for the same quantization affecting Z) and the
 * direction-cosine ratio is sensitive to that quantization even with
 * genuinely zero real lateral motion; PITCH_BEND_SMOOTHING_ALPHA's
 * exponential average (below) reduces but doesn't eliminate that noise
 * on its own, since it's a low-pass filter, not a floor. Applied as a
 * "soft knee" in pitch_bend_14bit_from_cosine_delta() below (subtracted
 * from the magnitude before normalizing, not a hard cutoff-then-jump)
 * so bend still ramps continuously from zero just past this threshold
 * rather than snapping straight to some nonzero value the instant it's
 * crossed. Unmeasured -- a first attempt at "comfortably above the
 * observed rest-state noise floor, still small relative to a deliberate
 * tilt," not derived from a captured real-noise session the way
 * MIN_STRIKE_DEPTH_DELTA elsewhere in this file was. */
#define PITCH_BEND_DEADZONE_COSINE_DELTA 0.03f

/* EMA smoothing on the cosine signal itself. Lowered from an initial
 * 0.35 (the same starting value AFTERTOUCH_SMOOTHING_ALPHA above still
 * uses) specifically for the same "very jittery" real feedback the
 * deadzone above addresses -- a smaller alpha weighs each new raw sample
 * less heavily against the running average, trading a little
 * responsiveness for meaningfully more noise rejection on a continuous,
 * held signal (unlike velocity's one-shot transient measurement, which
 * smoothing would only blunt, not clean up). */
#define PITCH_BEND_SMOOTHING_ALPHA 0.15f

typedef enum {
    PAD_STATE_IDLE = 0,
    PAD_STATE_AWAITING_STRIKE,
    PAD_STATE_NOTE_ON,
} pad_expr_state_t;

typedef struct {
    pad_expr_state_t state;
    uint32_t touch_start_ms;

    /* Hall sample clock reference (same to_ms_since_boot() clock as Hall
     * sample timestamps, so directly comparable) strike_time_ms below
     * measures elapsed time from. Set the instant AWAITING_STRIKE
     * begins. */
    uint32_t touch_start_sample_ms;

    /* Highest RAW depth (tiles_hall_get_depth(), NOT a delta from any
     * per-touch reference) seen at any point since touch began,
     * including the very first reading captured the instant touch was
     * detected. Real feedback, two rounds: "strong hard presses don't
     * trigger anything" (fixed by tracking the peak instead of the
     * instantaneous value, since a fast strike can spring back down
     * before a check made *right now* would still see it past
     * threshold), then "sudden full force press is not triggering the
     * notes... touch is detected... just no midi" -- a real debug
     * capture showed the depth reading *at the instant touch was first
     * detected* already sitting at 880-1040 (essentially full mechanical
     * compression, out of the ~900-1184 full-press range) for several
     * failed hits: for a hard enough strike, the entire compression can
     * complete faster than capacitive touch detection catches up, so by
     * the time software sees "touched," the press already happened.
     * An earlier version of this tracking subtracted a per-touch
     * "reference depth" captured at touch-down, meaning even an
     * already-fully-compressed initial reading started its own delta at
     * 0 -- discarding exactly the information needed to recognize "this
     * already happened." hall.c's depth is already baseline-relative
     * (drift-compensated for untouched pads via its own background
     * tracker -- see hall.c), so there was never a need for a *second*,
     * per-touch reference on top of it; comparing the raw peak directly
     * against MIN_STRIKE_DEPTH_DELTA handles both a strike that develops
     * gradually after touch begins and one that had already finished
     * before touch was even detected. */
    float peak_depth;

    /* Set exactly once, the moment peak_depth first crosses
     * MIN_STRIKE_DEPTH_DELTA -- the elapsed time (Hall sample clock)
     * between touch_start_sample_ms and that crossing, which
     * velocity_from_strike_time() maps to a MIDI velocity. Naturally
     * comes out as ~0 (max velocity) when the very first reading at
     * touch-down was already past threshold -- correct: that reading
     * means the strike was already essentially instantaneous. See this
     * file's "Velocity: elapsed-time-to-actuation" section for why this
     * replaced an acceleration estimate. threshold_crossed guards the
     * one-time capture (a later, larger peak shouldn't overwrite the
     * timing of when the strike was first detected). */
    uint32_t strike_time_ms;
    bool threshold_crossed;

    uint32_t last_seen_sample_time_ms;

    /* When the current note actually fired -- gates RETRIGGER_ARM_
     * DEPTH_DELTA below with a short grace period (RETRIGGER_GRACE_MS)
     * so a fast strike's own post-impact rebound (Hall depth springing
     * back toward baseline within a few ms of the note firing, before
     * the player has done anything else) doesn't immediately read as a
     * deliberate release-and-retrigger. */
    uint32_t note_on_ms;

    /* Exponential moving average of depth, feeding aftertouch only --
     * see AFTERTOUCH_SMOOTHING_ALPHA above. Seeded (not zeroed) at
     * note-on so aftertouch doesn't start with an artificial ramp-up
     * from 0. */
    float smoothed_depth;

    /* Cached at note-on and reused for aftertouch/note-off, so a live
     * scale change mid-hold (once scale switching exists) can't send
     * note-off for a different note than was turned on -- a stuck note
     * otherwise. */
    uint8_t active_note;
    uint8_t last_sent_aftertouch; /* 0xFF = force the first send */

    /* Bridges a brief capacitive touch dropout -- see
     * TOUCH_DROPOUT_GRACE_MS's own comment for why this exists. Updated
     * to the current time on every scan where the RAW touch reading is
     * true; last_touched_valid guards the very first touch ever seen on
     * this pad (before it's true, last_touched_ms is meaningless, not
     * "a long time ago" -- an unguarded check right after boot would
     * otherwise read as still-touched for the first few ms). */
    uint32_t last_touched_ms;
    bool last_touched_valid;
} pad_expr_t;

static pad_expr_t s_pads[TILES_NUM_PADS];

/* Pitch bend is module-level, not per-pad, because -- see this file's
 * "Pitch bend from sideways motion" section -- only ever one pad "owns"
 * the single shared MIDI channel's bend at a time; there is no
 * meaningful per-pad bend state to keep for a pad that isn't the
 * current owner. */
static bool s_pitch_bend_enabled;
static uint8_t s_pitch_bend_owner_pad; /* 0 = no owner */
static float s_pitch_bend_baseline_cosine;
static float s_pitch_bend_smoothed_cosine;
static uint16_t s_pitch_bend_last_sent;

/* "Expression mute" -- services/expression_control.h's circle+square
 * 3-second combo hold. A hard kill switch for pitch bend and poly
 * aftertouch, deliberately separate from s_pitch_bend_enabled above
 * (that's the player's own on/off preference; this overrides it
 * entirely, on top, without disturbing what it was set to) -- unmuting
 * restores exactly whatever tiles_expression_toggle_pitch_bend() state
 * was already in effect before muting. Note-on/off/velocity are read
 * directly from touch+Hall, never gated by this flag -- see
 * tiles_expression_set_muted()'s own comment. */
static bool s_expression_muted;

void tiles_expression_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pads[i] = (pad_expr_t){0};
        s_pads[i].state = PAD_STATE_IDLE;
    }
    s_pitch_bend_enabled = false;
    s_pitch_bend_owner_pad = 0u;
    s_pitch_bend_last_sent = PITCH_BEND_CENTER;
    s_expression_muted = false;
}

/* See this file's "Pitch bend from sideways motion" section for the
 * physics reasoning -- a direction cosine of the raw sample's X
 * component relative to the total field magnitude, invariant (to first
 * order) to Z depth for a fixed real lateral tilt. */
static float hall_x_direction_cosine(int16_t x, int16_t y, int16_t z) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    float magnitude = sqrtf(fx * fx + fy * fy + fz * fz);
    if (magnitude < 1.0f) {
        /* Guards a near-zero field reading (shouldn't happen with a real
         * magnet present) from a divide-by-near-zero blowing the ratio
         * up -- reads as "no lateral information yet" rather than noise. */
        return 0.0f;
    }
    return fx / magnitude;
}

/* Maps a cosine delta (current direction cosine minus this note's
 * baseline) to the 14-bit MIDI pitch bend wire value -- see
 * s_pitch_bend_max_cosine_deviation's own comment. Deadzone applied as a
 * "soft knee," not a hard cutoff -- see PITCH_BEND_DEADZONE_COSINE_
 * DELTA's own comment for why: within the deadzone, output is exactly
 * centered; just past it, output ramps continuously from 0 rather than
 * jumping straight to some nonzero value, and still reaches full swing
 * at exactly the same real deviation (s_pitch_bend_max_cosine_deviation)
 * as before the deadzone existed. */
static uint16_t pitch_bend_14bit_from_cosine_delta(float delta) {
    float magnitude = fabsf(delta);
    float sign = (delta < 0.0f) ? -1.0f : 1.0f;
    if (magnitude <= PITCH_BEND_DEADZONE_COSINE_DELTA) {
        magnitude = 0.0f;
    } else {
        magnitude -= PITCH_BEND_DEADZONE_COSINE_DELTA;
    }
    float usable_range = s_pitch_bend_max_cosine_deviation - PITCH_BEND_DEADZONE_COSINE_DELTA;
    if (usable_range < 0.01f) {
        /* Guards against services/expression_control.h's sub-menu (row
         * 2) ever being tuned to a sensitivity at or below the deadzone
         * itself, which would otherwise divide by zero or a negative
         * range -- floors to a narrow but well-defined usable range
         * instead of clamping the sub-menu's own values. */
        usable_range = 0.01f;
    }
    float normalized = sign * (magnitude / usable_range);
    if (normalized > 1.0f) {
        normalized = 1.0f;
    }
    if (normalized < -1.0f) {
        normalized = -1.0f;
    }
    int32_t bend = (int32_t)PITCH_BEND_CENTER + (int32_t)(normalized * 8191.0f);
    if (bend < 0) {
        bend = 0;
    }
    if (bend > 16383) {
        bend = 16383;
    }
    return (uint16_t)bend;
}

/* Real feedback: "when you press sentia button once it turns on and off
 * the pitch bend" -- called by services/expression_control.h on a
 * genuine square ("sentia") short click. Turning it off while a note
 * currently owns the bend resets to center immediately rather than
 * leaving that note stuck bent. */
void tiles_expression_toggle_pitch_bend(void) {
    s_pitch_bend_enabled = !s_pitch_bend_enabled;
    printf("[expression] pitch bend %s\n", s_pitch_bend_enabled ? "enabled" : "disabled");
    if (!s_pitch_bend_enabled && s_pitch_bend_owner_pad != 0u) {
        tiles_midi_send_pitch_bend(PITCH_BEND_CENTER);
        s_pitch_bend_last_sent = PITCH_BEND_CENTER;
        s_pitch_bend_owner_pad = 0u;
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

void tiles_expression_set_aftertouch_sensitivity(uint16_t depth_full_scale) {
    /* Never 0 -- aftertouch_from_depth() divides by this. */
    s_depth_to_aftertouch_full_scale = depth_full_scale > 0u ? depth_full_scale : 1u;
    printf("[expression] aftertouch sensitivity (full-scale depth) now %u\n", s_depth_to_aftertouch_full_scale);
}

void tiles_expression_set_muted(bool muted) {
    s_expression_muted = muted;
    printf("[expression] muted=%d\n", (int)s_expression_muted);
    if (muted && s_pitch_bend_owner_pad != 0u) {
        /* Same "never leave a note stuck bent" rule
         * tiles_expression_toggle_pitch_bend() already follows -- reset
         * to center immediately rather than waiting for that note's own
         * release/retrigger to clear it. */
        tiles_midi_send_pitch_bend(PITCH_BEND_CENTER);
        s_pitch_bend_last_sent = PITCH_BEND_CENTER;
        s_pitch_bend_owner_pad = 0u;
    }
}

static void begin_awaiting_strike(pad_expr_t *s, uint8_t pad, uint32_t now_ms) {
    s->state = PAD_STATE_AWAITING_STRIKE;
    s->touch_start_ms = now_ms;
    s->touch_start_sample_ms = now_ms;
    s->last_seen_sample_time_ms = 0;

    /* Checked immediately, not just seeded -- see peak_depth's own
     * comment: for a hard enough strike, compression can already be
     * complete by the time touch is detected at all, and that has to
     * count as an instant (max-velocity) strike, not "not pressed yet". */
    float initial_depth = (float)tiles_hall_get_depth(pad);
    s->peak_depth = initial_depth;
    s->threshold_crossed = (initial_depth >= MIN_STRIKE_DEPTH_DELTA);
    s->strike_time_ms = 0;
}

/* Maps elapsed strike time (ms, touch_start_sample_ms to the moment
 * peak_depth crossed MIN_STRIKE_DEPTH_DELTA) to a MIDI velocity -- see
 * this file's "Velocity: elapsed-time-to-actuation" section for the
 * full reasoning. Faster (smaller ms) is a harder strike. */
static uint8_t velocity_from_strike_time(uint32_t strike_time_ms) {
    if (strike_time_ms <= STRIKE_TIME_MAX_VELOCITY_MS) {
        return 127u;
    }
    if (strike_time_ms >= STRIKE_TIME_MIN_VELOCITY_MS) {
        return (uint8_t)MIN_VELOCITY;
    }
    /* Power curve, not linear -- normalized in (0, 1) as "how much of
     * the way from slow to fast," exponent > 1 suppresses the low
     * (slow) end relative to a straight line. */
    float normalized = (float)(STRIKE_TIME_MIN_VELOCITY_MS - strike_time_ms) /
                        (float)(STRIKE_TIME_MIN_VELOCITY_MS - STRIKE_TIME_MAX_VELOCITY_MS);
    float curved = powf(normalized, VELOCITY_CURVE_EXPONENT);
    int vel = (int)((float)MIN_VELOCITY + (float)(127u - MIN_VELOCITY) * curved);
    if (vel < (int)MIN_VELOCITY) {
        vel = (int)MIN_VELOCITY;
    }
    if (vel > 127) {
        vel = 127;
    }
    return (uint8_t)vel;
}

static uint8_t aftertouch_from_depth(uint16_t depth) {
    uint32_t scaled = ((uint32_t)depth * 127u) / s_depth_to_aftertouch_full_scale;
    return (uint8_t)(scaled > 127u ? 127u : scaled);
}

/* Claims pitch bend ownership for `pad` at the moment its note fires --
 * see this file's "Pitch bend from sideways motion" section. If a
 * DIFFERENT pad currently owns the bend (still held from an earlier
 * strike), resets to center first so this new note doesn't inherit its
 * offset. No-op if pitch bend is disabled or expression mute is active. */
static void claim_pitch_bend_owner(uint8_t pad) {
    if (!s_pitch_bend_enabled || s_expression_muted) {
        return;
    }
    if (s_pitch_bend_owner_pad != 0u && s_pitch_bend_owner_pad != pad) {
        tiles_midi_send_pitch_bend(PITCH_BEND_CENTER);
    }
    s_pitch_bend_owner_pad = pad;
    tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
    s_pitch_bend_baseline_cosine = hall_x_direction_cosine(hs.x, hs.y, hs.z);
    s_pitch_bend_smoothed_cosine = s_pitch_bend_baseline_cosine;
    s_pitch_bend_last_sent = PITCH_BEND_CENTER;
}

/* Releases pitch bend ownership if `pad` currently holds it, resetting
 * to center so the note ending doesn't leave a stale bend applied to
 * whatever else might still be held (or to nothing at all). Safe to
 * call unconditionally on every note-off/retrigger, enabled or not. */
static void release_pitch_bend_owner_if(uint8_t pad) {
    if (s_pitch_bend_owner_pad == pad) {
        tiles_midi_send_pitch_bend(PITCH_BEND_CENTER);
        s_pitch_bend_last_sent = PITCH_BEND_CENTER;
        s_pitch_bend_owner_pad = 0u;
    }
}

void tiles_expression_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        pad_expr_t *s = &s_pads[i];

        bool raw_touched = tiles_touch_is_touched(pad);
        if (raw_touched) {
            s->last_touched_ms = now_ms;
            s->last_touched_valid = true;
        }
        /* See TOUCH_DROPOUT_GRACE_MS's own comment -- bridges a brief
         * real capacitive dropout so it doesn't read as a full release. */
        bool touched =
            raw_touched || (s->last_touched_valid && (now_ms - s->last_touched_ms) < TOUCH_DROPOUT_GRACE_MS);

        if (s->state == PAD_STATE_IDLE) {
            /* services/expression_control.h's sub-menu (circle+square
             * held) claims the pad grid for its own slider taps -- a
             * fresh touch while it's showing must never also start a
             * real strike underneath. A pad already past IDLE when the
             * sub-menu opens is deliberately left alone (see the loop
             * below), only a brand-new touch is suppressed here. */
            if (touched && !tiles_expression_control_owns_pad_grid()) {
                begin_awaiting_strike(s, pad, now_ms);
                /* Touch-only haptic acknowledgment, independent of
                 * whether this ever becomes a real press -- see
                 * tiles_haptics_trigger_touch_pulse()'s own comment. */
                tiles_haptics_trigger_touch_pulse(pad);
            }
            continue;
        }

        if (s->state == PAD_STATE_AWAITING_STRIKE) {
            /* Only read a fresh Hall sample while still touched -- once
             * released there's nothing new to gather, the commit
             * decision below just uses whatever peak/timing was already
             * measured. */
            if (touched) {
                tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
                if (hs.valid && hs.sample_time_ms != s->last_seen_sample_time_ms) {
                    s->last_seen_sample_time_ms = hs.sample_time_ms;
                    float depth = (float)tiles_hall_get_depth(pad);
                    if (depth > s->peak_depth) {
                        s->peak_depth = depth;
                    }
                    if (!s->threshold_crossed && s->peak_depth >= MIN_STRIKE_DEPTH_DELTA) {
                        /* First sample to cross the actuation threshold --
                         * see strike_time_ms's own comment. */
                        s->threshold_crossed = true;
                        s->strike_time_ms = hs.sample_time_ms - s->touch_start_sample_ms;
                    }
                }
            }

            /* Gated on the PEAK depth reached, not the current instant --
             * see peak_depth's own comment for why: a fast, percussive
             * strike can spring back (or end touch) before a reading
             * taken *right now* would still show it past threshold,
             * which silently lost real hard strikes before this fix
             * ("strong hard presses don't trigger anything"). */
            bool pressed = s->threshold_crossed;

            /* "ready": a real press has been measured -- fires the
             * instant it's detected, however fast or slow that took, no
             * fixed elapsed-ms floor and no waiting for more samples (see
             * this file's velocity section for why that wait is gone).
             * "commit_on_release": touch already ended, but a real press
             * was measured before it did -- commit now rather than
             * discarding a genuine hit just because contact happened to
             * end first (a real, common shape for a fast percussive
             * strike). */
            bool ready = touched && pressed;
            bool commit_on_release = !touched && pressed;

            if (ready || commit_on_release) {
                s->active_note = tiles_note_map_get_note(pad);
                uint8_t velocity = velocity_from_strike_time(s->strike_time_ms);
                /* Temporary bring-up visibility -- prints exactly what
                 * the commit decision was based on, so a real-hardware
                 * session can read off actual numbers instead of
                 * guessing constants blind. Replace with a real
                 * usb_vendor/ diagnostics stream once that exists, same
                 * reasoning as touch.c/standby.c's own temporary
                 * prints. */
                printf("[expression] pad %u note-on: %s, peak_depth=%d strike_time_ms=%u velocity=%u\n", pad,
                       commit_on_release ? "commit_on_release" : "ready", (int)s->peak_depth, s->strike_time_ms,
                       velocity);
                tiles_midi_note_on(s->active_note, velocity);
                /* Same velocity value driving both -- "mapped to the
                 * velocity curve by default" means the kick and the MIDI
                 * note agree exactly, not two independent estimates. */
                tiles_haptics_trigger_kick(pad, velocity);
                s->last_sent_aftertouch = 0xFFu;
                /* Seed the smoother with the real depth right now rather
                 * than 0 -- see the field's own comment. */
                s->smoothed_depth = (float)tiles_hall_get_depth(pad);
                s->note_on_ms = now_ms;
                s->state = PAD_STATE_NOTE_ON;
                claim_pitch_bend_owner(pad);
                continue;
            }

            if (!touched) {
                /* Released without ever measuring a real press -- a
                 * light tap, not an actual key motion. No note was ever
                 * sent. Temporary bring-up visibility: real feedback
                 * ("sudden full force press is not triggering the
                 * notes") described touches that looked and felt like
                 * real hard strikes but produced nothing, with no way to
                 * tell from the existing note-on-only print whether
                 * depth genuinely never moved or something else is
                 * wrong -- this print exists specifically to answer
                 * that on the next capture. */
                printf("[expression] pad %u cancelled (no press): peak_depth=%d touch_duration_ms=%u\n", pad,
                       (int)s->peak_depth, now_ms - s->touch_start_ms);
                s->state = PAD_STATE_IDLE;
            }
            continue;
        }

        /* PAD_STATE_NOTE_ON */
        if (!touched) {
            tiles_midi_note_off(s->active_note);
            tiles_haptics_stop(pad);
            release_pitch_bend_owner_if(pad);
            s->state = PAD_STATE_IDLE;
            continue;
        }

        float raw_depth = (float)tiles_hall_get_depth(pad);

        /* Retrigger without a full release -- real feedback: "contact
         * with pad has to be broken for retrigger, that's bad." Once raw
         * depth has eased back down close to true rest (not just down
         * from this note's own peak -- see RETRIGGER_ARM_DEPTH_DELTA),
         * treat it exactly like touch had been released and retouched:
         * send note-off for the held note and drop back into
         * strike-detection, all without touch itself ever going false.
         * A subsequent real press is then measured and fires a
         * brand-new note-on with its own freshly computed velocity
         * through the exact same path as any other strike. */
        if ((now_ms - s->note_on_ms) >= RETRIGGER_GRACE_MS && raw_depth <= RETRIGGER_ARM_DEPTH_DELTA) {
            tiles_midi_note_off(s->active_note);
            tiles_haptics_stop(pad);
            release_pitch_bend_owner_if(pad);
            begin_awaiting_strike(s, pad, now_ms);
            continue;
        }

        /* EMA toward this scan's raw depth -- both a strengthening press
         * (more pressure past the strike) and an easing-off one (less
         * pressure, still touching) move it, smoothly. */
        s->smoothed_depth += AFTERTOUCH_SMOOTHING_ALPHA * (raw_depth - s->smoothed_depth);

        uint8_t at = aftertouch_from_depth((uint16_t)s->smoothed_depth);
        if (at != s->last_sent_aftertouch) {
            s->last_sent_aftertouch = at;
            /* "Expression mute" (services/expression_control.h) silences
             * poly aftertouch specifically -- basic note-on/off/velocity
             * above are unaffected. tiles_haptics_set_sustain_level()
             * doesn't need a matching guard here: haptics.c's own mute
             * flag already makes it a no-op (see tiles_haptics_set_muted). */
            if (!s_expression_muted) {
                tiles_midi_send_poly_aftertouch(s->active_note, at);
            }
            tiles_haptics_set_sustain_level(pad, at);
        }

        /* Pitch bend: only the current owner pad drives the shared
         * channel -- see this file's "Pitch bend from sideways motion"
         * section. pad == 0 never matches a real logical pad, so this
         * is naturally a no-op both while disabled and for every pad
         * that isn't the owner, with no separate enabled check needed. */
        if (pad == s_pitch_bend_owner_pad) {
            tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
            if (hs.valid) {
                float cosine = hall_x_direction_cosine(hs.x, hs.y, hs.z);
                s_pitch_bend_smoothed_cosine += PITCH_BEND_SMOOTHING_ALPHA * (cosine - s_pitch_bend_smoothed_cosine);
                uint16_t bend =
                    pitch_bend_14bit_from_cosine_delta(s_pitch_bend_smoothed_cosine - s_pitch_bend_baseline_cosine);
                if (bend != s_pitch_bend_last_sent) {
                    s_pitch_bend_last_sent = bend;
                    tiles_midi_send_pitch_bend(bend);
                }
            }
        }
    }
}
