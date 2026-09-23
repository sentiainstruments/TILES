#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "cv_gate.h"
#include "midi_out.h"
#include "haptics.h"
#include "expression_control.h"
#include "game_mode.h"
#include "octave_control.h"
#include "op_mode.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* Real feedback (board 2 only, an experiment -- see this file's own
 * "Melodic harmonics" section further below): "i wanna add harmonics
 * into melodic mode. like capacitive touch only plays the respective
 * harmonics of the note being played by a pad with real pressure. this
 * behaviour only happens when a single pad is being pressed not in
 * poliphony." Compile-time, default OFF -- this build ships to every
 * board, and this is the one thing that must NOT: flip to 1, build,
 * flash ONLY board 2, then flip back to 0 before committing again. No
 * runtime toggle -- flash-persisted settings don't exist in this
 * codebase yet, so a runtime flag would silently reset to off on every
 * boot, which defeats "just live on this board." */
#define TILES_MELODIC_HARMONICS_ENABLED 0

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
#define MIN_STRIKE_DEPTH_DELTA TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA

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
 * from real numbers instead of guessing a third velocity model.
 *
 * STRIKE_TIME_MIN_VELOCITY_MS widened 150 -> 300 same session, real
 * feedback right after trying the linear curve above: "vewlocity shoots
 * up to max xeasely, we need more playing range and less inmediatye
 * hard strike." A 140ms-wide window (10-150) meant an ordinary,
 * unhurried tap -- nowhere near a deliberate slow push, but nowhere
 * near a fast strike either -- already sat well past the midpoint of
 * the range, so under the now-linear mapping it read as most of the way
 * to max velocity. Widening the slow end to 300ms doesn't change what
 * counts as a genuinely fast strike (still <= 10ms for 127), but it
 * stretches ordinary-to-slow playing across double the window, giving
 * real dynamic range to press speeds that used to cluster near the top
 * instead of only ever separating out the most extreme soft touches.
 *
 * Still not enough, real feedback with a live serial capture showing why:
 * a deliberately LIGHT tap on pad 1 committed at strike_time_ms=10 (the
 * MAX-velocity floor) -- "slow light press is still to hard velocity
 * wise... we need a way to measure light press with distance but we
 * need some of the bias of speed as well." Elapsed-time-to-threshold
 * fundamentally can't tell a light, quick tap from a hard, quick strike:
 * both cross MIN_STRIKE_DEPTH_DELTA almost immediately if the touch
 * itself is fast, however little force is behind it -- time-to-
 * threshold measures *quickness*, not *how hard*, and those aren't the
 * same thing for a fast-but-gentle touch.
 *
 * First attempt at fixing this added a depth signal (how far past the
 * threshold peak_depth had already traveled AT THE EXACT SAMPLE that
 * crossed it) blended 70/30 with time, with zero added latency -- but
 * real feedback after trying it: "light preasure taps do medium
 * velocity when they should do minimum velocity. we need to make
 * gentil slow taps and fast light taps low velocity it cant be strong
 * and strong and deep has to be consistently strong like a piano. it
 * shoul dfeel like a hammer action piano." The zero-latency version's
 * real flaw: measuring overshoot AT THE INSTANT of crossing still lets
 * a fast-but-light touch fake a real overshoot, if the touch happened
 * to be moving quickly right as it grazed the threshold -- exactly the
 * "fast light tap reads as medium/strong" symptom. A real acoustic
 * piano hammer doesn't have this problem because its mechanism
 * physically can't cover full key travel fast without real force behind
 * it -- "fast" and "light" can't coexist at the same travel distance on
 * a piano the way they can on a shallow capacitive/Hall pad. The fix:
 * don't judge depth from one instantaneous sample -- give the strike a
 * short, fixed follow-through window (VELOCITY_FOLLOWTHROUGH_MS) AFTER
 * crossing to keep revealing its true depth before committing (see
 * tiles_expression_scan()'s own `ready` condition below). A gentle slow
 * tap and a fast light tap both plateau near the threshold during that
 * window (there's no real force behind either to carry depth further);
 * a strong, deep press keeps climbing well past it regardless of
 * exactly how fast it started. This is the ACTUAL "hammer action" fix
 * -- depth over a real observation window is what correlates with
 * force on this hardware, not an instantaneous depth reading, and not
 * elapsed time to a shallow threshold. Costs a small, fixed amount of
 * onset latency (up to VELOCITY_FOLLOWTHROUGH_MS) for any touch that
 * stays down that long; a touch that releases sooner than that still
 * commits immediately on release with whatever depth it reached (see
 * commit_on_release below), so a genuinely brief tap adds no latency at
 * all. Depth is now even more dominant (STRIKE_DEPTH_WEIGHT, 0.85) with
 * only a small time-based bias left (0.15) -- enough to still slightly
 * favor a faster hit at the SAME eventual depth, per "some of the bias
 * of speed as well" from the previous round, but no longer enough on
 * its own to read a shallow touch as anything but soft. */
#define STRIKE_TIME_MAX_VELOCITY_MS 10u
#define STRIKE_TIME_MIN_VELOCITY_MS 300u
#define VELOCITY_CURVE_EXPONENT 1.0f

/* How long after crossing MIN_STRIKE_DEPTH_DELTA to keep watching
 * peak_depth climb before actually committing the note -- see this
 * section's own header for why. First attempt, not measured against
 * real strikes; long enough that a genuinely light/gentle touch has
 * clearly plateaued by the end of it, short enough to stay well under
 * perceptible note-onset latency (typically cited around 10-20ms for a
 * percussive instrument). */
#define VELOCITY_FOLLOWTHROUGH_MS 20u

/* How much peak_depth can overshoot MIN_STRIKE_DEPTH_DELTA (by the end
 * of the follow-through window above) before the depth signal itself
 * maxes out. Raised alongside the follow-through window's introduction
 * -- a hard strike now has a real window to keep climbing in, not just
 * one instantaneous sample, so it can plausibly overshoot much further
 * than before. First attempt, not measured -- there's no captured data
 * yet for "how far does a genuinely hard strike get in
 * VELOCITY_FOLLOWTHROUGH_MS beyond the threshold on this hardware,"
 * unlike MIN_STRIKE_DEPTH_DELTA's own real capture data above. Revisit
 * once a real light-vs-hard capture session records actual
 * peak_depth-at-commit numbers instead of only strike_time_ms. */
#define STRIKE_DEPTH_OVERSHOOT_FULL_SCALE 550.0f

/* Depth's share of the final blend -- see this section's own header for
 * why depth so heavily dominates and time is only a small bias. */
#define STRIKE_DEPTH_WEIGHT 0.85f

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
 * `[expression] pitch bend sent` print (since removed -- see this file's
 * pitch-bend-sent entry in services/README.md -- once it had served
 * exactly this diagnostic purpose, added specifically because this
 * constant had been guessed blind through several prior rounds) during
 * several seconds of a real, ordinary straight-down press with no
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
 * tradeoff were actually visible together. Lowered to 0.04 accordingly.
 *
 * That whole capture predates the adaptive depth-rate-gated recentering
 * fix (see PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW's own comment) --
 * its at-rest noise floor was dominated by pressure-coupling this
 * constant had no way to address except by sitting high enough to
 * reject it, which is exactly why it also cost some real tilt
 * sensitivity. With that noise now suppressed at its actual source
 * instead, the true remaining noise floor is very likely meaningfully
 * SMALLER than that old data suggests, but this hasn't been re-measured
 * yet. Lowered 0.04 -> 0.025 as a first, deliberately modest step (not
 * a re-guess to something tiny) -- real feedback: "wow it feels good...
 * a tiny bit more sensitivity... max ease of tilt but without loosing
 * precision for regular press." Changed alone this round, not stacked
 * with a big PITCH_BEND_ARM_MS cut too, so a fresh capture can tell
 * which constant (if either) needs correcting if regular-press
 * precision regresses. */
#define PITCH_BEND_DEADZONE_COSINE_DELTA 0.025f

/* How long AFTER claiming ownership before the baseline cosine is
 * actually captured, letting PITCH_BEND_SMOOTHING_ALPHA's cascade settle
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
 * bend stays centered (see the NOTE_ON loop below).
 *
 * Raised 25 -> 250 once X/Y/magnitude became a TWO-STAGE cascade (see
 * PITCH_BEND_SMOOTHING_ALPHA's own comment) -- 25ms was tuned for a
 * SINGLE EMA stage and never revisited when a second stage was added, a
 * real bug found by tracing through an actual capture rather than
 * another guessed constant: with 25ms nowhere near enough for a 2-stage
 * cascade to converge, `pitch_bend_baseline_x/y` ended up captured
 * while `pitch_bend_smoothed_x2/y2` was still essentially sitting at
 * its cold-start seed (the noisy instantaneous sample from the exact,
 * often percussive strike instant this comment already warns about) --
 * then, over the following several hundred ms, the cascade genuinely
 * converged toward the pad's TRUE average position, which reads as a
 * large, steadily growing "delta" with nothing to do with real tilt.
 * This one bug plausibly explained two separate real-feedback
 * complaints at once: "it definitely still triggers on full press
 * without bend" (the convergence transient crossing the deadzone on
 * essentially every note, tilt or not) and "bend reacts but not
 * consistently" (the transient's size/direction depends on how much the
 * raw signal moved between the noisy seed instant and true
 * convergence, which varies note to note). 250ms is a first attempt at
 * actually giving the cascade (per-stage ~100-200ms, ~1.4x for two
 * stages -- see PITCH_BEND_SMOOTHING_ALPHA's own math) real time to
 * converge before baseline is trusted; costs a longer "no bend yet"
 * grace period right after every strike than before. Real feedback once
 * played: "stable [the freeze is fixed] but bend takes too long to
 * start... requires max tilt for it to happen" -- combined with
 * PITCH_BEND_ARM_MS's own 120ms confirmation window stacked on top,
 * that's up to ~370ms of elapsed note time before a real tilt could
 * ever reach full confidence; a quick, natural tilt gesture doesn't
 * last that long, so it read as weak ("requires max tilt") when the
 * real requirement was time, not tilt angle. Lowered 250 -> 120 -- still
 * ~5x the original (broken) 25ms, but the WORST of the convergence-
 * transient risk this constant exists to fix was always concentrated in
 * roughly the first single-stage settling window (~100-150ms), not the
 * full two-stage one; 120ms is a first attempt at a middle ground
 * rather than re-guessing back to the original broken value. Unmeasured
 * whether some residual convergence-transient risk leaks back in at
 * this shorter window -- worth a fresh capture to confirm. */
#define PITCH_BEND_SETTLE_MS 120u

/* How fast the baseline slowly re-centers toward the CURRENT raw X/Y
 * while no real bend run is active -- real feedback, this file's own
 * X-only ("side tilt") debugging round: "there is always some minor
 * give in pressed mode either way." A real-hardware capture of a
 * straight, non-tilted hold found the problem wasn't (only) fast
 * flickering noise -- PITCH_BEND_ARM_MS's sign-consistency requirement
 * already handles that case (see that constant's own comment) -- it was
 * a SUSTAINED, one-directional offset for the length of an entire ~9s
 * hold: raw X sat consistently 32-112 away from the baseline captured
 * in the first PITCH_BEND_SETTLE_MS of contact, for the whole hold, no
 * flip-flopping at all. A single fixed baseline can never distinguish
 * that from a genuine held tilt lasting just as long -- both look
 * identical to it. Fixed with a slow high-pass/DC-blocking approach
 * instead: baseline_x/baseline_y keep drifting toward whatever X/Y
 * currently reads, but ONLY while pitch_bend_run_active is false (see
 * that field's own comment) -- gated on the PREVIOUS tick's
 * classification, one tick of lag, negligible against any real human
 * gesture. This freezes recentering the instant a real bend run is
 * confirmed, so an actively-held deliberate tilt doesn't fade back
 * toward center on its own; recentering only ever happens while
 * genuinely at rest (or inside noise too small to have confirmed a run
 * at all), which is exactly when it's safe to treat "wherever the pad
 * currently sits" as the new true center.
 *
 * Inherent tradeoff of any DC-blocking filter, not fully solved by the
 * run-gate above: a single bend held continuously for MANY seconds
 * (well past PITCH_BEND_ARM_MS's confirmation window, but the player
 * never releases and re-strikes) will very slowly relax back toward
 * center too, since pitch_bend_run_active would need to go false at
 * some point for that specific held note to ever recenter -- in
 * practice it doesn't (a continuously-held real run keeps run_active
 * true, keeps recentering paused, matching real intent) -- but this is
 * worth knowing about if a future round finds a very long sustained
 * bend gradually fading. Extrapolated from PITCH_BEND_SMOOTHING_ALPHA's
 * own already-observed real-hardware settling time (documented there as
 * roughly 100-200ms) -- deliberately ~40x slower than that, aiming for
 * a multi-second re-center rather than one fast enough to fight a real
 * held expressive gesture. Unmeasured against real long-hold data;
 * worth a fresh capture to confirm this doesn't recenter either too
 * slowly (give still leaks through for the first few seconds of a long
 * hold) or too fast (a deliberate multi-second hold starts drifting
 * back toward center before the player releases it).
 *
 * Second real problem this exposed, once tested against an explicit,
 * slow, deliberately-HELD single-direction lean (not just a straight
 * press) -- real feedback: "the old thing was not working we need to
 * compensate for preassure depth and drift." That capture sent ZERO
 * bend for a full ~10s deliberate lean. Averaging the raw X samples by
 * hand (not just eyeballing individual ones) found the real signal WAS
 * there -- roughly 13-15 raw units consistently above whatever the
 * baseline happened to be at the time, both early and late in the hold
 * -- but individual samples swing far more wildly than that around it
 * (holding a pad leaned to one side takes continuous muscle tension,
 * which is naturally less steady than a relaxed straight press; real
 * physiological tremor rides on top of the real lean). The recenter
 * step was chasing RAW X directly, tick by tick -- exactly as noisy as
 * any single sample -- which does two bad things at once: it makes
 * baseline_x/y itself jumpy (defeating the point of a "true rest
 * center"), and, more importantly, it keeps dragging the baseline
 * toward wherever the noisy CURRENT sample happens to be, actively
 * erasing the real ~13-15-unit average bias before it ever gets a
 * chance to build up into a confirmed run. Fixed by recentering toward
 * pitch_bend_smoothed_x/y (the existing PITCH_BEND_SMOOTHING_ALPHA
 * medium EMA, ~100-200ms -- already real-hardware-validated as enough
 * to reject fast noise, see that constant's own history) instead of raw
 * X/Y -- the recenter target itself is now already tremor-filtered,
 * so it can only chase a signal that's persisted for a meaningful
 * stretch, not every individual noisy sample. At the time, this did NOT
 * touch how the live bend delta itself was computed (current_cosine_x
 * still used RAW X paired with THIS SAME tick's magnitude) -- a later
 * round (see PITCH_BEND_SMOOTHING_ALPHA's own "cascade" comment) also
 * moved the live delta itself onto the same cascaded, tremor-filtered
 * X/Y/magnitude, once real research (not another guessed constant)
 * showed a single EMA stage's rolloff genuinely wasn't steep enough on
 * its own; the "same magnitude for both terms" invariant this section
 * cares about was preserved throughout that change.
 *
 * This constant, and the whole idea of a SINGLE recenter rate, was
 * replaced by an ADAPTIVE rate -- real feedback, after a real-data
 * depth-vs-Y calibration curve (tried next, see git history for that
 * whole round) still left "pressure is still doing pitch bend... for
 * pitch bend to actually work its taking a lot of time and tilt": "lets
 * think of the architecture the math and industry practices for this
 * kind of situations." A static calibration -- whether one constant or
 * a whole curve -- can only ever be as good as the one capture it came
 * from, and does nothing to reduce the noise the deadzone/confirmation
 * window downstream still have to fight, which is exactly what made
 * genuine tilt slow and insensitive. Real insight: the pressure-
 * coupling artifact only actually happens WHILE depth is changing --
 * holding steady at any depth doesn't introduce it. So instead of
 * predicting what X/Y SHOULD be at a given depth, gate the recenter
 * RATE on whether depth is currently changing: fast (see
 * PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST below) while actively
 * pressing/releasing, so the baseline snaps to reality in real time
 * instead of lagging behind it (there's nothing to lose by moving fast
 * here -- a run is never confirmed during a press ramp anyway); this
 * SLOW rate, unchanged, once depth has settled and holds steady, which
 * is when a real bend needs protecting from fading. This is the same
 * underlying idea as the well-known "One Euro Filter" (adapt a filter's
 * rate based on the speed of the thing it's tracking) and, more
 * directly, how biosignal processing rejects a KNOWN motion confound:
 * use an independent detector of that confound (there, an
 * accelerometer; here, depth's own rate of change) to gate trust in the
 * signal it corrupts, rather than modeling the corruption itself. See
 * the main scan loop's own "depth_activity" comment for the actual
 * blend. Needs no calibration capture at all -- unlike either previous
 * attempt, it measures the real depth/tilt relationship live, on every
 * pad, continuously, so it can't go stale or fail to generalize from
 * whichever single pad a capture happened to be taken from. */
#define PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW 0.002f
/* How fast the baseline snaps to current X/Y while depth is actively
 * changing (depth_activity near 1) -- see PITCH_BEND_BASELINE_RECENTER_
 * ALPHA_SLOW's own comment for the full reasoning. Comparable to (a
 * bit faster than) PITCH_BEND_SMOOTHING_ALPHA's own per-stage rate
 * (0.08, ~100-200ms) -- fast enough that the baseline doesn't lag
 * meaningfully behind even a quick strike's own depth ramp, not so fast
 * it becomes as noisy as an unfiltered raw sample again. Unmeasured --
 * a first attempt; worth a fresh capture to confirm this is fast enough
 * to track a genuinely quick strike without leaving a residual gap. */
#define PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST 0.25f
/* How large pitch_bend_smoothed_depth_rate's own magnitude needs to be
 * to count as "depth is definitely actively changing right now"
 * (depth_activity = 1.0) -- see the main scan loop's own comment for
 * how this scales the blend between the two constants above, and
 * pitch_bend_smoothed_depth_rate's own struct comment for why this is
 * measured against a SMOOTHED SIGNED rate, not a raw per-tick delta (the
 * first version of this constant, 8.0 against the raw delta, saturated
 * to 1.0 almost constantly during real tilting -- real feedback: "still
 * no pitch bend on tilt but preassure works" -- because depth itself
 * wobbles by a lot DURING a real tilt, not just during a deliberate
 * press; only sign-CONSISTENT movement should count as "pressing," and
 * the smoothed-signed-rate fixes that, but the right threshold against
 * THAT smoothed signal is a fresh unknown, not a simple rescale of the
 * old one). Unmeasured -- a first attempt pending a real capture of
 * pitch_bend_smoothed_depth_rate's own values during both a genuine
 * press and a genuine tilt, to see where they actually separate. */
#define PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE 8.0f

/* Brief noise-transient filter -- real feedback wanted LESS tilt needed
 * to trigger a bend, and a big amplitude deadzone was the previous
 * round's way of also rejecting brief accidental wobbles, which fought
 * directly against that ask. A deviation past the deadzone ramps
 * linearly from 0 up to full weight over this many ms rather than
 * outputting at full strength the instant it's crossed -- a genuinely
 * brief, noisy blip never reads as more than a fraction of its already-
 * small magnitude before the next real sample either confirms or
 * clears it, while a real held tilt reaches full weight almost
 * immediately. Reset to 0 (see pitch_bend_14bit_from_cosine_delta()'s
 * own run-tracking) whenever magnitude drops back within the deadzone
 * -- i.e. a genuine return toward center, not just a fast sample-to-
 * sample sign wobble on top of an otherwise still-real, still-ongoing
 * deflection; that distinction (and why it matters once X+Y are
 * combined) is documented at the run-tracking's own site.
 *
 * Raised 15 -> 120 after debugging X-only ("side tilt") specifically --
 * real feedback: "there is always some minor give in pressed mode
 * either way." A dedicated real-hardware capture of a completely
 * straight, non-tilted hold (this file's own X-only round, prompted by
 * "lets debug side tilt and ignore other tilt for now") found that
 * ordinary mechanical give under sustained pressure alone -- no
 * deliberate tilt at all -- crosses PITCH_BEND_DEADZONE_COSINE_DELTA on
 * 35% of samples and crosses s_pitch_bend_max_cosine_deviation (i.e.
 * would send close to FULL bend) on 11.5% of samples, out of 461
 * captured. The old combined X+Y magnitude averaged out this kind of
 * single-axis wobble reasonably well (uncorrelated noise on two axes
 * partially cancels in a sum-of-squares); X alone doesn't get that
 * benefit, so give-driven excursions and real deliberate tilt now
 * overlap heavily in raw MAGNITUDE alone (give: median 0.030, p90
 * 0.069, p95 0.081, max 0.106; real captured deliberate tilt from an
 * earlier round: median 0.053, p90 0.063, max 0.085 -- see
 * s_pitch_bend_max_cosine_deviation's own comment -- almost the same
 * range). What DOES differ qualitatively: real tilt sustains one sign
 * for the length of the gesture, while give flickers back and forth
 * within roughly 100-200ms as the pad settles. 15ms was nowhere near
 * long enough to exploit that difference; 120ms is a first attempt at
 * actually requiring a held direction, still well short of the
 * multi-hundred-ms "hold to arm" gate a much earlier round tried and
 * reverted -- that version's problem was a persistently-wrong BASELINE
 * (not noise) looking identical to real intent to a pure time filter,
 * which PITCH_BEND_SETTLE_MS above already handles at the source; this
 * constant only ever sees deviations from an already-settled baseline.
 * No acceleration/growth past full weight either, for the same reason
 * this constant's own history originally established.
 *
 * Lowered 120 -> 60 once X/Y/magnitude became a two-stage cascade (see
 * PITCH_BEND_SMOOTHING_ALPHA's own comment) -- real feedback: "bend
 * takes too long to start... requires max tilt for it to happen,"
 * stacked with PITCH_BEND_SETTLE_MS's own 250ms at the time (up to
 * ~370ms total before any tilt could reach full confidence -- a quick,
 * natural gesture doesn't last that long, so it read as needing more
 * ANGLE when the real bottleneck was TIME). This constant's original
 * 120ms was specifically sized to reject X-ALONE's own noise floor
 * (measured before X+Y was restored); the two-stage cascade now rejects
 * a meaningful share of that same noise UPSTREAM, before this
 * confirmation window ever sees it, so it shouldn't need to work as
 * hard downstream to finish the job. 60ms is a first attempt at
 * reclaiming some of that stacked latency without re-guessing all the
 * way back to the original (pre-X-only-round) 15ms; still needs a fresh
 * live capture to confirm real give doesn't leak back through at this
 * shorter window now that the signal feeding it is cleaner.
 *
 * Lowered again, 60 -> 30, once the adaptive depth-rate-gated
 * recentering (see PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW's own
 * comment) actually fixed pressure-coupling at its source rather than
 * fighting it downstream -- real feedback, once that worked: "wow it
 * feels good. it need a tiny bit more sensitivity and less trigger
 * time... minimum trigger time and max ease of tilt but without loosing
 * precision for regular press." With the dominant noise source (real
 * hardware "give"/pressure-coupling) now suppressed before it ever
 * reaches this confirmation window, what's left for this constant to
 * reject is mostly genuine hand tremor already reduced by the two-stage
 * cascade -- shouldn't need as long a hold to confirm as it did while
 * still fighting a noisier signal. Deliberately not lower still in this
 * same round as PITCH_BEND_DEADZONE_COSINE_DELTA's own reduction --
 * changing both at once and by a lot risks not knowing which one to
 * revert if "regular press precision" comes back regressed; a fresh
 * live capture should confirm this before either goes lower again. */
#define PITCH_BEND_ARM_MS 30u

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
 * bend," not measured against real playing.
 *
 * Now applied as a TWO-STAGE cascade (see pitch_bend_smoothed_x2/y2's
 * own struct comment) instead of a single EMA pass -- real feedback
 * after combining X+Y again still found it "too jittery, inconsistent
 * and not expressive," and actual research into this rather than
 * another guessed constant ("lets do logic and math and online
 * research for this first") found why: physiological hand tremor sits
 * in a well-characterized 6-15Hz band (most energy 7-12Hz), and a
 * SINGLE first-order EMA only rolls off at -6dB/octave -- at this
 * alpha's roughly 1.7-2Hz cutoff, 8-12Hz tremor is only attenuated to
 * roughly 18-22% of its original size, which matches exactly what real
 * playing showed: reduced, not gone. Standard cascaded-filter theory
 * (used e.g. for oscilloscope/amplifier bandwidth chaining) gives a way
 * to do meaningfully better for roughly the SAME latency budget rather
 * than just slowing this constant down further: cascaded identical
 * stages combine their rise times as a root-sum-square, not a sum, so
 * TWO stages at this SAME alpha only cost about 40% more settling time
 * (sqrt(2) ~= 1.41x) while doubling the rolloff steepness to -12dB/
 * octave -- roughly twice the tremor-band rejection (in dB) for a
 * fraction of the latency a single slower filter would need to match
 * it. This constant's own value is UNCHANGED from before -- the win
 * comes entirely from applying it twice in series, not from retuning
 * it. Still unmeasured against real playing at this specific
 * configuration; worth a fresh capture to confirm.
 *
 * The two-stage cascade above was briefly SPLIT, then briefly REMOVED,
 * then restored to exactly this uniform two-stage arrangement -- see the
 * main scan loop's own comment (at current_cosine_x/y) for the full
 * blow-by-blow of both failed intermediate attempts, real feedback
 * quotes included ("the last boot re introduced the pitch bend issues
 * with preassure," then "you've ruined the stable version... like it's
 * just reading the unfiltered numbers"). Short version: the live delta
 * and the baseline recenter target must always read the SAME cascade
 * stage (a hard invariant this file has hit more than once), and that
 * shared stage must be the fully-cascaded one, not the lighter one --
 * both terms landing on the lighter stage together technically satisfies
 * "same stage" but throws away the noise rejection PITCH_BEND_DEADZONE_
 * COSINE_DELTA and PITCH_BEND_ARM_MS were actually tuned against. This
 * constant's own tremor-rejection math above is exactly what applying it
 * twice in series, uniformly, is still buying. */
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
     * TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL..+NUM_MEMBER_CHANNELS-1)
     * while a note is held -- see claim_mpe_channel()/end_held_note()
     * below. While !tiles_expression_is_mpe_enabled(), this is always
     * TILES_MIDI_MPE_MASTER_CHANNEL instead (every note shares that
     * one channel -- see this file's own "non-MPE" section). Only
     * meaningful while state == PAD_STATE_NOTE_ON. */
    uint8_t midi_channel;
    /* Set from s_next_mpe_claim_seq at every note-on, regardless of
     * MPE mode -- a monotonic "how recently was this pad struck"
     * ordinal. Only actually consulted while !tiles_expression_is_
     * mpe_enabled() (see s_non_mpe_owner_pad's own comment for why:
     * "most recently touched/bent pad wins" needs to compare currently
     * -held pads against each other), but set unconditionally so it's
     * never stale garbage if the mode is toggled mid-performance. */
    uint32_t touch_claim_seq;

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
     * full reasoning, including why Y joined X here. NOT frozen once
     * settled -- see PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW's own
     * comment: these keep drifting toward the CURRENT x/y for as long as
     * no real bend run is active, at a rate that ADAPTS to how fast depth is
     * currently changing (fast while actively pressing/releasing, slow
     * while holding steady) -- see s->pitch_bend_prev_depth's own
     * comment and the main scan loop's "depth_activity" comment for the
     * full reasoning. This is what stops a pad's real, pressure-
     * correlated mechanical drift from reading as fake tilt, without
     * needing to know in advance what that drift curve looks like.
     *
     * Two earlier, more complicated attempts at this were tried and
     * REVERTED first: a two-depth-zone self-learning version ("pressure
     * is generating crazy pitch bend" -- too slow to converge within a
     * single note), then a fixed real-data-derived depth-vs-Y curve
     * (better, but "pressure is still doing pitch bend... takes a lot
     * of time and tilt" -- a STATIC calibration captured once can't
     * track real dynamic behavior, and doesn't reduce the noise the
     * deadzone/confirmation window still had to fight downstream). The
     * adaptive-rate approach that replaced both needs no calibration
     * capture at all -- it measures the depth/tilt relationship live,
     * on every pad, continuously. */
    float pitch_bend_baseline_x;
    float pitch_bend_baseline_y;
    /* Previous tick's s->smoothed_depth -- feeds pitch_bend_smoothed_
     * depth_rate below, which is what actually computes depth_activity
     * (see the main scan loop's own comment), the "is a real physical
     * confound currently active" signal that drives the adaptive
     * baseline-recenter rate above. Same underlying idea as the well-
     * known "One Euro Filter" (adapt a filter's own rate based on how
     * fast the tracked signal is CURRENTLY moving, not a fixed rate)
     * and, more directly, how biosignal processing handles a KNOWN
     * confound: use an independent detector of that confound (there, an
     * accelerometer measuring motion known to corrupt a heart-rate
     * sensor; here, depth's own rate of change, which correlates with
     * the mechanical drift this whole section exists to reject) to gate
     * trust in the primary signal, rather than modeling the confound's
     * effect directly. */
    float pitch_bend_prev_depth;
    /* EMA of the SIGNED tick-to-tick depth delta (not its magnitude) --
     * real feedback: "still no pitch bend on tilt but preassure works."
     * A real capture of an actual tilt gesture found depth itself
     * swinging by hundreds of raw units DURING the tilt, not just during
     * a deliberate press -- physically real: tilting a small pad with
     * one fingertip naturally shifts pressure unevenly across it, so
     * depth isn't a pure press-only signal either (the SAME cross-axis
     * coupling this whole section already fights, just running in the
     * other direction: tilt bleeding into depth, not depth bleeding into
     * X/Y). Gating the recenter rate on the RAW per-tick depth delta's
     * magnitude made this read as "depth is always actively changing,"
     * keeping the baseline in fast-snap mode almost constantly and
     * swallowing real tilt right along with real pressure changes. Fix:
     * smooth the SIGNED delta first, THEN take ITS magnitude for
     * depth_activity (see the main scan loop's own comment) -- a genuine
     * sustained press/release ramp keeps a consistent sign tick after
     * tick, so its smoothed value stays substantial; depth wobbling both
     * up and down during a tilt (no consistent direction) partially
     * cancels in a signed average instead of accumulating, reading as
     * LOWER activity. The exact same "sign consistency separates real
     * motion from noise" principle this file's own pitch-bend run-
     * tracking already uses for X/Y, applied here to depth instead. */
    float pitch_bend_smoothed_depth_rate;
    /* Used BEFORE baseline_settled to arrive at a clean initial baseline
     * capture (see PITCH_BEND_SETTLE_MS) -- AND kept running every tick
     * afterward too (real feedback: "we need to compensate for
     * preassure depth and drift"), now doubling as the RECENTER target
     * for PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW/_FAST below, instead
     * of that recenter step chasing raw X/Y directly. Deliberately still
     * separate from `pitch_bend_baseline_x/y` -- this is the medium-
     * timescale "where does the pad seem to be settling right now"
     * signal (PITCH_BEND_SMOOTHING_ALPHA, ~100-200ms), baseline_x/y is
     * the slower "true rest center" signal (PITCH_BEND_BASELINE_
     * RECENTER_ALPHA_SLOW/_FAST, adaptive) that chases THIS, not raw
     * samples -- see that constant's own comment for why a raw-noise-
     * chasing baseline was itself the actual problem a real deliberate,
     * slowly-leaned hold exposed. */
    float pitch_bend_smoothed_x;
    float pitch_bend_smoothed_y;
    /* Second, more heavily cascaded stage -- see PITCH_BEND_SMOOTHING_
     * ALPHA's own comment for the cascaded-filter math. BOTH the live
     * delta computation (current_cosine_x/y) AND the baseline recenter
     * target read THIS stage, not the lighter one above -- see the main
     * scan loop's own comment for why a real attempt at splitting them
     * across the two stages (to let fast wiggles through) made things
     * WORSE, not better, twice over. */
    float pitch_bend_smoothed_x2;
    float pitch_bend_smoothed_y2;
    /* Single EMA stage -- magnitude never needed a second cascade stage
     * the way X/Y did: it's read identically by both the "predicted
     * baseline" and "current" terms below (see that computation's own
     * comment), so there's only ever one lag to match against itself
     * regardless of how heavily it's filtered. */
    float pitch_bend_smoothed_magnitude;
    /* Magnitude actually used by BOTH cosine terms below -- see the main
     * scan loop's own comment (where this is chased/frozen, mirroring
     * pitch_bend_baseline_x/y exactly) for why a real held tilt still
     * needs a FROZEN magnitude, not the live one, once a run is
     * confirmed. */
    float pitch_bend_run_magnitude;
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
    /* Independent fast-wiggle-vibrato detector -- see VIBRATO_ENERGY_
     * NOISE_FLOOR's own comment for the real capture behind this and
     * why it's a SEPARATE signal from everything above, not a change to
     * it. pitch_bend_wiggle_energy is the smoothed magnitude of the gap
     * between the two existing cascade stages; pitch_bend_wiggle_active/
     * _start_ms are the same "how long has this been true" confirmation
     * shape as pitch_bend_run_active/_start_ms above, applied to "is
     * wiggle energy currently above the noise floor" instead of "is
     * delta currently outside the deadzone". */
    float pitch_bend_wiggle_energy;
    bool pitch_bend_wiggle_active;
    uint32_t pitch_bend_wiggle_start_ms;
} pad_expr_t;

static pad_expr_t s_pads[TILES_NUM_PADS];

/* The player's own single on/off preference for pitch bend (see
 * tiles_expression_toggle_pitch_bend()) -- separate from each pad's own
 * per-note pitch_bend_active above, which latches whatever this was at
 * the moment that specific note fired. */
static bool s_pitch_bend_enabled;


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

#if TILES_MELODIC_HARMONICS_ENABLED
/* ============================================================================
 * Melodic harmonics (board 2 only -- see TILES_MELODIC_HARMONICS_ENABLED's
 * own comment above). Real feedback: "capacitive touch only plays the
 * respective harmonics of the note being played by a pad with real
 * pressure. this behaviour only happens when a single pad is being
 * pressed not in poliphony." No established hardware convention for
 * this exists (checked) -- closest musical analogue is a piano's
 * sympathetic resonance, not guitar harmonics (those are positional on
 * one string; this is cross-pad). Design confirmed with the player
 * rather than guessed:
 *
 *   - Mapping: by TOUCH ORDER, not pad position. The 1st other pad
 *     touched (while eligible) sounds the octave (2nd harmonic), the
 *     2nd sounds the octave+fifth (3rd), and so on through
 *     HARMONIC_SEMITONES below. Position-relative-to-the-fundamental
 *     was the rejected alternative -- it reads more "physical," but a
 *     diatonic scale layout doesn't land pads on true harmonic ratios
 *     and a key/scale change would silently reshuffle which pad is
 *     "which harmonic."
 *   - Harmonics 2-5 only (octave / octave+fifth / 2 octaves / 2
 *     octaves+major third) -- higher partials detune noticeably and
 *     cluster too close together in 12-tone equal temperament to read
 *     as distinct pitches.
 *   - Fixed, gentle velocity (HARMONIC_VELOCITY) regardless of how hard
 *     the fundamental is pressed -- the rejected alternative (volume
 *     follows the fundamental's live pressure) is more true to real
 *     sympathetic resonance but adds a second continuous-controller
 *     stream this first version deliberately skips.
 *   - A harmonic pad that gets pressed hard enough to cross this file's
 *     own real-strike threshold (MIN_STRIKE_DEPTH_DELTA) is left
 *     completely alone by this section -- its OWN, unmodified
 *     PAD_STATE_IDLE -> AWAITING_STRIKE -> NOTE_ON pipeline just runs
 *     as it always has, the same real-strike code every other pad on
 *     every other board uses, and promotes it to a genuine independent
 *     note. This section only has to notice that on the very next
 *     scan (find_sole_held_pad() below stops returning exactly one
 *     pad once a second note is genuinely held) and tear down every
 *     harmonic voice in response -- "not in polyphony" enforces
 *     itself, no special-casing needed for the promotion itself.
 *
 * Channel budget: harmonics never steal a Member Channel from a real
 * note, and a real note may always steal one back from a harmonic (see
 * harmonic_channel_is_reserved()'s own comment) -- reuses this file's
 * existing "reserved channel" mechanism (the same one that already
 * protects the sequencer's own lanes in claim_mpe_channel() below)
 * rather than teaching that carefully-tuned stealing logic a second,
 * different kind of steal. The real cost: HARMONIC_MAX_VOICES of the
 * 15 Member Channels are permanently held back from real polyphony
 * while this build is running, 11 left for genuine notes -- a real,
 * explicit tradeoff of this being scoped to one board's experiment
 * rather than the default build.
 */

/* 2nd through 5th harmonic, natural harmonic series in semitones above
 * the fundamental, rounded to the nearest equal-tempered semitone:
 * +12 octave, +19 octave+fifth, +24 two octaves, +28 two octaves+major
 * third. Index 0 = the 1st OTHER pad touched, per this section's own
 * header comment. */
#define HARMONIC_MAX_VOICES 4u
static const uint8_t HARMONIC_SEMITONES[HARMONIC_MAX_VOICES] = {12u, 19u, 24u, 28u};

/* Fixed, deliberately gentle -- roughly a third of full scale, meant to
 * read as an overtone shimmering under the real note, not as a second
 * equal voice. Unmeasured, like every first-pass constant in this
 * file -- not felt on real hardware yet. */
#define HARMONIC_VELOCITY 40u

/* Real feedback: "also fine tune palm rejection and accidental
 * touches" -- then, correcting this section's first attempt: "palm
 * rejection is more of a multiple harmonics detected at once like
 * resting hand by accident. faster brushing or struming withing
 * reasonable human capability should be detected as intentional."
 *
 * HARMONIC_TOUCH_DWELL_MS is unaffected by that correction -- a pad
 * must stay continuously touched this long before it's even a
 * candidate, filtering a brief single-pad graze. Still scoped only to
 * this new path; MIN_STRIKE_DEPTH_DELTA already requires genuine
 * pressure for the real note pipeline everywhere else, so a bare touch
 * was always harmless there.
 *
 * The multi-pad veto below is a full redesign, not a tuning pass. The
 * first version vetoed on how close together several pads' touches
 * BEGAN (onset timing) -- exactly what a fast, deliberate strum/brush
 * also does, so it rejected genuine playing along with real accidents.
 * Onset timing was never the right signal. Real trackpads don't use it
 * either: the actual palm signal is contact SIZE and PERSISTENCE -- a
 * palm is one large, mostly-stationary contact that arrives and STAYS;
 * a finger, or several fingers/a pick sweeping through a strum, is
 * small and keeps moving, so even a fast gesture that briefly touches
 * several pads at once doesn't keep them all down together. This
 * hardware has no true contact-area sensing (each pad is a discrete
 * touched/not-touched switch, not a continuous surface), so the closest
 * available analog is COUNT sustained over TIME: how many candidate
 * pads are touched at the same instant, and whether that count keeps
 * being true rather than passing through. HARMONIC_PALM_MIN_SIMULTANEOUS
 * candidates all touched at once, continuously for HARMONIC_PALM_
 * SUSTAIN_MS, reads as a resting palm; anything shorter -- however tight
 * the individual touches land in time -- doesn't, regardless of how
 * fast the gesture was. Both unmeasured, like every first-pass constant
 * in this file -- not tuned against a real palm or a real strum on real
 * hardware yet. */
#define HARMONIC_TOUCH_DWELL_MS 25u
#define HARMONIC_PALM_MIN_SIMULTANEOUS 3u
#define HARMONIC_PALM_SUSTAIN_MS 150u

typedef struct {
    bool active;
    uint8_t pad;          /* 1..TILES_NUM_PADS, valid only while active */
    uint8_t note;
    uint8_t midi_channel;
} harmonic_voice_t;

static harmonic_voice_t s_harmonic_voices[HARMONIC_MAX_VOICES];
/* 0 = no harmonics session in progress. Set the instant find_sole_held_
 * pad() below finds exactly one held pad; cleared (and every active
 * voice torn down) the instant it stops being exactly that same pad. */
static uint8_t s_harmonic_fundamental_pad;
/* 0 = not currently in a high-simultaneous-touch episode -- see
 * HARMONIC_PALM_SUSTAIN_MS's own comment. Set to the scan timestamp
 * the instant the candidate count first reaches HARMONIC_PALM_MIN_
 * SIMULTANEOUS; reset to 0 the instant it drops back below. */
static uint32_t s_harmonic_high_count_since_ms;

/* The LAST HARMONIC_MAX_VOICES Member Channels, permanently carved out
 * for harmonics only -- see this section's own header comment on why a
 * separate reserved range, not a second stealing policy, is what keeps
 * a harmonic from ever being able to take a channel a real note needs.
 * claim_mpe_channel() below skips every channel this covers, exactly
 * like it already skips the sequencer's own reserved lanes. */
static bool harmonic_channel_is_reserved(uint8_t channel) {
    return channel > (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + TILES_MIDI_MPE_NUM_MEMBER_CHANNELS -
                                1u - HARMONIC_MAX_VOICES);
}

/* Pad currently in PAD_STATE_NOTE_ON, IF exactly one is -- 0 if none or
 * more than one are held. O(24), cheap, called once per scan. */
static uint8_t find_sole_held_pad(void) {
    uint8_t found = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        if (s_pads[i].state == PAD_STATE_NOTE_ON) {
            if (found != 0u) {
                return 0u; /* a second held pad -- not sole, stop looking */
            }
            found = (uint8_t)(i + 1u);
        }
    }
    return found;
}

static void end_harmonic_voice(uint8_t idx) {
    harmonic_voice_t *v = &s_harmonic_voices[idx];
    if (!v->active) {
        return;
    }
    tiles_midi_note_off(v->midi_channel, v->note);
    /* Deliberately NOT tiles_cv_gate_note_off()/_note_on() below -- CV/
     * gate is explicitly monophonic, tracking whichever note claimed it
     * most recently (see cv_gate.h's own last-note-priority arbitration).
     * A harmonic voice competing for that same single analog output
     * would let a light touch steal the pitch/gate signal away from the
     * genuine fundamental note it's supposed to be shimmering under --
     * exactly the kind of interference a "quiet overtone" feature must
     * not cause. CV/gate only ever reflects real, pressed notes. */
    v->active = false;
}

static void end_all_harmonic_voices(void) {
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        end_harmonic_voice(i);
    }
    s_harmonic_fundamental_pad = 0u;
    s_harmonic_high_count_since_ms = 0u;
}

/* No stealing in either direction: scans only the reserved range above,
 * and simply returns 0xFF (no voice starts) if every reserved channel
 * already holds a harmonic -- see this section's own header comment on
 * why harmonics are deliberately the lowest-priority thing on this
 * board's MPE bus. */
static uint8_t claim_harmonic_channel(void) {
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        uint8_t channel = (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i);
        if (harmonic_channel_is_reserved(channel) && !s_mpe_channels[i].in_use) {
            s_mpe_channels[i].in_use = true;
            /* owner_pad = 0 -- a real note's pad is always 1..24 (see
             * mpe_channel_slot_t's own comment), so this both reads as
             * "not owned by a real held pad" and can never collide with
             * a genuine pad number. claim_mpe_channel() below already
             * skips this whole range for real notes regardless, so
             * nothing ever inspects this field for a harmonic-held
             * channel -- set purely so a slot showing in_use=true is
             * never mistaken for stale/uninitialized data if this file
             * is debugged later. */
            s_mpe_channels[i].owner_pad = 0u;
            s_mpe_channels[i].claim_seq = s_next_mpe_claim_seq++;
            return channel;
        }
    }
    return 0xFFu;
}

/* Called once per scan, after the main per-pad loop below (so every
 * pad's PAD_STATE_NOTE_ON is current for this tick first). Real
 * feedback's full gesture in one pass:
 *   1. Exactly one pad held elsewhere in this file -> that's the
 *      fundamental; anything else (none held, or a second pad just
 *      promoted to a real note) ends every harmonic voice outright.
 *   2. For every OTHER pad currently touched and not itself mid-strike
 *      or already a real note, start a harmonic voice in the first free
 *      slot if it doesn't have one yet.
 *   3. For every pad with an active voice that's no longer eligible
 *      (released, or promoted to a real note itself), end it.
 */
static void scan_melodic_harmonics(uint32_t now_ms) {
    if (!tiles_op_mode_is_melodic_active()) {
        if (s_harmonic_fundamental_pad != 0u) {
            end_all_harmonic_voices();
        }
        return;
    }

    uint8_t fundamental = find_sole_held_pad();
    if (fundamental != s_harmonic_fundamental_pad) {
        end_all_harmonic_voices();
        s_harmonic_fundamental_pad = fundamental;
    }
    if (fundamental == 0u) {
        return;
    }
    uint8_t fundamental_note = s_pads[fundamental - 1u].active_note;

    /* End any voice whose pad is no longer eligible FIRST, so a pad that
     * just released frees its slot for a different pad to claim this
     * same scan rather than waiting a tick. */
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        harmonic_voice_t *v = &s_harmonic_voices[i];
        if (!v->active) {
            continue;
        }
        pad_expr_t *ps = &s_pads[v->pad - 1u];
        if (ps->state == PAD_STATE_NOTE_ON || !tiles_touch_is_touched(v->pad)) {
            end_harmonic_voice(i);
        }
    }

    /* First pass: which pads are even candidates this scan (touched,
     * not the fundamental, not already a real note or an existing
     * harmonic voice, and past HARMONIC_TOUCH_DWELL_MS) -- built before
     * any of them can claim a channel, so the sustained-count palm check
     * just below sees this whole scan's real candidate count at once,
     * not a partial one still being accumulated mid-loop. */
    bool candidate[TILES_NUM_PADS + 1u] = {false}; /* 1-indexed, [0] unused */
    uint8_t candidate_count = 0u;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (pad == fundamental) {
            continue;
        }
        pad_expr_t *ps = &s_pads[pad - 1u];
        if (ps->state == PAD_STATE_NOTE_ON || !tiles_touch_is_touched(pad)) {
            continue;
        }
        if ((now_ms - ps->touch_start_ms) < HARMONIC_TOUCH_DWELL_MS) {
            continue; /* hasn't dwelled long enough yet -- try again next scan */
        }
        bool already_voiced = false;
        for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
            if (s_harmonic_voices[i].active && s_harmonic_voices[i].pad == pad) {
                already_voiced = true;
                break;
            }
        }
        if (already_voiced) {
            continue;
        }
        candidate[pad] = true;
        candidate_count++;
    }

    /* Count EVERY currently-touched other pad here, not just fresh
     * dwell-cleared candidates -- an already-sounding harmonic voice's
     * pad is still genuinely touched right now, and a palm landing
     * partway through an existing legitimate session must still be
     * recognized (it doesn't only count pads that are brand new this
     * scan). */
    uint8_t simultaneous = candidate_count;
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        if (s_harmonic_voices[i].active && !candidate[s_harmonic_voices[i].pad]) {
            simultaneous++;
        }
    }
    bool palm_rejected = false;
    if (simultaneous >= HARMONIC_PALM_MIN_SIMULTANEOUS) {
        if (s_harmonic_high_count_since_ms == 0u) {
            s_harmonic_high_count_since_ms = now_ms;
        } else if ((now_ms - s_harmonic_high_count_since_ms) >= HARMONIC_PALM_SUSTAIN_MS) {
            palm_rejected = true;
        }
    } else {
        s_harmonic_high_count_since_ms = 0u;
    }

    if (palm_rejected) {
        /* Confirmed: several pads have been touched together long
         * enough to be a resting palm, not a fast strum passing
         * through. Can't reliably tell which currently-touched pads
         * are "the palm" and which might be a genuine touch caught in
         * the same episode with only binary touch data -- errs toward
         * silence (see this section's own header) and drops all of
         * them, new and already-sounding alike, for as long as the
         * high count persists. */
        printf("[expression] harmonics: %u pads touched together >= %ums -- palm rejected\n", (unsigned)simultaneous,
               (unsigned)HARMONIC_PALM_SUSTAIN_MS);
        for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
            end_harmonic_voice(i);
        }
        return;
    }

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (!candidate[pad]) {
            continue;
        }
        int8_t free_slot = -1;
        for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
            if (!s_harmonic_voices[i].active) {
                free_slot = (int8_t)i;
                break;
            }
        }
        if (free_slot < 0) {
            continue;
        }
        uint8_t channel = claim_harmonic_channel();
        if (channel == 0xFFu) {
            continue; /* every reserved channel already holds a harmonic */
        }
        int note = (int)fundamental_note + (int)HARMONIC_SEMITONES[free_slot];
        if (note > 127) {
            continue; /* out of MIDI range this high -- silently skip, don't clamp into a wrong pitch */
        }
        harmonic_voice_t *v = &s_harmonic_voices[free_slot];
        v->active = true;
        v->pad = pad;
        v->note = (uint8_t)note;
        v->midi_channel = channel;
        tiles_midi_note_on(channel, v->note, HARMONIC_VELOCITY);
    }
}
#endif /* TILES_MELODIC_HARMONICS_ENABLED */


