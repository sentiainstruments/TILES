#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "midi_out.h"
#include "haptics.h"
#include "expression_control.h"
#include "game_mode.h"
#include "octave_control.h"
#include "op_mode.h"

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
 * to travel before its speed even gets measured.
 * History: raised 150 -> 300 after real feedback that a fast-but-shallow
 * flick still read as a hard strike ("when I press faster but not deep
 * the reading is still strong"), reasoning that requiring more real
 * travel before triggering at all would mean only genuinely committed
 * force could cross it quickly. That traded away something this
 * section's own comment explicitly flagged as a risk at the time
 * ("revisit... if deliberate soft presses stop registering") --  exactly
 * what happened: real feedback again, "reduce the deadzone before
 * velocity picks up on pad pressed, rn we cant play lightly enough." A
 * genuinely light, SLOW, deliberate press has just as little depth as a
 * fast shallow flick, and 300 was rejecting both alike -- no note at
 * all, regardless of how deliberately or gently it was played, which is
 * a worse outcome than the fast-flick misread this was trying to avoid.
 * Restored to 150 -- the ONE value this whole section's own real capture
 * data (140 real touches) actually validated as the line between
 * incidental contact (~96 ceiling) and a genuine press (~192 floor);
 * 300 was a guess layered on top of that real data, not itself measured
 * against it. The elapsed-time velocity model (below) still
 * distinguishes fast strikes from slow ones at this lower threshold same
 * as it always did -- a genuinely fast-but-shallow flick will still read
 * as a quick, therefore harder-mapped, strike (matching how real
 * velocity-sensitive keybeds already work: speed of travel IS the
 * standard velocity signal, not a bug), while a slow, light press now
 * finally gets to register at all instead of being silently dropped. */
#define MIN_STRIKE_DEPTH_DELTA 150.0f

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
 * them, VELOCITY_CURVE_EXPONENT shapes the curve: real feedback first
 * asked for the acoustic-instrument feel a exponent > 1 gives (1.8,
 * suppressing the low/slow end relative to a straight line so a merely-
 * adequate-speed press reads noticeably quieter than a confidently fast
 * one) -- then, after trying it: "make velocity curve and aftertouch
 * less steep. more gradual for soft detection better." Suppressing the
 * low end is exactly what made soft detection worse: d(curved)/d(time)
 * is smallest right where slow/soft strikes live, compressing a wide
 * range of genuinely different soft touches into a narrow band near
 * MIN_VELOCITY with little to no felt difference between them. Dropped
 * to 1.0 -- a plain linear response, equal sensitivity across the whole
 * speed range, no low-end suppression at all. Both constants are still
 * first attempts, not measured against real strikes -- there's no
 * equivalent captured data yet for "how many ms does a hard strike
 * actually take to cross MIN_STRIKE_DEPTH_DELTA on this hardware,"
 * unlike the depth-delta numbers above. The `[expression]` print below
 * now reports strike_time_ms directly on every commit specifically so
 * the next real-hardware session can calibrate these three constants
 * from real numbers instead of guessing a third velocity model. */
#define STRIKE_TIME_MAX_VELOCITY_MS 10u
#define STRIKE_TIME_MIN_VELOCITY_MS 150u
#define VELOCITY_CURVE_EXPONENT 1.0f

/* Even a strike weak enough to barely clear MIN_STRIKE_DEPTH_DELTA
 * should produce an audible note, not near-silence -- the curve above
 * can push a very slow qualifying strike's raw output below this, so
 * it's still clamped up to a floor rather than left near-silent. */
#define MIN_VELOCITY 8u

/* Real calibration data, not a placeholder -- now from TWO sessions on
 * TWO different physical units, which don't agree, and the newer one
 * wins. An earlier unit's serial capture session (diagnostics/
 * calibration.h's 'f' command, all 24 magnets seated, a normal regular
 * full press -- which bottomed that unit's mechanical travel, no further
 * "harder" position existed) measured |raw Z - rest baseline| = 784 to
 * 1184, average 918; 900 was picked from that. Unit 2's own later
 * session (diagnostics/README.md's own capture-session entry) measured
 * a genuine STRONG STRIKE, not a "regular full press," across 4 sampled
 * corner pads: 1697, 1488, 1328, 1280, average ~1448 -- 60% higher.
 * Real feedback afterward: "make velocity curve and aftertouch less
 * steep. more gradual for soft detection better." Leaving full-scale at
 * 900 against unit 2's real ~1450 ceiling means aftertouch pegs at 127
 * well before a real hard press's actual travel is used, which reads as
 * steep/twitchy -- little room for gradual continued-pressure
 * expression once it's already maxed out. Raised to 1450, unit 2's own
 * real average, so the full 0-127 aftertouch sweep uses this unit's
 * real strike range instead of an older, softer-reading unit's. Still a
 * single shared constant across all 24 pads, not a real per-pad curve
 * (explicitly out of V1 scope -- see hall.h) -- and still just the
 * average of 4 sampled pads on ONE unit, not all 24; revisit if the
 * remaining 20 pads (or the other 3 units) turn out meaningfully
 * different once fully sampled.
 *
 * Runtime, not a fixed #define, so services/expression_control.h's
 * sub-menu (row 4, aftertouch sensitivity) can adjust it live via
 * tiles_expression_set_aftertouch_sensitivity() below. Defaults to
 * exactly this same calibrated 1450 value. */
static uint16_t s_depth_to_aftertouch_full_scale = 1450u;

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
 * services/expression_control.c's square-button short-click handling) --
 * that's a single on/off PREFERENCE, separate from the genuinely
 * per-note MECHANICS below.
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
 * joysticks use to derive tilt independent of plunger depth. See
 * hall_xy_and_magnitude()/direction_cosine_from() below for the actual
 * computation, and PITCH_BEND_SETTLE_MS/the vertical-pressure
 * compensation further below for how the baseline this compares against
 * is captured and corrected -- real hardware showed the theory above
 * doesn't hold PERFECTLY in practice.
 *
 * Both in-plane axes (X and Y) now feed the bend, not X alone -- real
 * feedback: "incorporate the 2 axis tilt onto the pitch bend to provide
 * a more strong reading of tilt... more sable reeds... make vibratos."
 * Originally X alone: this project has no hardware documentation on
 * which local Hall axis corresponds to which physical direction on a
 * mounted pad, so X was picked as "sideways" somewhat arbitrarily, and Y
 * was left unused entirely since MIDI pitch bend is inherently
 * one-dimensional (a single 14-bit value) and blending two axes into one
 * needed an actual design, not a guess. Two things argued for revisiting
 * that: any real physical tilt genuinely deflects the field in BOTH X
 * and Y to some degree (a magnetic dipole's off-axis response isn't
 * confined to one hardware axis just because the intended playing
 * gesture is), so X-only was discarding real, correlated signal, not
 * just noise -- and a vibrato specifically needs SMALL, RAPID wiggles to
 * register reliably, exactly the amplitude range where a single axis's
 * own noise floor matters most. See hall_xy_and_magnitude()/
 * direction_cosine_from() below for the combined computation: MAGNITUDE
 * comes from both axes' compensated deviation (sqrt(dx^2 + dy^2), a
 * strictly stronger/less noisy reading of "how far off center" than X
 * alone, regardless of exactly which direction a real tilt or wiggle
 * leans in), while SIGN/polarity stays anchored to X alone, preserving
 * the already-tuned left/right bend-direction feel this file's deadzone
 * and sensitivity constants were calibrated against. This deliberately
 * does NOT attempt true 2D vibrato (bend direction tied to whichever way
 * the finger actually wiggles) -- that needs a real 2D bend axis with no
 * established precedent here yet; this is the minimal change that makes
 * an ordinary X-tilt wiggle read as a stronger, more reliable signal.
 *
 * Genuinely per-note now, not a workaround: real feedback: "we need to
 * make sure we have individual per note pitch bend not just regular all
 * key pitch bend. like the roli seaboard." Pitch Bend Change is a
 * channel-wide MIDI message with no per-note addressing in the spec
 * itself, so independent per-note bend was never possible on this
 * project's old single MIDI channel -- earlier versions of this section
 * documented a single-"owner"-pad workaround for exactly that
 * limitation. midi/midi_out.h now implements real MPE (MIDI Polyphonic
 * Expression): every currently-held note gets its own MIDI channel (see
 * claim_mpe_channel()/end_held_note() below, this file's own per-pad
 * voice-management layer on top of that wire-protocol support), so
 * there is no more shared state to arbitrate -- pitch_bend_* fields live
 * directly on each pad's own pad_expr_t, and multiple pads can each
 * bend independently at the same time, exactly like a real Seaboard.
 * The old "reset to center before handing off" concern doesn't
 * disappear entirely -- it becomes "always leave a freed MPE channel
 * centered before it's reused by a different note," which
 * end_held_note() below is the single place that guarantees. */
