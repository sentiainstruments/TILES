#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "midi_out.h"
#include "haptics.h"

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
 * pad has to be broken for retrigger, that's bad." Depth (relative to
 * the ORIGINAL touch_start_depth, the same reference the strike itself
 * was measured against) has to ease back down to within this of that
 * original light-touch reading -- not just down from this note's own
 * peak -- before a renewed press is treated as a brand-new strike. This
 * is deliberately conservative and close to "as light as the initial
 * touch again": a real held note's pressure is expected to fluctuate
 * somewhat for aftertouch's own sake, and a threshold any looser risks
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
 * position signal. Concretely: touch_start_sample_ms marks the first
 * fresh Hall sample after touch begins (the same reference
 * touch_start_depth already used), and strike_time_ms (see pad_expr_t)
 * is set exactly once, the moment peak_depth_delta first crosses
 * MIN_STRIKE_DEPTH_DELTA, as the gap between those two sample
 * timestamps. This needs only two timestamps, not a differentiated
 * series -- immune to the per-sample noise/quantization that broke the
 * accel approach, and well-defined even when only one or two samples
 * arrive before the threshold is crossed (a fast, hard strike no longer
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
 * hall.h. */
#define DEPTH_TO_AFTERTOUCH_FULL_SCALE 900u

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

typedef enum {
    PAD_STATE_IDLE = 0,
    PAD_STATE_AWAITING_STRIKE,
    PAD_STATE_NOTE_ON,
} pad_expr_state_t;

typedef struct {
    pad_expr_state_t state;
    uint32_t touch_start_ms;

    /* Reference depth MIN_STRIKE_DEPTH_DELTA below measures real travel
     * against -- captured immediately in begin_awaiting_strike(), at the
     * exact scan tick touch is first detected, using whatever depth
     * hall.c already has cached for this pad. NOT "wait for the first
     * fresh Hall sample after touch begins" (an earlier version of this
     * field) -- that seemed safer against hall.c's background-round-
     * robin staleness, but broke fast strikes outright: a hard, fast
     * press can already be well past MIN_STRIKE_DEPTH_DELTA by the time
     * the *first* Hall sample after touch begins actually arrives, so
     * using that sample as the zero reference made peak_depth_delta
     * start near 0 and unable to ever reach threshold again on the way
     * back down -- "if I press really fast and hard nothing happens."
     * hall.c's depth is already baseline-relative (drift-compensated for
     * untouched pads via its own background tracker), so a cached
     * pre-touch reading is a perfectly valid zero point -- the earlier
     * "staleness" concern was solving a problem that didn't actually
     * exist, at the cost of one that very much did. Always valid the
     * instant AWAITING_STRIKE begins, hence no longer a bool-guarded
     * "first sample" flag. */
    float touch_start_depth;

    /* Paired with touch_start_depth -- the reference strike_time_ms
     * below measures elapsed time from. Set at the same moment, from
     * the scan tick's own clock (the same to_ms_since_boot() clock Hall
     * sample timestamps use, so directly comparable to them later). */
    uint32_t touch_start_sample_ms;

    /* Highest depth_delta (relative to touch_start_depth) seen at any
     * point since touch began, not just the current instant. Real
     * feedback: "strong hard presses don't trigger anything" -- a fast,
     * percussive strike can bounce back down (Hall depth springing back,
     * or touch itself ending) before the *current* reading still shows
     * it past MIN_STRIKE_DEPTH_DELTA, silently losing a real hit that
     * clearly happened. Gating on the peak ever reached, instead of
     * whatever the reading happens to be at the exact instant this scan
     * checks it, means a hit that already cleared the threshold stays
     * "pressed" even after it springs back. */
    float peak_depth_delta;

    /* Set exactly once, the moment peak_depth_delta first crosses
     * MIN_STRIKE_DEPTH_DELTA -- the elapsed time (Hall sample clock)
     * between touch_start_sample_ms and that crossing, which
     * velocity_from_strike_time() maps to a MIDI velocity. See this
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

void tiles_expression_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pads[i] = (pad_expr_t){0};
        s_pads[i].state = PAD_STATE_IDLE;
    }
}

static void begin_awaiting_strike(pad_expr_t *s, uint8_t pad, uint32_t now_ms) {
    s->state = PAD_STATE_AWAITING_STRIKE;
    s->touch_start_ms = now_ms;
    /* Captured immediately, not on a later "first fresh sample" -- see
     * touch_start_depth's own comment for why. */
    s->touch_start_depth = (float)tiles_hall_get_depth(pad);
    s->touch_start_sample_ms = now_ms;
    s->peak_depth_delta = 0.0f;
    s->threshold_crossed = false;
    s->strike_time_ms = 0;
    s->last_seen_sample_time_ms = 0;
}

/* Maps elapsed strike time (ms, touch_start_sample_ms to the moment
 * peak_depth_delta crossed MIN_STRIKE_DEPTH_DELTA) to a MIDI velocity --
 * see this file's "Velocity: elapsed-time-to-actuation" section for the
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
    uint32_t scaled = ((uint32_t)depth * 127u) / DEPTH_TO_AFTERTOUCH_FULL_SCALE;
    return (uint8_t)(scaled > 127u ? 127u : scaled);
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
            if (touched) {
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
                    float delta = depth - s->touch_start_depth;
                    if (delta > s->peak_depth_delta) {
                        s->peak_depth_delta = delta;
                    }
                    if (!s->threshold_crossed && s->peak_depth_delta >= MIN_STRIKE_DEPTH_DELTA) {
                        /* First sample to cross the actuation threshold --
                         * see strike_time_ms's own comment. */
                        s->threshold_crossed = true;
                        s->strike_time_ms = hs.sample_time_ms - s->touch_start_sample_ms;
                    }
                }
            }

            /* Gated on the PEAK depth reached, not the current instant --
             * see peak_depth_delta's own comment for why: a fast,
             * percussive strike can spring back (or end touch) before a
             * reading taken *right now* would still show it past
             * threshold, which silently lost real hard strikes before
             * this fix ("strong hard presses don't trigger anything"). */
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
                printf("[expression] pad %u note-on: %s, peak_depth_delta=%d strike_time_ms=%u velocity=%u\n", pad,
                       commit_on_release ? "commit_on_release" : "ready", (int)s->peak_depth_delta,
                       s->strike_time_ms, velocity);
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
            tiles_midi_note_off(s->active_note);
            tiles_haptics_stop(pad);
            s->state = PAD_STATE_IDLE;
            continue;
        }

        float raw_depth = (float)tiles_hall_get_depth(pad);

        /* Retrigger without a full release -- real feedback: "contact
         * with pad has to be broken for retrigger, that's bad." Once
         * depth has eased back down close to the pad's original
         * touch-down reading (not just down from this note's peak --
         * see RETRIGGER_ARM_DEPTH_DELTA), treat it exactly like touch
         * had been released and retouched: send note-off for the held
         * note and drop back into strike-detection using the current
         * depth as a fresh reference, all without touch itself ever
         * going false. A subsequent real press is then measured and
         * fires a brand-new note-on with its own freshly computed
         * velocity through the exact same path as any other strike. */
        if ((now_ms - s->note_on_ms) >= RETRIGGER_GRACE_MS &&
            (raw_depth - s->touch_start_depth) <= RETRIGGER_ARM_DEPTH_DELTA) {
            tiles_midi_note_off(s->active_note);
            tiles_haptics_stop(pad);
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
            tiles_midi_send_poly_aftertouch(s->active_note, at);
            tiles_haptics_set_sustain_level(pad, at);
        }
    }
}