/* ---- Non-MPE compatibility mode -----------------------------------------
 * Real feedback: "lets make sure the pitch bend works with non mpe
 * layouts meaning pitch bend wheel... look for the max most
 * compatible and standardized version." Default true (MPE, this
 * file's original and still-recommended behavior) -- see tiles_
 * expression_set_mpe_enabled()'s own header-comment for what flipping
 * this actually changes. */
static bool s_mpe_enabled = true;

/* Which pad currently drives the shared master channel's continuous
 * controllers (pitch bend, channel pressure) while !s_mpe_enabled --
 * 0 means no pad is currently held at all. Real feedback: "most
 * recently touched/bent pad wins" -- set to the newly-struck pad on
 * every note-on (see the note-on site below), and handed back to
 * whichever OTHER currently-held pad has the highest touch_claim_seq
 * when the current owner releases (see end_held_note()'s own non-MPE
 * branch) -- never just reset to "none" while some other pad is still
 * genuinely held, the same way a real pitch-bend wheel doesn't stop
 * mattering just because a second finger also pressed a key. Unused,
 * and left at whatever it last was, while s_mpe_enabled is true. */
static uint8_t s_non_mpe_owner_pad;

/* "Expression mute" -- a hard kill switch for pitch bend and poly
 * aftertouch, deliberately separate from s_pitch_bend_enabled above
 * (that's the player's own on/off preference; this overrides it
 * entirely, on top, without disturbing what it was set to) -- unmuting
 * restores exactly whatever tiles_expression_toggle_pitch_bend() state
 * was already in effect before muting. Note-on/off/velocity are read
 * directly from touch+Hall, never gated by this flag -- see
 * tiles_expression_set_muted()'s own comment for the full history,
 * including why this can no longer actually be triggered by anything
 * in services/expression_control.c as of the MPE-toggle gesture that
 * replaced it. */
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
    s_mpe_enabled = true;
    s_non_mpe_owner_pad = 0u;
    s_pitch_bend_enabled = false;
    s_expression_muted = false;