#define PITCH_BEND_CENTER 8192u

/* NOTE on everything calibrated below: these constants (deadzone,
 * sensitivity, smoothing, ARM timing) were all captured/tuned against
 * the OLD signal -- current cosine compared against a FIXED baseline
 * captured once at note-on. The vertical-pressure compensation added
 * alongside MPE (see pitch_bend_14bit_from_cosine_delta()'s own comment
 * further below) changes what's actually being measured: the baseline
 * is now re-derived from the CURRENT depth every tick instead of staying
 * fixed, which should substantially reduce exactly the press-depth-
 * correlated component these captures were fighting. That means these
 * specific numbers are likely stale again and worth a fresh capture
 * round against the corrected signal rather than assumed still-correct
 * -- kept as a reasonable starting point, not re-guessed blind on top
 * of an already-changed signal.
 *
 * Cosine delta (see the section comment above) that maps to the full
 * +/-8191 MIDI range. Unmeasured -- there is no captured real-hardware
 * data yet for how much a deliberate sideways push actually moves this
 * ratio on this board's magnet/sensor geometry, unlike the depth-based
 * constants elsewhere in this file.
 *
 * History: 0.15 ("very jittery... not as sensitive") -> 0.30 -> 0.20,
 * paired across those rounds with an ever-more-elaborate stack of
 * deadzone/depth-compensation/confirm/acceleration logic trying to
 * distinguish noise from intent entirely downstream of the raw signal.
 * Real feedback after that stack still produced "so jittery at rest and
 * at the same time it requires too much tilt to register that it might
 * break the keys" -- both complaints AT ONCE is a real signal the
 * layered-workarounds approach had reached diminishing returns: each
 * round's fix for one symptom was fighting the previous round's fix for
 * the other. Reset back to 0.15 and a much simpler pipeline (see
 * PITCH_BEND_SETTLE_MS below for what actually replaced most of that
 * stack) rather than continuing to add more compensating layers on top
 * of ones that weren't clearly working.
 *
 * Runtime, not a fixed #define, so services/expression_control.h's
 * sub-menu (row 2, pitch bend sensitivity) can adjust it live via
 * tiles_expression_set_pitch_bend_sensitivity() below -- a SMALLER value
 * here means MORE sensitive (less real motion needed to reach full
 * bend).
 *
 * Recalibrated from TWO real captures, cross-referenced against each
 * other -- real feedback after the deadzone-only recalibration below:
 * "not pitch bending consistently... requires some extreme bend for it
 * to happen." A second capture, this time of a real DELIBERATE sideways
 * tilt (comfortable, not extreme force) held on a struck pad: 634 sent
 * deltas, min 0.0323, median 0.0526, p75 0.0573, p90 0.0626, p95 0.0656,
 * max 0.0851. Compared directly against the at-rest capture
 * (PITCH_BEND_DEADZONE_COSINE_DELTA's own comment): the OLD default here
 * (0.15) was more than double the highest deliberate-tilt sample ever
 * observed -- a real deliberate push on this hardware simply never gets
 * anywhere close to it, which is exactly "requires extreme bend." Reset
 * to 0.065 -- comfortably above the deadzone (0.04, see below) so the
 * bulk of a real tilt (median 0.0526 up through p90 0.0626) covers
 * roughly half to full swing, while the single strongest sample (0.0851)
 * simply clips at full scale, same as pushing harder than needed on any
 * other control. */