#if TILES_MELODIC_HARMONICS_ENABLED
    for (uint8_t i = 0; i < HARMONIC_MAX_VOICES; i++) {
        s_harmonic_voices[i] = (harmonic_voice_t){0};
    }
    s_harmonic_fundamental_pad = 0u;
    s_harmonic_high_count_since_ms = 0u;
#endif
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

/* The MPE specification's own recommended Pitch Bend Sensitivity default
 * -- what a receiver falls back to if it never honors an incoming RPN 0
 * override (see TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES's own comment
 * in midi_out.h for the full history of trying to fix that via RPN).
 * Used below to defensively compensate the WIRE value itself, independent
 * of whether any given receiver actually listens to that RPN. */
#define PITCH_BEND_MPE_SPEC_DEFAULT_RANGE_SEMITONES 48.0f

/* How much of the full +/-8191 wire range this device ever actually
 * sends, at maximum tilt -- real feedback found sending RPN 0 (Pitch
 * Bend Sensitivity = TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, 12) is
 * NOT enough on its own: "tried serum and also is bending too far. so
 * its not roli. the tilt pushes too far" -- reported on Xfer Serum,
 * confirming the earlier ROLI Equator report ("still does more in
 * Equator mpe mode" even with the RPN sent on every Member Channel) was
 * never a ROLI-specific quirk. Two unrelated synths from two unrelated
 * vendors, both still swinging like a full 48-semitone (4-octave) range
 * at max tilt despite this device declaring 12 -- the common factor
 * isn't either receiver's own quirks, it's that dynamically honoring a
 * THIRD-PARTY controller's Pitch Bend Sensitivity RPN is, in practice,
 * not something real-world MPE hosts/plugins reliably do, even though
 * it's spec-legal; ROLI's own docs confirm Equator treats its Pitch Bend
 * Range as a value the USER manually sets to match the controller, not
 * one it negotiates automatically, and Serum showing the identical
 * symptom suggests the same is true there too.
 *
 * Rather than keep trusting RPN negotiation (already tried twice: once
 * on the Master Channel only, once redundantly on every Member Channel
 * too -- both left in place, since they're correct and harmless for any
 * receiver that DOES honor them), compensate defensively at the actual
 * wire value instead: assume the worst realistic case -- a receiver
 * that ignores the RPN entirely and uses the spec's own 48-semitone
 * default -- and scale this device's own max output down to whatever
 * fraction of 48 semitones its declared TILES_MIDI_MPE_PITCH_BEND_RANGE_
 * SEMITONES (12) actually is. At max tilt, the wire value now only ever
 * reaches 12/48 = 25% of full scale, so even a receiver stuck at the
 * 48-semitone default still produces the intended ~12-semitone swing.
 *
 * Real, documented tradeoff: a receiver that DOES correctly honor the
 * RPN (none confirmed yet, out of two tested) would now see a much
 * narrower ~3-semitone actual range (12 * 0.25), not the full 12 --
 * worth revisiting if such a receiver is ever found and this then reads
 * as needing MORE tilt for a full bend specifically there. Until then,
 * matching the two real receivers actually tested is the right default. */
#define PITCH_BEND_WIRE_RANGE_COMPENSATION \
    ((float)TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES / PITCH_BEND_MPE_SPEC_DEFAULT_RANGE_SEMITONES)

/* An exponential ease-out response curve (y = (1-e^(-kx))/(1-e^(-k)))
 * was tried here and REVERTED -- real feedback: "preassurte is afecting
 * big tilt. big tilt should be an exponential curve that reaches the
 * octave not a contant jittery" led to trying exactly that shape
 * (steepest slope near x=0, flattening toward x=1, so noise near max
 * tilt would move the output less) -- but real feedback on the very
 * next boot: "you made the tilt still weird and also preaqssure change
 * is affecting tilt." Two real problems with that attempt, not one:
 *
 * 1. Implementation bug: the curve was applied to `ratio` AFTER
 * PITCH_BEND_ARM_MS's own confidence multiplier had already been folded
 * in (ratio *= confidence, THEN curve(ratio)) -- since the curve's
 * slope is steepest near 0, this amplified the temporal confidence
 * RAMP-IN itself, not just the spatial tilt amount. A tilt at full
 * magnitude but only 10% into its 30ms confirmation window (ratio=0.1
 * effective) got curved to ~0.27, nearly 3x what the linear ramp-in was
 * designed to output at that point -- compressing/distorting exactly
 * the first 30ms of every single gesture, which is also when residual
 * noise (pressure or otherwise) is most present before a hold settles.
 *
 * 2. More fundamental: ANY curve satisfying y(0)=0 and y(1)=1 must, by
 * the mean value theorem, have average slope exactly 1 over [0,1] --
 * reducing sensitivity somewhere (near x=1, to fight top-of-range
 * jitter) mathematically REQUIRES increasing it somewhere else (this
 * curve's own steep near-x=0 slope). That's not a bug, it's what this
 * whole class of fix does by construction -- it redistributes where
 * noise is visible across the range, it doesn't reduce the noise
 * itself. Bug (1) made this worse than the tradeoff alone would have,
 * but the tradeoff itself means this approach was never going to be a
 * clean win: ordinary small-to-moderate tilts (where real playing
 * spends most of its time, and where pressure-coupling residual noise
 * already lives) got MORE sensitive, not less, in exchange for a
 * calmer arrival at the rarely-reached extreme. Reverted to the plain
 * linear mapping this file already had tuned; a future attempt at
 * smoothing the top of the range specifically should restrict any
 * reshaping to a narrow region near the clamp (e.g. identity below some
 * knee point, eased only above it) rather than reshaping the whole
 * [0,1] domain, so the bulk of ordinary playing is never touched. */

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
        /* A brand-new run starts ONLY on the first deviation since the
         * deadzone, NOT on every direction reversal -- real feedback
         * combining X+Y again, on a real firm press+tilt-left+tilt-
         * right+center gesture: "too jittery, inconsistent and not
         * expressive with tilt, just jittery on press." A live capture
         * showed the COMBINED magnitude staying genuinely elevated
         * (real signal, well above the "give"-only noise floor
         * characterized during the X-only debugging round) for nearly
         * the entire gesture, but delta_x's own raw sign -- which
         * `positive` above still is, unchanged -- kept flipping almost
         * every sample even during that real, sustained deflection
         * (holding a pad tilted, even in one intended direction, wobbles
         * in exact angle more than it wobbles in exact PRESENCE of
         * deflection). Resetting the confirmation window on every one of
         * those flips meant confidence could never accumulate past a
         * few ms before restarting from zero, which is exactly "jittery,
         * never confident" -- and see this file's own PITCH_BEND_
         * BASELINE_RECENTER_ALPHA comment for the other half of the same
         * bug: pitch_bend_run_active gates baseline recentering too, so
         * a run that can never stay confirmed also means the baseline
         * never stops chasing, actively erasing the real signal. A
         * direction reversal no longer resets `run_active` or
         * `run_start_ms` -- only actually returning to the deadzone
         * does. `run_positive` is still updated every tick (kept for
         * completeness/future use) but no longer read to decide whether
         * to reset -- `sign` below, taken fresh from THIS tick's
         * `positive`, already gives the output the correct polarity on
         * every single tick regardless of what the run's own history is,
         * so output direction was never actually at risk from this
         * change -- only how much CONFIDENCE (via hold_ms below) a
         * wobbly-but-real gesture is allowed to accumulate. */
        if (!s->pitch_bend_run_active) {
            s->pitch_bend_run_active = true;
            s->pitch_bend_run_start_ms = now_ms;
        }
        s->pitch_bend_run_positive = positive;
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