static float s_pitch_bend_max_cosine_deviation = 0.065f;

/* Small deltas this close to baseline are treated as exactly centered
 * (no bend at all) rather than passed through -- applied as a "soft
 * knee" in pitch_bend_14bit_from_cosine_delta() below (subtracted from
 * the magnitude before normalizing, not a hard cutoff-then-jump) so bend
 * still ramps continuously from zero just past this threshold rather
 * than snapping straight to some nonzero value the instant it's crossed.
 *
 * Real data, not a guess, for once: a debug-console capture of the
 * `[expression] pitch bend sent` print below (added specifically because
 * this constant had been guessed blind through several prior rounds)
 * during several seconds of a real, ordinary straight-down press with no
 * intentional tilt -- 1070 sent deltas, median 0.0257, p90 0.0373, p95
 * 0.0409, p99 0.051, max 0.0945. This confirms what earlier rounds only
 * speculated: pressing straight down really does move this ratio
 * substantially on real hardware (the on-axis-magnet assumption in this
 * file's own physics section doesn't fully hold for this board's real
 * assembly), not just quantization noise.
 *
 * Recalibrated a second time, cross-referenced against a matching
 * DELIBERATE-tilt capture (s_pitch_bend_max_cosine_deviation's own
 * comment) -- real feedback that the first recalibration (0.045) was
 * "not pitch bending consistently," i.e. rejecting some genuine tilt
 * along with the noise. Comparing what fraction of EACH distribution a
 * given threshold rejects (not just each one's own percentiles in
 * isolation) found a much cleaner separation point than either capture
 * suggested alone: at 0.04, 94% of at-rest samples are rejected while
 * only 0.6% of real deliberate-tilt samples are lost; the previous 0.045
 * only gained 3 more points of rest-noise rejection (97%) at the cost of
 * losing 5.6% of real tilt signal -- a bad trade once both sides of the
 * tradeoff were actually visible together. Lowered to 0.04 accordingly. */
#define PITCH_BEND_DEADZONE_COSINE_DELTA 0.04f

/* How long AFTER claiming ownership before the baseline cosine is
 * actually captured, letting PITCH_BEND_SMOOTHING_ALPHA's EMA settle
 * for a few ticks first instead of trusting one raw, instantaneous
 * sample -- likely the real fix for "so jittery at rest," more directly
 * than any amount of downstream deadzone tuning could be. A single
 * baseline sample captured at the exact, often percussive instant a
 * note fires is itself just as susceptible to the same raw-quantization
 * noise (see PITCH_BEND_DEADZONE_COSINE_DELTA's own comment) as any
 * later reading -- if THAT one sample happened to land off from the
 * pad's true rest tilt, every subsequent reading compares against a
 * baseline that's already wrong, which no amount of smoothing or
 * deadzone applied to the LIVE signal can fix, since the error lives in
 * the reference point itself, not the live samples. During this window
 * bend stays centered (see the NOTE_ON loop below) -- imperceptibly
 * short, well under a typical strike-to-hold transition. Unmeasured --
 * a first attempt at "long enough for the EMA to meaningfully settle,
 * short enough nothing else notices the delay." */
#define PITCH_BEND_SETTLE_MS 25u

/* Brief noise-transient filter -- real feedback wanted LESS tilt needed
 * to trigger a bend, and a big amplitude deadzone was the previous
 * round's way of also rejecting brief accidental wobbles, which fought
 * directly against that ask. A deviation past the deadzone ramps
 * linearly from 0 up to full weight over this many ms rather than
 * outputting at full strength the instant it's crossed -- a genuinely
 * brief, noisy blip never reads as more than a fraction of its already-
 * small magnitude before the next real sample either confirms or
 * clears it, while a real held tilt reaches full weight almost
 * immediately (this is deliberately short, NOT the multi-hundred-ms
 * "hold to arm" gate a previous round tried -- that version could
 * itself make USTABLE things worse: something persistently a little off
 * -- like an unsettled baseline -- would ramp up right along with a
 * genuine tilt, since a fixed offset looks identical to real intent to
 * a pure time-based filter. PITCH_BEND_SETTLE_MS above is what's meant
 * to actually fix a bad baseline; this is only for genuinely transient,
 * self-clearing noise). No acceleration/growth past full weight either,
 * for the same reason -- see this constant's own history above.
 * Unmeasured -- a first attempt at "smooths a blip, doesn't add a
 * perceptible lag to a real tilt." */
#define PITCH_BEND_ARM_MS 15u