/* Independent fast-wiggle-vibrato detector -- deliberately separate from
 * everything above (pitch_bend_smoothed_x/y/x2/y2, baseline_x/y,
 * run_magnitude) and everything below (pitch_bend_apply_vibrato()'s own
 * additive output), per this file's own hard-won lesson: TWO earlier
 * attempts at fast-wiggle support that touched the SHARED tilt/pressure
 * signal both made pressure stability worse, not better (see
 * PITCH_BEND_SMOOTHING_ALPHA's own history and current_cosine_x/y's
 * "Attempt 1"/"Attempt 2" comments). This round reads two values that
 * already exist (pitch_bend_smoothed_x/y, the lighter first cascade
 * stage, and _x2/_y2, the fully-cascaded second stage) but never
 * modifies them, and its own output is added on top of the final wire
 * value, after everything above has already run -- it cannot affect,
 * and cannot be affected by, the baseline/current-cosine computation
 * the stability fixes depend on.
 *
 * Real feedback: "maybe fast wiggles can be tected and that activates
 * bivrato? i just need fast wiggles of the keys to activate bedn as
 * well." Researched real precedent first (LinnStrument's own docs:
 * "wobble a finger left and right to generate vibrato" on the SAME
 * pitch-bend path used for slides; Haken Continuum: "convert finger
 * tremble into vibrato" via direct high-resolution tracking) -- neither
 * needs a separate detector, because their sensors are precise/
 * responsive enough that one live signal handles both slow tilts and
 * fast wiggles. This hardware can't do that directly: its 2-stage
 * cascade (needed for the cross-axis pressure-coupling this file has
 * fought all session) rolls off a real wiggle's own amplitude right
 * along with tremor, confirmed with real numbers rather than assumed --
 * a captured ~5.5s deliberate tilt hold reached 94% of max bend output,
 * a ~8.6s fast wiggle on the same pad never exceeded 12%, even though
 * their RAW motion amplitude was comparable (this file's own "[wiggle-
 * cap]" capture, since removed).
 *
 * The fix that capture's own data ruled out: lightening the shared
 * live signal (tried twice, both broke pressure stability). The fix
 * that worked: measure how much the LIGHTER first cascade stage
 * disagrees with the fully-cascaded second stage -- during a slow,
 * settled signal (steady press, or a tilt held long enough for both
 * stages to converge), that gap is small; during a genuine fast
 * wiggle, the light stage tracks the oscillation while the heavy stage
 * can't keep up, so the gap is large AND stays large for the duration.
 * A dedicated three-gesture real capture on one pad (hold still,
 * deliberate tilt, fast wiggle -- this file's own "[vib-cap]" capture,
 * since removed) measured the smoothed magnitude of that gap directly:
 *   - hold still:      mean 7.5, sustained energy never exceeds ~9
 *   - deliberate tilt:  mean 8.5, one single-tick blip to ~12 (at
 *                        RELEASE, not onset -- see VIBRATO_ARM_MS below)
 *   - fast wiggle:      mean 16.2, energy stays above 11 for 93% of
 *                        the gesture's whole duration
 * Onset (the first 300ms of every strike, all three gestures) stayed
 * under 7 -- an ordinary strike's own fast depth ramp does not spike
 * this signal. */
#define VIBRATO_ENERGY_NOISE_FLOOR 8.0f
/* Above this, wiggle depth is full-scale (before VIBRATO_ARM_MS's own
 * confirmation ramp) -- see VIBRATO_ENERGY_NOISE_FLOOR's own comment for
 * the real capture (sustained ~16.2 during a genuine wiggle) this is
 * measured against. The 8..16 span between the two constants is a soft
 * knee, not a hard cutoff -- ramps continuously so a borderline gesture
 * doesn't click a vibrato on/off, same reasoning as PITCH_BEND_DEADZONE_
 * COSINE_DELTA's own soft-knee treatment. */
#define VIBRATO_ENERGY_FULL_SCALE 16.0f
/* Requires wiggle energy to stay above VIBRATO_ENERGY_NOISE_FLOOR for
 * this long before vibrato ramps in at all -- same "hold to confirm"
 * shape as PITCH_BEND_ARM_MS, applied here specifically because the real
 * capture found the one place a NON-wiggle gesture came close to the
 * floor was a single tick at the exact instant of RELEASING a held tilt
 * (a fast depth-ramp event, same class of transient as every other
 * cross-axis-coupling artifact this file has fought, just at release
 * instead of onset) -- harmless there on its own since pitch bend resets
 * to center on note-off regardless of what this detector is doing, but
 * a similarly brief transient from a deliberate hard press MID-hold
 * (untested; no capture of that specific gesture yet) is exactly the
 * kind of thing this file's history says not to assume away. A real
 * wiggle's own 93%-of-duration coverage sails through this easily; a
 * one-or-two-tick transient does not. */