/* EMA smoothing on the cosine signal itself. History: 0.35 -> 0.15 for
 * "very jittery" real feedback on the AT-REST case, which the deadzone
 * above (now recalibrated from real data) directly targets -- but real
 * feedback after that recalibration found the deadzone alone wasn't
 * enough: "not jittery on press anymore but jittery when pitch bend is
 * triggered." That's a DIFFERENT case the deadzone can't help with --
 * the deadzone only zeroes OUT small deltas near center, it does nothing
 * to smooth the noise riding on top of a delta that's already past it.
 * The same ongoing press-depth-correlated wobble the deadzone capture
 * measured at rest (median 0.0257, p90 0.0373 -- see that constant's own
 * comment) doesn't disappear once a real tilt is added on top of it; it
 * keeps contributing the same absolute jitter, which is proportionally
 * MORE noticeable at low-to-moderate bend amounts since nothing here
 * scales it down. Lowered further, 0.15 -> 0.08, to reject more of that
 * ongoing wobble on the engaged signal specifically -- a real,
 * deliberately held tilt (at least a couple hundred ms in practice)
 * still easily outlasts this filter's now-longer settling time; a
 * continuous quick wobble on top of it doesn't. Unmeasured -- a first
 * attempt at "smooths the wobble, doesn't read as mushy/laggy on a real
 * bend," not measured against real playing. */
#define PITCH_BEND_SMOOTHING_ALPHA 0.08f

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

    /* This pad's MPE Member Channel (status-byte nibble,
     * TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL..+NUM_MEMBER_CHANNELS-1) while
     * a note is held -- see claim_mpe_channel()/end_held_note() below.
     * Only meaningful while state == PAD_STATE_NOTE_ON. */
    uint8_t midi_channel;

    /* Per-note pitch bend -- one full independent set of state per pad,
     * not shared module-level state, now that MPE gives every held note
     * its own channel (see this file's "Pitch bend from sideways
     * motion" section for the full history of why this used to be a
     * single shared "owner pad"). pitch_bend_active records whether
     * pitch bend was actually enabled (and not muted) at the moment
     * THIS note fired -- toggling the feature mid-hold doesn't
     * retroactively add or remove bend from an already-sounding note,
     * matching the original single-owner version's behavior. */
    bool pitch_bend_active;
    /* Raw X and Y, settled -- see PITCH_BEND_SETTLE_MS's own comment.
     * These are what the vertical-pressure compensation re-derives an
     * expected baseline cosine from at the CURRENT depth every tick,
     * rather than comparing against one fixed baseline cosine the way
     * earlier rounds did -- see the main scan loop's own comment for the
     * full reasoning, including why Y joined X here. */
    float pitch_bend_baseline_x;
    float pitch_bend_baseline_y;
    /* Only used BEFORE baseline_settled, to arrive at a clean baseline
     * capture (see PITCH_BEND_SETTLE_MS) -- not read again afterward. */
    float pitch_bend_smoothed_x;
    float pitch_bend_smoothed_y;
    /* EMA of the already depth-compensated delta (real feedback: "you
     * broke mpe preassure... biased towards down it never goes up" --
     * see the main scan loop's own comment on why this replaced
     * smoothing the cosine itself). */
    float pitch_bend_smoothed_delta;
    bool pitch_bend_baseline_settled;
    uint32_t pitch_bend_claim_ms;
    uint16_t pitch_bend_last_sent;
    /* Run-tracking for PITCH_BEND_ARM_MS's brief noise-transient filter
     * -- see that constant's own comment. pitch_bend_run_active is false
     * whenever the (deadzone-adjusted) delta is currently within the
     * deadzone; pitch_bend_run_positive records which side of center the
     * current run is on, so a sign flip is treated as a brand-new run
     * rather than a continuation; pitch_bend_run_start_ms is when the
     * CURRENT run began. */
    bool pitch_bend_run_active;
    bool pitch_bend_run_positive;
    uint32_t pitch_bend_run_start_ms;
} pad_expr_t;

static pad_expr_t s_pads[TILES_NUM_PADS];

/* The player's own single on/off preference for pitch bend (see
 * tiles_expression_toggle_pitch_bend()) -- separate from each pad's own
 * per-note pitch_bend_active above, which latches whatever this was at
 * the moment that specific note fired. */
static bool s_pitch_bend_enabled;

/* Temporary bring-up visibility for a real calibration capture -- real
 * feedback: "the value of the 2 axis changes with pressure so we need to
 * compensate for that... your math is so off." The current compensation
 * (direction_cosine_from(), a ratio against magnitude) assumes X and Y
 * scale PROPORTIONALLY with the field magnitude under a pure depth
 * change with zero real tilt; real hardware evidently doesn't hold to
 * that closely enough. Fixing this properly needs an actual measured
 * X(depth)/Y(depth) relationship from this hardware, not another guessed
 * ratio -- this print exists to capture that: raw x, y, z, and depth
 * together, throttled globally (not per pad) so a single-pad capture
 * (press straight down repeatedly, no deliberate tilt at all, across the
 * full depth range) stays readable rather than flooding across whichever
 * other pads also happen to be held. */
static uint32_t s_hall_calibration_print_ms;
#define HALL_CALIBRATION_PRINT_INTERVAL_MS 40u

/* MPE Member Channel allocator -- one slot per Member Channel
 * (TILES_MIDI_MPE_NUM_MEMBER_CHANNELS of them), mirroring
 * services/haptics.c's own voice-stealing policy almost exactly
 * (oldest-claim-wins eviction via a monotonic sequence number) for the
 * same "ran out of a limited hardware/protocol resource, evict the
 * longest-held one rather than refuse the new one" reasoning -- see
 * claim_mpe_channel() below. Running out of 15 simultaneous independent
 * channels on a 24-pad board is a real possibility (unlike haptics'
 * ceiling, which is driven by power budget and typically much lower),
 * but still an edge case most sessions won't hit. */