#define VIBRATO_ARM_MS 80u
/* Fixed vibrato rate, not derived from the player's own gesture speed --
 * deliberately simple for a first attempt rather than trying to extract
 * a stable rate/phase estimate from an already-small, jittery energy
 * signal. 5.5Hz sits in the commonly-cited natural vibrato range for
 * strings/voice (this file's own tremor-vs-vibrato research already put
 * genuine vibrato at 4-8Hz); worth revisiting if a future round finds
 * a fixed rate feels robotic against real playing. */
#define VIBRATO_RATE_HZ 5.5f
/* Peak wire-unit amplitude of the added oscillation at full depth --
 * roughly 0.9 semitones peak (see PITCH_BEND_WIRE_RANGE_COMPENSATION's
 * own comment: this device's post-compensation full-scale bend is
 * ~2047 wire units for TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES, 12
 * semitones, so 1 semitone is roughly 2047/12 =~ 171 wire units) -- a
 * musical/design choice, not a hardware measurement, same as
 * s_pitch_bend_max_cosine_deviation's own history; worth tuning against
 * real playing like every other constant in this pipeline. */
#define VIBRATO_MAX_WIRE_DEPTH 150.0f
/* Not M_PI -- not guaranteed available without a feature-test macro on
 * this newlib target; services/standby.c already established the
 * pattern of defining pi locally rather than relying on it (see
 * TILES_STANDBY_PI there). */
#define TILES_EXPRESSION_VIBRATO_PI 3.14159265358979323846f

/* Applies the independent wiggle-vibrato detector's output to an
 * already-fully-computed wire bend value -- called AFTER
 * pitch_bend_14bit_from_cosine_delta() returns, never before, so
 * nothing here can influence the baseline/current-cosine computation
 * that function and its caller depend on. See VIBRATO_ENERGY_NOISE_
 * FLOOR's own comment for the full reasoning and the real capture
 * behind every constant used here. */
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

    /* Phase reduced via INTEGER modulo on now_ms before ever touching a
     * float, not sinf(2*pi*rate*now_ms/1000.0f) directly -- casting a
     * multi-million-ms uptime straight to float first loses enough
     * mantissa precision to visibly drift this LFO's phase over long
     * continuous uptimes (a plain uint32_t now_ms already exceeds
     * float's exact-integer range well within a day of uptime). Taking
     * the remainder in exact uint32_t arithmetic first means only the
     * small, always-precise phase-within-one-period value ever reaches
     * sinf(). */
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

float tiles_expression_get_pitch_bend_sensitivity(void) {
    return s_pitch_bend_max_cosine_deviation;
}

void tiles_expression_set_aftertouch_sensitivity(uint16_t depth_full_scale) {
    /* Never 0 -- aftertouch_from_depth() divides by this. */
    s_depth_to_aftertouch_full_scale = depth_full_scale > 0u ? depth_full_scale : 1u;
    printf("[expression] aftertouch sensitivity (full-scale depth) now %u\n", s_depth_to_aftertouch_full_scale);
}

uint16_t tiles_expression_get_aftertouch_sensitivity(void) {
    return s_depth_to_aftertouch_full_scale;
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

/* Real feedback: "lets make sure the pitch bend works with non mpe
 * layouts meaning pitch bend wheel." Deliberately does NOT retroactively
 * touch any note already held at the moment this is flipped -- a note
 * struck under the OLD scheme keeps whatever channel it already claimed
 * (its own Member Channel under MPE, or the shared master channel under
 * non-MPE) until its own note-off, exactly the same "toggling mid-hold
 * doesn't retroactively change an already-sounding note" precedent this
 * file's own pitch_bend_active field already establishes elsewhere. Only
 * the NEXT note-on picks up the new routing. s_non_mpe_owner_pad is left
 * as-is rather than force-reset here for the same reason -- it's simply
 * unconsulted while s_mpe_enabled is true, and the first note struck
 * after switching back to non-MPE will correctly claim it fresh. */
void tiles_expression_set_mpe_enabled(bool enabled) {
    s_mpe_enabled = enabled;
    printf("[expression] MPE mode %s\n", enabled ? "enabled" : "disabled (single-channel, standard MIDI)");
}

bool tiles_expression_is_mpe_enabled(void) {
    return s_mpe_enabled;
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

/* Time-only sub-score, 0..1 -- "how much of the way from slow to fast,"
 * curved by VELOCITY_CURVE_EXPONENT (see that constant's own comment;
 * 1.0 is currently a plain linear response). Split out of
 * velocity_from_strike() below so that function reads as "combine two
 * scores" rather than one tangled formula. */
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

/* Depth-only sub-score, 0..1 -- how far peak_depth had already overshot
 * MIN_STRIKE_DEPTH_DELTA at the exact sample that crossed it. See this
 * file's "Velocity" section header for why this exists (time-to-
 * threshold alone can't tell a light quick tap from a hard quick
 * strike) and why it's now the dominant signal. */
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

/* Maps a strike's elapsed time (touch_start_sample_ms to the moment
 * peak_depth crossed MIN_STRIKE_DEPTH_DELTA) AND how far peak_depth had
 * already overshot that threshold at that exact sample to a MIDI
 * velocity -- see this file's "Velocity" section for the full
 * reasoning. Depth leads (STRIKE_DEPTH_WEIGHT), time is the remaining
 * bias -- faster and/or a bigger overshoot both push toward a harder
 * strike. */
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

/* Thin public wrapper -- see this function's own declaration in
 * expression.h for why services/op_mode.c needs it and what its two
 * inputs must be measured against. */
uint8_t tiles_expression_velocity_from_strike(uint32_t strike_time_ms, float peak_depth) {
    return velocity_from_strike(strike_time_ms, peak_depth);
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
    /* Seeds BOTH cascade stages (and magnitude's own single stage)
     * directly from this instant's raw reading -- starting a slow filter
     * at 0 would produce a fake ramp-up transient every single note-on;
     * starting it at the current real value means it only has to track
     * genuine CHANGE from here, not climb out of a false zero first. */
    s->pitch_bend_smoothed_x = x;
    s->pitch_bend_smoothed_x2 = x;
    s->pitch_bend_smoothed_y = y;
    s->pitch_bend_smoothed_y2 = y;
    s->pitch_bend_smoothed_magnitude = magnitude;
    s->pitch_bend_run_magnitude = magnitude;
    /* Seeded from a fresh read here, NOT s->smoothed_depth -- at this
     * exact point s->smoothed_depth still holds whatever this pad's
     * PREVIOUS note last left it at (it isn't reseeded to the real
     * current depth until a few lines after this call returns, see that
     * field's own seeding comment) -- reading it here would prime the
     * very first depth_activity computation with a fake, stale delta. */
    s->pitch_bend_prev_depth = (float)tiles_hall_get_depth(pad);
    /* Zeroed, not carried over -- a stale nonzero rate left over from
     * this pad's PREVIOUS note would otherwise bias depth_activity (and
     * so the adaptive recenter rate) for the first few ticks of a brand
     * new note, before real samples have a chance to overwrite it. */
    s->pitch_bend_smoothed_depth_rate = 0.0f;
    s->pitch_bend_smoothed_delta = 0.0f;
    s->pitch_bend_baseline_settled = false;
    s->pitch_bend_claim_ms = now_ms;
    s->pitch_bend_last_sent = PITCH_BEND_CENTER;
    s->pitch_bend_run_active = false;
    /* Zeroed, not carried over -- same reasoning as pitch_bend_smoothed_
     * depth_rate above: a stale energy value left over from this pad's
     * PREVIOUS note would otherwise bias the vibrato detector for the
     * first few ticks of a brand new note. */
    s->pitch_bend_wiggle_energy = 0.0f;
    s->pitch_bend_wiggle_active = false;
}

/* Scans every OTHER currently-held pad (excluding `exclude_pad`) and
 * returns whichever has the highest touch_claim_seq -- i.e. whichever
 * was struck most recently -- or 0 if none are held. Used only by
 * end_held_note()'s own non-MPE branch below, to hand the shared
 * channel's continuous-controller ownership back to an older still-
 * held pad rather than dropping it to "nobody" the instant the most
 * recent one releases. */
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
    tiles_cv_gate_note_off(s->active_note);
    tiles_haptics_stop(pad);
    tiles_midi_send_pitch_bend(s->midi_channel, PITCH_BEND_CENTER);
    s->pitch_bend_active = false;
    if (s->midi_channel == TILES_MIDI_MPE_MASTER_CHANNEL) {
        /* Real bug this addition itself would otherwise introduce: the
         * MPE-only cleanup below computes idx as midi_channel minus
         * TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL (1u) -- on the shared
         * master channel (0u) that underflows a uint8_t to 255, which
         * would then write s_mpe_channels[255], 240 bytes past the end
         * of a 15-entry array. Guarded here instead of changing that
         * subtraction, since master-channel notes have no per-channel
         * pool slot to release in the first place -- there's nothing
         * for that line to correctly do for them at all. Real feedback:
         * "most recently touched/bent pad wins" -- if this pad was the
         * one currently driving the shared channel's pitch bend/
         * pressure, hand that back to whichever OTHER held pad was
         * touched most recently, and immediately resend ITS actual
         * current bend so the channel reflects that pad's real tilt
         * instead of staying wherever this departing pad (or the
         * center send just above) left it. */
        if (s_non_mpe_owner_pad == pad) {
            uint8_t next_owner = find_most_recent_held_pad(pad);
            s_non_mpe_owner_pad = next_owner;
            if (next_owner != 0u) {
                pad_expr_t *next = &s_pads[next_owner - 1u];
                tiles_midi_send_pitch_bend(TILES_MIDI_MPE_MASTER_CHANNEL, next->pitch_bend_last_sent);
            }
        }
    } else {
        uint8_t idx = (uint8_t)(s->midi_channel - TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL);
        s_mpe_channels[idx].in_use = false;
    }
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
 * nibble). If every (non-reserved) Member Channel is already in use,
 * forcibly ends the oldest-claimed one's note (via end_held_note()
 * above) and hands that SAME channel straight to the new pad, rather
 * than freeing it and re-searching -- avoids a redundant second scan
 * and keeps the "steal" atomic from this function's own perspective.
 *
 * Real feedback: "is there anything needed to stop stuck niotes?" --
 * tiles_op_mode_sequencer_channel_is_reserved() (see that function's own
 * comment) is checked for every candidate in BOTH the free-slot search
 * and the steal-oldest fallback below, so a live touch can never claim
 * one of the sequencer's OP_SEQ_NUM_LANES own channels while it's
 * genuinely running -- the real, if narrow, collision that used to be
 * possible now that up to 4 lanes and live melodic touches can genuinely
 * run at the same time. Doesn't retroactively evict a live touch that
 * already sat on one of those channels BEFORE the sequencer started
 * wanting it -- reaching that needs several simultaneous fingers already
 * down (search fills the LOW channels first, lane channels are the top
 * few) AND the sequencer starting at that exact moment; narrower still
 * than the gap this fix closes, and not chased here. */
static uint8_t claim_mpe_channel(uint8_t pad) {
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        uint8_t channel = (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i);
        if (!s_mpe_channels[i].in_use && !tiles_op_mode_sequencer_channel_is_reserved(channel)
#if TILES_MELODIC_HARMONICS_ENABLED
            && !harmonic_channel_is_reserved(channel)
#endif
        ) {
            s_mpe_channels[i].in_use = true;
            s_mpe_channels[i].owner_pad = pad;
            s_mpe_channels[i].claim_seq = s_next_mpe_claim_seq++;
            return channel;
        }
    }

    uint8_t oldest_idx = TILES_MIDI_MPE_NUM_MEMBER_CHANNELS;
    for (uint8_t i = 0; i < TILES_MIDI_MPE_NUM_MEMBER_CHANNELS; i++) {
        if (tiles_op_mode_sequencer_channel_is_reserved((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i))) {
            continue;
        }
#if TILES_MELODIC_HARMONICS_ENABLED
        if (harmonic_channel_is_reserved((uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + i))) {
            continue;
        }
#endif
        if (oldest_idx == TILES_MIDI_MPE_NUM_MEMBER_CHANNELS || s_mpe_channels[i].claim_seq < s_mpe_channels[oldest_idx].claim_seq) {
            oldest_idx = i;
        }
    }
    if (oldest_idx == TILES_MIDI_MPE_NUM_MEMBER_CHANNELS) {
        /* Every single Member Channel is reserved -- impossible given
         * TILES_MIDI_MPE_NUM_MEMBER_CHANNELS (15) > OP_SEQ_NUM_LANES (4),
         * but a defined, harmless fallback (the first Member Channel)
         * rather than reading s_mpe_channels[15] out of bounds if either
         * constant ever changed. */
        return TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL;
    }
    uint8_t stolen_pad = s_mpe_channels[oldest_idx].owner_pad;
    printf("[expression] pad %u stealing pad %u's MPE channel %u (all %u member channels in use)\n", pad, stolen_pad,
           (unsigned)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + oldest_idx), (unsigned)TILES_MIDI_MPE_NUM_MEMBER_CHANNELS);
    pad_expr_t *stolen = &s_pads[stolen_pad - 1u];
    end_held_note(stolen, stolen_pad);
    stolen->state = PAD_STATE_IDLE;
    /* Real bug found reviewing this function, not from real feedback:
     * end_held_note() above sets in_use=false for the stolen channel
     * (freeing its OLD owner), and this steal path hands that same
     * channel straight to the NEW pad without ever setting in_use back
     * to true -- so the free-slot search at the top of this function
     * would see this exact index as available again on the very next
     * call, handing the identical channel to a THIRD pad while the
     * second one is still actively sounding on it. Both would then
     * share one MPE channel: pitch bend/pressure from either bends the
     * other's note, and a note-off from either can strand or kill the
     * other's. */
    s_mpe_channels[oldest_idx].in_use = true;
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
             * loop below), only a brand-new touch is suppressed here.
             *
             * tiles_op_mode_owns_pad(pad) (not the blanket _owns_pad_grid())
             * here specifically because chord mode only needs its own 8
             * chord-strip pads excluded -- its other 16 melody pads must
             * keep running this exact real strike pipeline unmodified, the
             * same "reuse expression.c, just remap notes" approach guitar
             * mode already established (see services/note_map.h's own
             * "Chord mode" section). Every other mode's answer is
             * identical to _owns_pad_grid()'s own, unchanged from before
             * this per-pad accessor existed. */
            if (touched && !tiles_expression_control_owns_pad_grid() && !tiles_octave_control_is_transpose_active() &&
                !tiles_op_mode_owns_pad(pad) && !tiles_game_mode_is_active()) {
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

            /* "ready": a real press has been measured AND
             * VELOCITY_FOLLOWTHROUGH_MS has elapsed since crossing while
             * still touched -- see that constant's own comment for why
             * this short wait exists (measuring depth over a real window
             * instead of one instantaneous sample is what makes a fast-
             * but-light tap actually read as soft). "commit_on_release":
             * touch ended before that window finished -- commit
             * immediately with whatever peak_depth was reached by then,
             * rather than discarding a genuine hit (or pointlessly
             * delaying a note that's already known to be over) just
             * because contact happened to end first. */
            uint32_t crossed_at_ms = s->touch_start_sample_ms + s->strike_time_ms;
            bool followthrough_elapsed = pressed && (now_ms - crossed_at_ms) >= VELOCITY_FOLLOWTHROUGH_MS;
            bool ready = touched && followthrough_elapsed;
            bool commit_on_release = !touched && pressed;

            if (ready || commit_on_release) {
                s->active_note = tiles_note_map_get_note(pad);
                uint8_t velocity = velocity_from_strike(s->strike_time_ms, s->peak_depth);
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
                /* Real feedback: "make sure the pitch bend works with
                 * non mpe layouts meaning pitch bend wheel." Bypasses
                 * claim_mpe_channel()'s whole pool entirely while
                 * !s_mpe_enabled -- every note just goes out on the one
                 * shared master channel, the same channel a plain non-
                 * MPE synth already listens to by default, rather than
                 * spending a Member Channel (and its steal-eviction
                 * machinery) on a note that was never going to use it
                 * as an independent channel anyway. */
                s->midi_channel = s_mpe_enabled ? claim_mpe_channel(pad) : TILES_MIDI_MPE_MASTER_CHANNEL;
                init_pitch_bend_for_pad(s, pad, now_ms);
                s->touch_claim_seq = s_next_mpe_claim_seq++;
                if (!s_mpe_enabled) {
                    /* Real feedback: "most recently touched/bent pad
                     * wins" -- the newly-struck pad always becomes the
                     * new owner (it's unconditionally the most recent
                     * touch by construction). Force-sends center rather
                     * than relying on this pad's own next computed bend
                     * to naturally differ from its last-sent value --
                     * init_pitch_bend_for_pad() above already reset
                     * THIS pad's own bookkeeping to center, but the
                     * actual synth channel could still be sitting
                     * wherever the PREVIOUS owner (a different pad)
                     * left it. */
                    s_non_mpe_owner_pad = pad;
                    tiles_midi_send_pitch_bend(TILES_MIDI_MPE_MASTER_CHANNEL, PITCH_BEND_CENTER);
                }
                tiles_midi_note_on(s->midi_channel, s->active_note, velocity);
                tiles_cv_gate_note_on(s->active_note, velocity);
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
                 * sent. */
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
             * flag already makes it a no-op (see tiles_haptics_set_muted).
             * Also gated on non-MPE ownership (s_non_mpe_owner_pad) --
             * while !s_mpe_enabled, every held pad shares this exact
             * channel, so without this check whichever pad's channel
             * pressure happened to change most recently on any given
             * scan would silently steal the shared channel's pressure
             * value out from under the others, flickering between
             * pads with no real ownership rule at all. Deliberately no
             * forced resync on ownership change here the way pitch
             * bend gets (see the note-on/end_held_note() sites) --
             * stale channel pressure for a scan or two is a much
             * smaller, more cosmetic problem than a note landing at
             * the wrong PITCH, which is the specific failure this
             * feature exists to prevent. */
            if (!s_expression_muted && (s_mpe_enabled || s_non_mpe_owner_pad == pad)) {
                tiles_midi_send_channel_pressure(s->midi_channel, at);
            }
            tiles_cv_gate_channel_pressure(s->active_note, at);
            tiles_haptics_set_sustain_level(pad, at);
        }

        /* Pitch bend: genuinely per-note under MPE -- every pad with
         * pitch_bend_active runs this completely independently on its
         * own MPE channel, no shared "owner" arbitration needed there
         * (see this file's "Pitch bend from sideways motion" section).
         * While !s_mpe_enabled, every pad computes its own bend exactly
         * the same way (unchanged below -- deliberately not gating the
         * COMPUTATION itself, only whether it actually reaches the
         * wire, to avoid disturbing this cascade's own carefully-tuned
         * timing for whichever pad happens to become owner later), but
         * only the current s_non_mpe_owner_pad's value actually gets
         * sent -- see the send site below and its own comment. */
        if (s->pitch_bend_active) {
            tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
            if (hs.valid) {
                float x, y, magnitude;
                hall_xy_and_magnitude(hs.x, hs.y, hs.z, &x, &y, &magnitude);

                /* Two-stage cascaded EMA of raw X/Y (see PITCH_BEND_
                 * SMOOTHING_ALPHA's own comment for the cascaded-filter
                 * math), single-stage for magnitude -- see
                 * pitch_bend_smoothed_x2/y2's own struct-field comment:
                 * BOTH the live delta computation below and the baseline
                 * recenter target read the fully-cascaded x2/y2, not the
                 * lighter first stage. Runs every tick unconditionally
                 * (both before AND after baseline settles), not just
                 * during the initial settle window. */
                s->pitch_bend_smoothed_x += PITCH_BEND_SMOOTHING_ALPHA * (x - s->pitch_bend_smoothed_x);
                s->pitch_bend_smoothed_x2 += PITCH_BEND_SMOOTHING_ALPHA * (s->pitch_bend_smoothed_x - s->pitch_bend_smoothed_x2);
                s->pitch_bend_smoothed_y += PITCH_BEND_SMOOTHING_ALPHA * (y - s->pitch_bend_smoothed_y);
                s->pitch_bend_smoothed_y2 += PITCH_BEND_SMOOTHING_ALPHA * (s->pitch_bend_smoothed_y - s->pitch_bend_smoothed_y2);
                s->pitch_bend_smoothed_magnitude += PITCH_BEND_SMOOTHING_ALPHA * (magnitude - s->pitch_bend_smoothed_magnitude);

                /* Independent wiggle-vibrato energy -- see
                 * pitch_bend_apply_vibrato()'s own VIBRATO_ENERGY_NOISE_
                 * FLOOR comment for the full reasoning and the real
                 * capture behind this. Reads the gap between the two
                 * cascade stages just updated above but writes neither
                 * of them -- runs unconditionally, same as the cascade
                 * itself, so it's already settled by the time a run
                 * might need it, not starting cold mid-gesture. */
                {
                    float wiggle_residual_x = s->pitch_bend_smoothed_x - s->pitch_bend_smoothed_x2;
                    float wiggle_residual_y = s->pitch_bend_smoothed_y - s->pitch_bend_smoothed_y2;
                    float wiggle_residual_magnitude =
                        sqrtf(wiggle_residual_x * wiggle_residual_x + wiggle_residual_y * wiggle_residual_y);
                    s->pitch_bend_wiggle_energy +=
                        PITCH_BEND_SMOOTHING_ALPHA * (wiggle_residual_magnitude - s->pitch_bend_wiggle_energy);
                }

                /* Signed tick-to-tick depth delta, smoothed -- see
                 * pitch_bend_smoothed_depth_rate's own struct comment for
                 * why SIGNED-then-smoothed (sign-consistent real presses
                 * survive, sign-flipping wobble during a tilt cancels
                 * out), not smoothed-then-absolute. Reuses PITCH_BEND_
                 * SMOOTHING_ALPHA rather than inventing a third tuning
                 * constant for what's the same underlying tremor-
                 * rejection problem this file already solved for X/Y. */
                s->pitch_bend_smoothed_depth_rate +=
                    PITCH_BEND_SMOOTHING_ALPHA * ((s->smoothed_depth - s->pitch_bend_prev_depth) -
                                                   s->pitch_bend_smoothed_depth_rate);

                /* 0 (depth steady) .. 1 (depth changing fast), clamped --
                 * see PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE's own comment
                 * for where this threshold came from, and PITCH_BEND_
                 * BASELINE_RECENTER_ALPHA_SLOW's for the full reasoning
                 * on why this drives the baseline recenter rate below. */
                float depth_activity = fabsf(s->pitch_bend_smoothed_depth_rate) / PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE;
                if (depth_activity > 1.0f) {
                    depth_activity = 1.0f;
                }

                s->pitch_bend_prev_depth = s->smoothed_depth;

                if (!s->pitch_bend_baseline_settled) {
                    /* See PITCH_BEND_SETTLE_MS's own comment -- stays
                     * centered (never even reaches the send-if-changed
                     * check below) until the cascade above has had a few
                     * ticks to settle, then captures baseline X/Y from
                     * that settled value rather than one raw
                     * instantaneous sample. */
                    if ((now_ms - s->pitch_bend_claim_ms) >= PITCH_BEND_SETTLE_MS) {
                        s->pitch_bend_baseline_x = s->pitch_bend_smoothed_x2;
                        s->pitch_bend_baseline_y = s->pitch_bend_smoothed_y2;
                        s->pitch_bend_baseline_settled = true;
                        s->pitch_bend_smoothed_delta = 0.0f;
                    }
                } else {
                    /* See PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW's own
                     * comment -- chases the smoothed (tremor-filtered)
                     * X/Y above, NOT raw X/Y directly -- chasing
                     * raw samples was itself the bug a real deliberate
                     * held lean exposed (see that constant's own
                     * comment). Only while no real bend run is confirmed
                     * (gated on the PREVIOUS tick's classification, since
                     * THIS tick's run state isn't known yet -- it depends
                     * on delta_x, computed below, which depends on
                     * baseline_x; the alternative would be a circular
                     * dependency within the same tick) -- a confirmed
                     * run always wins regardless of depth_activity, so
                     * pressing harder WHILE holding a deliberate bend
                     * can't erase it. The RATE itself blends between the
                     * SLOW and FAST constants by depth_activity: near-
                     * instant while depth is actively changing (nothing
                     * lost, since a run is never confirmed during a
                     * press ramp anyway -- this is what stops pressure
                     * from reading as fake tilt, without needing to know
                     * in advance what the real X/Y-vs-depth relationship
                     * looks like), slow once depth holds steady (so a
                     * genuine held tilt doesn't fade). */
                    if (!s->pitch_bend_run_active) {
                        /* CUBED, not blended linearly against depth_
                         * activity directly -- real feedback: "still no
                         * pitch bend on tilt but preassure works," traced
                         * with a real capture to FAST (0.25) being ~125x
                         * SLOW (0.002): a plain linear blend means even
                         * modest activity (0.1-0.3, which ordinary sensor
                         * noise produces almost continuously, real tilt
                         * gesture or not) already pulls the effective
                         * rate to 10-30% of that huge range -- 10-40x
                         * bigger than pure SLOW -- defeating the whole
                         * point of a "slow, protective" regime for
                         * anything short of activity being almost exactly
                         * 1.0. Cubing keeps LOW-to-moderate activity's
                         * contribution proportionally much smaller
                         * (0.3 -> 0.027, 0.1 -> 0.001) while leaving
                         * activity=1.0 (a genuinely fast, unambiguous
                         * press) still mapping to the full FAST rate --
                         * shifts the whole curve toward "stay slow unless
                         * activity is clearly, unambiguously high"
                         * without changing either endpoint. */
                        float activity_shaped = depth_activity * depth_activity * depth_activity;
                        float recenter_alpha = PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW +
                                                (PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST -
                                                 PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW) *
                                                    activity_shaped;
                        s->pitch_bend_baseline_x += recenter_alpha * (s->pitch_bend_smoothed_x2 - s->pitch_bend_baseline_x);
                        s->pitch_bend_baseline_y += recenter_alpha * (s->pitch_bend_smoothed_y2 - s->pitch_bend_baseline_y);
                        /* Chased in lockstep with baseline_x/y above, same
                         * gate, same one-tick-of-lag reasoning -- see this
                         * section's own comment right below (at
                         * predicted_baseline_cosine_x) for WHY magnitude
                         * itself now needs to freeze during a confirmed
                         * run too, not just baseline_x/y. */
                        s->pitch_bend_run_magnitude = s->pitch_bend_smoothed_magnitude;
                    }

                    float baseline_x_at_depth = s->pitch_bend_baseline_x;
                    float baseline_y_at_depth = s->pitch_bend_baseline_y;

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
                     * Both terms below use THIS SAME tick's
                     * `pitch_bend_smoothed_magnitude` -- critically, the
                     * SAME one, not two independently-lagging values. A
                     * first version of this fix smoothed the "current"
                     * cosine (an EMA, lagging by construction) but
                     * compared it against an unsmoothed "predicted
                     * baseline" -- during any depth change (essentially
                     * the entire strike-to-hold ramp on every note), that
                     * lag MISMATCH alone produces a nonzero, depth-
                     * correlated delta even for zero real tilt,
                     * reintroducing exactly the bias this compensation
                     * exists to remove. Real feedback after that version:
                     * "pitch bend is so extreme... biased towards down it
                     * never goes up" -- consistent with a lag-driven
                     * artifact, since a struck-then-held note's depth is
                     * deepening (not easing) for most of its hold. Fixed,
                     * at the time, by computing the raw delta from
                     * unsmoothed X and unsmoothed per-tick magnitude
                     * together (so a pure depth change cancelled to 0
                     * before any smoothing happened at all) and smoothing
                     * THAT result instead. X/Y and magnitude are now
                     * BOTH cascaded (see PITCH_BEND_SMOOTHING_ALPHA's own
                     * comment) rather than raw, but the invariant that
                     * actually mattered is preserved: both terms still
                     * divide by the exact same magnitude value at this
                     * exact tick, so the mismatch this history warns
                     * about can't recur -- it was never really about
                     * "raw vs. smoothed," it was about the two sides
                     * disagreeing on WHICH magnitude to use.
                     *
                     * Sign flipped here (predicted - current, not
                     * current - predicted) -- real feedback: "bend is
                     * flipped its bending in the opposite way than we
                     * need." pitch_bend_14bit_from_cosine_delta() itself
                     * is otherwise direction-agnostic.
                     *
                     * Two axes combined, not X alone -- real feedback:
                     * "incorporate the 2 axis tilt onto the pitch bend to
                     * provide a more strong reading of tilt... more sable
                     * reeds... make vibratos." Y gets the exact same
                     * same-magnitude compensation treatment as X above
                     * (delta_y cancels to 0 for a pure depth change, for
                     * the identical reason delta_x does). MAGNITUDE
                     * combines both axes (sqrt(dx^2 + dy^2)) -- strictly
                     * >= either axis alone, so a real tilt or wiggle that
                     * happens to land partly on Y adds to the reading
                     * instead of being lost. SIGN stays anchored to
                     * delta_x alone, deliberately not a true 2D bend
                     * direction -- preserves the already-tuned left/right
                     * bend feel the deadzone/sensitivity constants below
                     * were calibrated against.
                     *
                     * This file went through a whole dedicated round of
                     * debugging X ALONE first -- real feedback: "lets
                     * debug side tilt and ignore other tilt for now" --
                     * to isolate and fix two real bugs that the combined
                     * signal had been partially masking: baseline drift
                     * (PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW/_FAST,
                     * needed because a pad's true rest position under sustained pressure
                     * measurably differs from its position in the first
                     * PITCH_BEND_SETTLE_MS of contact) and a too-short
                     * noise-transient window (PITCH_BEND_ARM_MS, raised
                     * 15 -> 120). Real feedback after confirming X alone
                     * still couldn't cleanly separate genuine held tilt
                     * from ordinary hand tremor without adding real
                     * latency (a from-scratch estimate: doubling
                     * PITCH_BEND_SMOOTHING_ALPHA's smoothing would cost
                     * roughly 300-400ms of onset latency, enough to kill
                     * genuine fast vibrato outright): "the old thing was
                     * not working we need to compensate for preassure
                     * depth and drift... yes go ahead with X+Y." Both
                     * bugfixes carry over unchanged -- they were never
                     * X-specific, and Y gets the identical treatment
                     * below.
                     *
                     * CURRENT cosine reads the fully-cascaded SECOND
                     * stage (pitch_bend_smoothed_x2/y2), the SAME stage
                     * baseline_x/y_at_depth is recentered from -- this
                     * went through two failed attempts before landing
                     * here, both worth keeping on record since the
                     * failure mode is subtle and could easily be re-tried
                     * by a future round chasing the same fast-wiggle ask.
                     *
                     * Attempt 1: read the LIGHTER first stage
                     * (pitch_bend_smoothed_x/y) here instead, keeping the
                     * baseline on the heavier second stage -- real
                     * feedback wanted this: "i just need fast wiggles of
                     * the keys to activate bend as well," and real
                     * musical vibrato genuinely does overlap hand
                     * tremor's frequency band (this file's own research
                     * summary), so a filter steep enough to reject one
                     * necessarily damps the other; splitting the cascade
                     * this way was meant to preserve a fast wiggle's
                     * amplitude in the live signal while keeping the
                     * baseline maximally stable. Broke almost
                     * immediately -- real feedback on the very next boot:
                     * "the last boot re introduced the pitch bend issues
                     * with preassure." Splitting the two terms across two
                     * DIFFERENTLY-lagged stages reintroduced exactly the
                     * "mismatched lag between the two terms" failure this
                     * file has hit more than once before (see this
                     * section's own "first version of this fix" comment
                     * above): the faster-reacting live signal raced ahead
                     * of the slower-reacting baseline during any real
                     * depth change, and that transient gap read as fake
                     * tilt again.
                     *
                     * Attempt 2: collapse BOTH terms onto the LIGHTER
                     * first stage instead (satisfies "both terms must
                     * read the same stage" -- true, but the wrong half of
                     * the mismatch to keep). Also broke, worse than
                     * either previous state -- real feedback: "you've
                     * ruined the stable version and bend is still extreme
                     * and unpredictable... like it's just reading the
                     * unfiltered numbers." PITCH_BEND_DEADZONE_COSINE_
                     * DELTA (0.025) and PITCH_BEND_ARM_MS (30) were both
                     * tuned down to their current, more sensitive values
                     * against a live signal that was ALWAYS the fully-
                     * cascaded stage up to that point -- swapping the
                     * live signal for the noisier single-stage one while
                     * leaving those thresholds at their tightest-ever
                     * settings meant meaningfully more raw tremor now had
                     * to be rejected by a deadzone/confirmation window
                     * that was never validated against that much noise.
                     * Worse still, the baseline recenter chases
                     * WHICHEVER stage current_cosine now reads at up to
                     * PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST (0.25, near-
                     * instant) during any depth change -- chasing the
                     * lighter, noisier stage at that rate lets baseline_x/
                     * y itself pick up much more raw tremor specifically
                     * during every press/release ramp, exactly the
                     * regime this whole mechanism exists to keep clean.
                     *
                     * Fix: both terms read x2/y2 -- literally the same
                     * arrangement that was already in place, continuously,
                     * from the original "too jittery, inconsistent"
                     * two-stage cascade all the way through the depth-
                     * rate-gating fix and the linear-to-cubic recenter
                     * fix that earned "wow it feels good" in the first
                     * place. The fast-wiggle ask is a real, still-open
                     * request, but this file's own tremor-vs-vibrato
                     * research already says why it can't be solved by
                     * lightening the ONE shared live signal both jobs
                     * depend on: any future attempt needs an
                     * INDEPENDENT wiggle detector layered on top of a
                     * stable bend, not a lighter filter substituted into
                     * this path. Magnitude stays single-stage -- see
                     * pitch_bend_smoothed_magnitude's own struct comment
                     * for why it never needed a second cascade stage --
                     * but as of the real capture behind PITCH_BEND_
                     * WIRE_RANGE_COMPENSATION's own history, it DOES need
                     * to freeze during a confirmed run, same as baseline_
                     * x/y just above (pitch_bend_run_magnitude, chased in
                     * lockstep with baseline_x/y, same gate).
                     *
                     * Why: the "pure depth change cancels to 0" proof a
                     * few paragraphs up only holds when x2 == baseline_x
                     * (no real tilt yet) -- for an ACTIVE, non-zero tilt
                     * (x2 != baseline_x, the whole point of a confirmed
                     * run), delta = (baseline_x - x2) / magnitude is
                     * still inversely proportional to whatever magnitude
                     * does. A real capture of a held ~5.5s deliberate
                     * tilt (see this file's own wiggle-vs-tilt capture,
                     * "[wiggle-cap]") found depth swinging from 527 to
                     * 1059 DURING that single "steady" hold -- a real
                     * hand isn't perfectly steady in press force while
                     * also concentrating on holding an angle -- and the
                     * sent bend value wobbling by as much as ~600 (out of
                     * a ~2047 max) over a few hundred ms in direct
                     * lockstep with those depth wobbles, even though
                     * baseline_x/y were already frozen (run_active was
                     * true throughout). That's this same division-by-
                     * live-magnitude effect, not a new bug: real feedback
                     * "not quite fully stable but mostly... when bent it
                     * wobbles" is that residual. Freezing magnitude too,
                     * the instant a run is confirmed, removes this
                     * specific coupling for the rest of that run, exactly
                     * how baseline_x/y already protect the ANGLE from
                     * further pressure changes once a run is confirmed --
                     * this extends the same protection to how that angle
                     * gets SCALED into a cosine.
                     * Real, accepted tradeoff, not fully solved: a
                     * deliberate LARGE press change mid-run (not just
                     * natural hold jitter) will now read against a stale
                     * magnitude for the rest of that run, which could
                     * under- or over-state the angle depending on which
                     * way depth moved -- unmeasured how often that
                     * matters in practice versus the jitter this fixes,
                     * worth a fresh capture if a future round finds bend
                     * feels wrong specifically after a big mid-hold press
                     * change. */
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

                    /* No further smoothing here -- X/Y/magnitude are
                     * already TWO-STAGE cascaded above before this point
                     * (see PITCH_BEND_SMOOTHING_ALPHA's own comment), so
                     * this used to be a THIRD EMA stage stacked on top of
                     * that, uncounted in the "cascading costs ~40% more
                     * latency, not 100%" estimate that motivated the
                     * cascade in the first place -- real feedback once
                     * played: "time before reacting is too long." A
                     * pad's own smoothed_delta field is kept (not removed
                     * from the struct) only because pitch_bend_14bit_
                     * from_cosine_delta()'s signature still takes a
                     * `delta` parameter; it's set directly here rather
                     * than filtered into over time. */
                    s->pitch_bend_smoothed_delta = raw_delta_this_tick;
                    float delta = s->pitch_bend_smoothed_delta;
                    uint16_t bend = pitch_bend_14bit_from_cosine_delta(s, delta, now_ms);
                    bend = pitch_bend_apply_vibrato(s, bend, now_ms);
                    if (bend != s->pitch_bend_last_sent) {
                        /* Updated regardless of ownership below -- this
                         * is deliberately this pad's own always-fresh
                         * "true current bend," not just "what was last
                         * actually transmitted." find_most_recent_
                         * held_pad()'s caller (end_held_note()'s own
                         * non-MPE branch) reads exactly this field to
                         * resync the channel the instant ownership
                         * hands back to this pad. */
                        s->pitch_bend_last_sent = bend;
                        if (s_mpe_enabled || s_non_mpe_owner_pad == pad) {
                            tiles_midi_send_pitch_bend(s->midi_channel, bend);
                        }
                    }
                }
            }
        }
    }
#if TILES_MELODIC_HARMONICS_ENABLED
    scan_melodic_harmonics(now_ms);
#endif
}