typedef struct {
    bool in_use;
    uint8_t owner_pad; /* 1..TILES_NUM_PADS, valid only while in_use */
    uint32_t claim_seq;
} mpe_channel_slot_t;
static mpe_channel_slot_t s_mpe_channels[TILES_MIDI_MPE_NUM_MEMBER_CHANNELS];
static uint32_t s_next_mpe_claim_seq = 1u;

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
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        s_mpe_channels[i] = (mpe_channel_slot_t){0};
    }
    s_next_mpe_claim_seq = 1u;
    s_pitch_bend_enabled = false;
    s_expression_muted = false;
}

/* Splits a raw Hall sample into its X and Y components and total field
 * magnitude |B| = sqrt(x^2+y^2+z^2) -- kept as separate outputs (rather
 * than only returning a direction cosine, as an earlier X-only version of
 * this function did) because the vertical-pressure compensation in this
 * file's main scan loop needs BOTH the current magnitude and this pad's
 * settled baseline X/Y to re-derive an expected baseline cosine at the
 * CURRENT depth, for each axis -- see that loop's own comment for why,
 * and for why Y joined X here at all. */
static void hall_xy_and_magnitude(int16_t x, int16_t y, int16_t z, float *x_out, float *y_out, float *magnitude_out) {
    float fx = (float)x;
    float fy = (float)y;
    float fz = (float)z;
    *x_out = fx;
    *y_out = fy;
    *magnitude_out = sqrtf(fx * fx + fy * fy + fz * fz);
}

/* See this file's "Pitch bend from sideways motion" section for the
 * physics reasoning -- a direction cosine of a field component relative
 * to the total field magnitude, invariant (to first order) to Z depth
 * for a fixed real lateral tilt. Guards a near-zero magnitude (shouldn't
 * happen with a real magnet present) from a divide-by-near-zero blowing
 * the ratio up -- reads as "no lateral information yet" rather than
 * noise. */
static float direction_cosine_from(float x_component, float magnitude) {
    if (magnitude < 1.0f) {
        return 0.0f;
    }
    return x_component / magnitude;
}

/* See PITCH_BEND_ARM_MS's own comment for the full reasoning -- a plain
 * 0..1 ramp-in over PITCH_BEND_ARM_MS of a run, capped at 1.0 (no
 * growth beyond that -- see that constant's own comment for why the
 * previous round's further acceleration-past-1.0x was removed).
 * `hold_ms` is how long the current run has lasted, 0 if there is no
 * active run. */
static float pitch_bend_confidence_multiplier(uint32_t hold_ms) {
    if (hold_ms >= PITCH_BEND_ARM_MS) {
        return 1.0f;
    }
    return (float)hold_ms / (float)PITCH_BEND_ARM_MS;
}

/* Maps a cosine delta (already vertical-pressure-compensated and
 * sign-flipped by the caller -- see this file's "Pitch bend from
 * sideways motion" section for why) to the 14-bit MIDI pitch bend wire
 * value -- see s_pitch_bend_max_cosine_deviation's own comment.
 * PITCH_BEND_DEADZONE_COSINE_DELTA is applied as a "soft knee," not a
 * hard cutoff: within it, output is exactly centered; just past it,
 * output ramps continuously from 0 rather than jumping straight to some
 * nonzero value, and still reaches full swing at exactly the same real
 * deviation (s_pitch_bend_max_cosine_deviation) as before any deadzone
 * existed -- before PITCH_BEND_ARM_MS's own confidence ramp (see
 * pitch_bend_confidence_multiplier()) is layered on top of that. Reads
 * and updates `s`'s own run-tracking fields (now per-pad, not
 * module-level -- every currently-bending pad confirms/tracks its run
 * completely independently of every other one). */
static uint16_t pitch_bend_14bit_from_cosine_delta(pad_expr_t *s, float delta, uint32_t now_ms) {
    float magnitude = fabsf(delta);
    bool positive = delta >= 0.0f;
    float sign = positive ? 1.0f : -1.0f;
    if (magnitude <= PITCH_BEND_DEADZONE_COSINE_DELTA) {
        /* Back within the deadzone -- no active run, and nothing to
         * confirm. */
        s->pitch_bend_run_active = false;
        magnitude = 0.0f;
    } else {
        if (!s->pitch_bend_run_active || positive != s->pitch_bend_run_positive) {
            /* A brand-new run: either the first deviation since the
             * deadzone, or a direction reversal -- either way, this is
             * NOT a continuation, so confirmation starts over from 0. */
            s->pitch_bend_run_active = true;
            s->pitch_bend_run_positive = positive;
            s->pitch_bend_run_start_ms = now_ms;
        }
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

    uint32_t hold_ms = s->pitch_bend_run_active ? (now_ms - s->pitch_bend_run_start_ms) : 0u;
    normalized *= pitch_bend_confidence_multiplier(hold_ms);

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

/* Shared by tiles_expression_toggle_pitch_bend() (disabling) and
 * tiles_expression_set_muted() (muting) below -- under the old
 * single-owner design there was at most one bending pad to reset;
 * under MPE every currently-held note can be bending independently at
 * once, so both call sites now need to walk every pad and center
 * whichever ones actually have pitch_bend_active set, rather than
 * resetting one single piece of shared state. */
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

/* Real feedback: "when you press sentia button once it turns on and off
 * the pitch bend" -- called by services/expression_control.h on a
 * genuine square ("sentia") short click. Turning it off resets to
 * center immediately, for every pad currently bending, rather than
 * leaving any of them stuck bent. */
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

void tiles_expression_set_aftertouch_sensitivity(uint16_t depth_full_scale) {
    /* Never 0 -- aftertouch_from_depth() divides by this. */
    s_depth_to_aftertouch_full_scale = depth_full_scale > 0u ? depth_full_scale : 1u;
    printf("[expression] aftertouch sensitivity (full-scale depth) now %u\n", s_depth_to_aftertouch_full_scale);
}

void tiles_expression_set_muted(bool muted) {
    s_expression_muted = muted;
    printf("[expression] muted=%d\n", (int)s_expression_muted);
    if (muted) {
        /* Same "never leave a note stuck bent" rule
         * tiles_expression_toggle_pitch_bend() already follows -- reset
         * to center immediately rather than waiting for each note's own
         * release/retrigger to clear it. */
        center_and_deactivate_all_bending_pads();
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
    /* powf() with VELOCITY_CURVE_EXPONENT == 1.0 is a plain linear
     * response (see that constant's own comment for why) -- kept as a
     * general power curve rather than hand-simplified to a straight
     * multiply, so a future round can retune the exponent again without
     * restructuring this function. normalized is in (0, 1) as "how much
     * of the way from slow to fast." */
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
 * see this file's "Pitch bend from sideways motion" section. Seeds
 * pitch_bend_active from the player's current enabled/muted state at
 * this exact moment (toggling either mid-hold doesn't retroactively
 * change an already-sounding note's bend). Does NOT capture the
 * baseline yet -- see PITCH_BEND_SETTLE_MS's own comment for why that's
 * deferred a few ticks, in the NOTE_ON loop below, rather than grabbed
 * from one instantaneous sample right here. */
static void init_pitch_bend_for_pad(pad_expr_t *s, uint8_t pad, uint32_t now_ms) {
    s->pitch_bend_active = s_pitch_bend_enabled && !s_expression_muted;
    if (!s->pitch_bend_active) {
        return;
    }
    tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
    float x, y, magnitude;
    hall_xy_and_magnitude(hs.x, hs.y, hs.z, &x, &y, &magnitude);
    (void)magnitude;
    s->pitch_bend_smoothed_x = x;
    s->pitch_bend_smoothed_y = y;
    s->pitch_bend_smoothed_delta = 0.0f;
    s->pitch_bend_baseline_settled = false;
    s->pitch_bend_claim_ms = now_ms;
    s->pitch_bend_last_sent = PITCH_BEND_CENTER;
    s->pitch_bend_run_active = false;
}

/* Ends `pad`'s currently-held note completely and cleanly: MIDI note-off,
 * haptic stop, pitch bend reset to center, and frees its MPE channel
 * slot -- the single place this whole sequence happens, used by every
 * normal note-off/retrigger call site below AND by claim_mpe_channel()'s
 * channel-stealing further below (running out of the 15 MPE member
 * channels is rare on a 24-pad board, but must still leave everything --
 * the synth's note state, this pad's own state machine -- consistent
 * when it happens). Always centers the freed channel's pitch bend,
 * whether or not this specific pad was actively bending, so the NEXT
 * note assigned to this channel (by claim_mpe_channel() below) can never
 * inherit a stale bend -- the per-note equivalent of the old
 * single-owner design's "reset to center when ownership changes" rule,
 * now enforced once per channel release instead of scattered across
 * every caller. Does NOT set state = PAD_STATE_IDLE; callers do that
 * themselves since the two normal call sites (note-off, retrigger)
 * transition to different next states. */
static void end_held_note(pad_expr_t *s, uint8_t pad) {
    tiles_midi_note_off(s->midi_channel, s->active_note);
    tiles_haptics_stop(pad);
    tiles_midi_send_pitch_bend(s->midi_channel, PITCH_BEND_CENTER);
    s->pitch_bend_active = false;
    uint8_t idx = (uint8_t)(s->midi_channel - TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL);
    s_mpe_channels[idx].in_use = false;
}

/* See this file's own header comment for the full "haptics vibration
 * randomly in mini games" reasoning -- called once by services/
 * game_mode.h right when it takes over the board. Only PAD_STATE_NOTE_ON
 * pads need the real end_held_note() teardown (note-off + haptic stop +
 * MPE channel release); a pad merely at PAD_STATE_AWAITING_STRIKE never
 * claimed a channel or sent a note-on in the first place, so it just
 * needs its state reset -- calling end_held_note() on one of those would
 * send a bogus note-off and free an MPE channel that was never claimed. */
void tiles_expression_force_release_all(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        pad_expr_t *s = &s_pads[i];
        if (s->state == PAD_STATE_NOTE_ON) {
            end_held_note(s, pad);
        }
        s->state = PAD_STATE_IDLE;
    }
}

/* MPE Member Channel allocator -- see s_mpe_channels' own comment for
 * the voice-stealing policy. Returns the claimed channel (status-byte
 * nibble). If every Member Channel is already in use, forcibly ends the
 * oldest-claimed one's note (via end_held_note() above) and hands that
 * SAME channel straight to the new pad, rather than freeing it and
 * re-searching -- avoids a redundant second scan and keeps the "steal"
 * atomic from this function's own perspective. */
static uint8_t claim_mpe_channel(uint8_t pad) {
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        if (!s_mpe_channels[i].in_use) {
            s_mpe_channels[i].in_use = true;
            s_mpe_channels[i].owner_pad = pad;
            s_mpe_channels[i].claim_seq = s_next_mpe_claim_seq++;
            return (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i);
        }
    }

    uint8_t oldest_idx = 0;
    for (uint8_t i = 1; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        if (s_mpe_channels[i].claim_seq < s_mpe_channels[oldest_idx].claim_seq) {
            oldest_idx = i;
        }
    }
    uint8_t stolen_pad = s_mpe_channels[oldest_idx].owner_pad;
    printf("[expression] pad %u stealing pad %u's MPE channel %u (all %u member channels in use)\n", pad, stolen_pad,
           (unsigned)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + oldest_idx), (unsigned)TILES_MIDI_MPE_NUM_MEMBER_CHANNELS);
    pad_expr_t *stolen = &s_pads[stolen_pad - 1u];
    end_held_note(stolen, stolen_pad);
    stolen->state = PAD_STATE_IDLE;
    s_mpe_channels[oldest_idx].owner_pad = pad;
    s_mpe_channels[oldest_idx].claim_seq = s_next_mpe_claim_seq++;
    return (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + oldest_idx);
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
             * held) claims the pad grid for its own slider taps,
             * services/octave_control.h's transpose mode (SW1+SW2 held)
             * claims it to display the current key, services/op_mode.h's
             * mode-select menu/sequencer mode claims it for mode-picking/
             * step-arming taps, and services/game_mode.h's minigames claim
             * it for their own menu/gameplay grid -- a fresh touch while
             * any of these is showing must never also start a real strike
             * underneath (real feedback on the transpose case: "playing
             * the grid in transpose menu exits the menu" -- notes firing
             * and haptics kicking in while the player is just trying to
             * read/set the key; the identical complaint later for game
             * mode: "no midi from pads in game mode... fix haptics
             * randomly happening in game modes" -- this file never had a
             * game_mode.h check at all until now, so every grid touch
             * during a menu selection or incidental contact mid-game ran
             * this same real note+haptic pipeline completely unaware
             * anything else owned the board). A pad already past IDLE
             * when any of these opens is deliberately left alone (see the
             * loop below), only a brand-new touch is suppressed here. */
            if (touched && !tiles_expression_control_owns_pad_grid() && !tiles_octave_control_is_transpose_active() &&
                !tiles_op_mode_owns_pad_grid() && !tiles_game_mode_is_active()) {
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
                /* Claims this note's own MPE Member Channel BEFORE
                 * sending note-on -- matters even under MPE's genuinely
                 * per-note channels, because claim_mpe_channel() can
                 * itself force-end a DIFFERENT pad's note to steal its
                 * channel if all 15 are already in use (see that
                 * function's own comment); that steal always leaves the
                 * channel centered before handing it over
                 * (end_held_note()'s own guarantee), so claiming first
                 * still means this note-on can never reach the synth
                 * while a stale bend from whatever used this channel
                 * before is still in effect -- the same real-hardware
                 * bug ("sometimes play lands in bent note") this
                 * ordering originally fixed, now guaranteed structurally
                 * by MPE's per-note channels in the common case and by
                 * this ordering in the channel-stealing edge case. */
                s->midi_channel = claim_mpe_channel(pad);
                init_pitch_bend_for_pad(s, pad, now_ms);
                tiles_midi_note_on(s->midi_channel, s->active_note, velocity);
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
            end_held_note(s, pad);
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
            end_held_note(s, pad);
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
                tiles_midi_send_channel_pressure(s->midi_channel, at);
            }
            tiles_haptics_set_sustain_level(pad, at);
        }

        /* Pitch bend: genuinely per-note now -- every pad with
         * pitch_bend_active runs this completely independently on its
         * own MPE channel, no shared "owner" arbitration needed (see
         * this file's "Pitch bend from sideways motion" section). */
        if (s->pitch_bend_active) {
            tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
            if (hs.valid) {
                float x, y, magnitude;
                hall_xy_and_magnitude(hs.x, hs.y, hs.z, &x, &y, &magnitude);

                if ((now_ms - s_hall_calibration_print_ms) >= HALL_CALIBRATION_PRINT_INTERVAL_MS) {
                    s_hall_calibration_print_ms = now_ms;
                    printf("[hall-cal] pad %u x=%d y=%d z=%d depth=%.0f magnitude=%.1f\n", pad, (int)hs.x, (int)hs.y,
                           (int)hs.z, (double)s->smoothed_depth, (double)magnitude);
                }

                if (!s->pitch_bend_baseline_settled) {
                    /* See PITCH_BEND_SETTLE_MS's own comment -- stays
                     * centered (never even reaches the send-if-changed
                     * check below) until the EMAs below have had a few
                     * ticks to settle, then captures baseline X/Y from
                     * those settled values rather than one raw
                     * instantaneous sample. */
                    s->pitch_bend_smoothed_x += PITCH_BEND_SMOOTHING_ALPHA * (x - s->pitch_bend_smoothed_x);
                    s->pitch_bend_smoothed_y += PITCH_BEND_SMOOTHING_ALPHA * (y - s->pitch_bend_smoothed_y);
                    if ((now_ms - s->pitch_bend_claim_ms) >= PITCH_BEND_SETTLE_MS) {
                        s->pitch_bend_baseline_x = s->pitch_bend_smoothed_x;
                        s->pitch_bend_baseline_y = s->pitch_bend_smoothed_y;
                        s->pitch_bend_baseline_settled = true;
                        s->pitch_bend_smoothed_delta = 0.0f;
                    }
                } else {
                    /* Vertical-pressure compensation -- real feedback:
                     * "the pitchbend seems to lean towards down bend not
                     * up bend regardless of tilt... it should compensate
                     * for vertical pressure to get the correct tilt."
                     * The direction-cosine theory (this file's own
                     * section above) assumes a PERFECTLY on-axis magnet;
                     * real hardware doesn't fully match that, so a fixed
                     * baseline COSINE (captured once, compared against
                     * for the rest of the hold) still lets a real
                     * assembly misalignment's contribution grow as |B|
                     * shrinks with a harder press, biasing the result
                     * toward whichever direction that misalignment
                     * happens to point, regardless of actual tilt.
                     * Fix: instead of comparing against a fixed baseline
                     * COSINE, re-derive what the baseline RAW X would
                     * predict the cosine to be AT THE CURRENT depth --
                     * direction_cosine_from(baseline_x, magnitude) with
                     * THIS tick's magnitude, not the magnitude from
                     * whenever baseline_x was captured. If X hasn't
                     * genuinely changed (pure depth change, zero real
                     * tilt), current cosine and this depth-adjusted
                     * prediction are mathematically IDENTICAL by
                     * construction (both are baseline_x /
                     * current_magnitude), so the raw delta is exactly 0
                     * -- regardless of how deep the press has gone. A
                     * REAL tilt, which genuinely changes X beyond
                     * whatever baseline_x was, still produces a real
                     * nonzero delta.
                     *
                     * Both terms below use THIS SAME tick's `magnitude`
                     * -- critically, neither is independently smoothed
                     * before the subtraction. A first version of this
                     * fix smoothed the "current" cosine (an EMA, lagging
                     * by construction) but compared it against an
                     * unsmoothed "predicted baseline" -- during any
                     * depth change (essentially the entire strike-to-
                     * hold ramp on every note), that lag mismatch alone
                     * produces a nonzero, depth-correlated delta even
                     * for zero real tilt, reintroducing exactly the bias
                     * this compensation exists to remove. Real feedback
                     * after that version: "pitch bend is so extreme...
                     * biased towards down it never goes up" -- consistent
                     * with a lag-driven artifact, since a struck-then-
                     * held note's depth is deepening (not easing) for
                     * most of its hold. Fixed by computing the raw,
                     * already depth-compensated delta fresh every tick
                     * (both terms same magnitude, so a pure depth change
                     * cancels to 0 before any smoothing happens at all)
                     * and smoothing THAT result instead -- smoothing is
                     * still wanted for noise rejection (see
                     * PITCH_BEND_SMOOTHING_ALPHA), just applied
                     * downstream of the compensation instead of feeding
                     * mismatched inputs into it.
                     *
                     * Sign flipped here (predicted - current, not
                     * current - predicted) -- real feedback: "bend is
                     * flipped its bending in the opposite way than we
                     * need." pitch_bend_14bit_from_cosine_delta() itself
                     * is otherwise direction-agnostic.
                     *
                     * Two axes combined here, not X alone -- real
                     * feedback: "incorporate the 2 axis tilt onto the
                     * pitch bend to provide a more strong reading of
                     * tilt... more sable reeds... make vibratos." Y gets
                     * the exact same same-magnitude compensation treatment
                     * as X above (delta_y cancels to 0 for a pure depth
                     * change, for the identical reason delta_x does).
                     * MAGNITUDE combines both axes (sqrt(dx^2 + dy^2)) --
                     * strictly >= either axis alone, so a real tilt or
                     * wiggle that happens to land partly on Y (which the
                     * old X-only signal simply discarded) now adds to the
                     * reading instead of being lost, giving small/rapid
                     * motion -- a vibrato wiggle, specifically -- a
                     * stronger, more reliable signal to clear the
                     * deadzone with. SIGN stays anchored to delta_x alone,
                     * deliberately not a true 2D bend direction -- this
                     * preserves the already-tuned left/right bend feel
                     * the deadzone/sensitivity constants below were
                     * calibrated against, rather than redefining what
                     * "positive bend" means. */
                    float predicted_baseline_cosine_x = direction_cosine_from(s->pitch_bend_baseline_x, magnitude);
                    float current_cosine_x = direction_cosine_from(x, magnitude);
                    float delta_x = predicted_baseline_cosine_x - current_cosine_x;

                    float predicted_baseline_cosine_y = direction_cosine_from(s->pitch_bend_baseline_y, magnitude);
                    float current_cosine_y = direction_cosine_from(y, magnitude);
                    float delta_y = predicted_baseline_cosine_y - current_cosine_y;

                    float combined_magnitude = sqrtf(delta_x * delta_x + delta_y * delta_y);
                    float raw_delta_this_tick = (delta_x >= 0.0f) ? combined_magnitude : -combined_magnitude;
                    s->pitch_bend_smoothed_delta +=
                        PITCH_BEND_SMOOTHING_ALPHA * (raw_delta_this_tick - s->pitch_bend_smoothed_delta);
                    float delta = s->pitch_bend_smoothed_delta;
                    uint16_t bend = pitch_bend_14bit_from_cosine_delta(s, delta, now_ms);
                    if (bend != s->pitch_bend_last_sent) {
                        s->pitch_bend_last_sent = bend;
                        tiles_midi_send_pitch_bend(s->midi_channel, bend);
                        /* Temporary bring-up visibility -- real feedback
                         * across several rounds of guessed constants
                         * with no captured real numbers behind most of
                         * them, unlike MIN_STRIKE_DEPTH_DELTA/DEPTH_TO_
                         * AFTERTOUCH_FULL_SCALE elsewhere in this file.
                         * Prints the raw (already depth-compensated)
                         * delta AND this tick's smoothed depth together
                         * so a future capture can check whether the
                         * compensation above actually decorrelated bend
                         * from press depth, not just eyeball it. */
                        printf("[expression] pad %u pitch bend sent: channel=%u bend=%u delta=%.4f depth=%.0f\n", pad,
                               s->midi_channel, bend, (double)delta, (double)s->smoothed_depth);
                    }
                }
            }
        }
    }
}
