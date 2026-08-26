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
 * Strike detection -- rebuilt from a real captured session (see
 * docs/architecture/defaults-and-safeguards.md-style reasoning below),
 * not another guess. Two rounds of guessed constants (15, then 30 for
 * MIN_STRIKE_DEPTH_DELTA; 0.05 for ACCEL_TO_VELOCITY_SCALE) both failed
 * on real hardware: "any touch still triggers midi... different
 * velocity doesn't trigger anything either, it's all the same
 * velocity." A debug-console capture of the `[expression]` print below
 * across ~140 real touches gave the actual numbers this section is now
 * built from:
 *   - Hall depth reads in steps of 16 raw counts (sensor/driver
 *     quantization) -- 32 (2 steps) was by far the single most common
 *     depth_delta observed (66 of 115 fired notes), with a long tail up
 *     to 96 -- this is bare capacitive contact / incidental mechanical
 *     settling, not an intentional press. Genuine deliberate presses in
 *     the same capture reached 192-736 (out of the ~900-unit full-press
 *     range from the earlier Hall calibration session) -- a clear gap
 *     between "just touched it" and "actually pressed it."
 *   - peak_accel across the same capture ranged 0-23, with the vast
 *     majority under 18. ACCEL_TO_VELOCITY_SCALE (0.05) mapped every one
 *     of those to a floored 0, so velocity was 115-for-115 stuck at
 *     MIN_VELOCITY (8) regardless of how hard anything was struck --
 *     confirming "it's all the same velocity" outright, not a subtle
 *     miscalibration.
 * Fixes below: MIN_STRIKE_DEPTH_DELTA raised well past the observed
 * touch-only ceiling; ACCEL_TO_VELOCITY_SCALE raised to actually spread
 * the observed 0-23 accel range across a meaningful chunk of MIDI 0-127;
 * the fixed MIN_STRIKE_WINDOW_MS time floor is gone (see below) since it
 * no longer does any useful work once depth itself gates the commit;
 * MAX_STRIKE_WINDOW_MS extended since requiring more real travel before
 * firing means genuine (if less explosive) presses need more time to
 * reach it than the old 60ms window meant to catch touch-only taps ever
 * needed.
 * ==========================================================================*/

/* Need at least this many fresh Hall samples before trusting an
 * acceleration estimate (2 velocity values need 3 depth samples). No
 * longer paired with a fixed minimum elapsed-ms floor -- MIN_STRIKE_
 * DEPTH_DELTA below is what actually gates whether a touch counts as a
 * real press; requiring extra elapsed time on top of that no longer
 * screens out anything real depth-gating doesn't already catch, and
 * would only slow down a fast, hard strike's response for no benefit. */
#define MIN_STRIKE_SAMPLES 3u

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

/* Safety timeout: once a real press has actually been detected (see
 * MIN_STRIKE_DEPTH_DELTA below), commit with whatever accel data exists
 * once this much time has passed since touch-down, even without 3 clean
 * samples yet -- e.g. if the background/priority scan couldn't keep up.
 * Does NOT by itself make a bare touch fire a note -- see below.
 * Extended from 60ms: that value was picked when *any* touch lasting
 * that long would fire (the original bug); now that real travel past
 * MIN_STRIKE_DEPTH_DELTA is required first, a deliberate-but-unhurried
 * press can reasonably take longer than 60ms to reach it, and shouldn't
 * get force-committed on a stale accel estimate before it even arrives. */
#define MAX_STRIKE_WINDOW_MS 250u

/* Minimum real depth travel (Hall units) since touch-down before a
 * touch counts as an actual press worth firing a note for, rather than
 * a light touch/rest with no real key motion -- see this section's own
 * header comment above for the real captured data this is picked from.
 * 150 sits comfortably above the observed touch-only ceiling (~96, the
 * vast majority clustering at exactly 32) and comfortably below the
 * smallest clearly-deliberate press observed (192) -- real margin on
 * both sides of an actual measured gap, not a number picked from
 * nowhere. A touch that never crosses this just sits in
 * PAD_STATE_AWAITING_STRIKE until release cancels it with no note ever
 * sent, matching how a real key requires an actual press, not just
 * contact. Leaves most of the ~900-unit full-press range as aftertouch
 * travel after the note fires, same as a synth-action keybed's
 * actuation point sitting well before its mechanical bottom. Still not
 * hardware-verified with this exact value -- the capture that produced
 * the numbers above mixed light touches and real presses in one
 * session without labeling which was which as they happened; revisit
 * with a labeled capture (explicit "light touch" vs "press" trials) if
 * light touches still get through or deliberate soft presses stop
 * registering. */
#define MIN_STRIKE_DEPTH_DELTA 150.0f

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

/* ---- Velocity curve --------------------------------------------------
 * Real feedback on the first (linear) accel->velocity mapping: "velocity
 * curve is bad, very light press is not giving low velocity enough. We
 * need a better velocity curve that's closer to a piano or a synth
 * keybed. Also give some flat full velocity at the max velocity, meaning
 * there's some space that we count as full velocity before the highest
 * full reading, to aid aftertouch."
 *
 * A straight linear scale (accel * constant) can't satisfy both "very
 * light stays genuinely quiet" and "a confidently hard hit reliably
 * maxes out" at once -- pushing the low end down by raising the scale
 * just pushes the whole curve down with it, and vice versa. A real
 * piano/synth action instead uses a *curved* response: a power curve
 * with VELOCITY_CURVE_EXPONENT > 1 suppresses the low end relative to
 * linear (a light touch reads noticeably quieter than a linear mapping
 * would give it -- real energy is required to sound loud, matching how
 * an acoustic action feels), then ramps up more steeply as accel
 * approaches ACCEL_FULL_VELOCITY. Past that point velocity is pinned at
 * 127 -- a deliberate flat plateau *below* the mechanically hardest hit
 * this hardware could ever register, per the explicit ask: a confident
 * hit should reliably read as full velocity without needing to find the
 * single hardest possible strike, and everything past that plateau
 * feeds aftertouch instead (services/expression.c's aftertouch is
 * already a wholly separate signal off raw depth, not velocity, so this
 * plateau doesn't cost aftertouch anything -- it just stops velocity
 * from trying to also account for that same extra travel).
 *
 * ACCEL_FULL_VELOCITY (20) sits just below the real captured peak_accel
 * range's high end (0-23, per the strike-detection rebuild above) --
 * genuinely hard strikes in that capture already reliably crossed
 * "most of the way there," so 20 should read as "hit it with real
 * intent," not "the single hardest hit ever recorded." Both this and
 * VELOCITY_CURVE_EXPONENT (1.8, chosen to noticeably suppress accel
 * readings under ~8-10 the way the captured light-touch-adjacent hits
 * did) are first attempts at a *feel*, not derived from a curve
 * measured on real hardware -- there's no substitute for playing it. */
#define ACCEL_FULL_VELOCITY 20.0f
#define VELOCITY_CURVE_EXPONENT 1.8f

/* Even a strike weak enough to barely clear MIN_STRIKE_DEPTH_DELTA
 * should produce an audible note, not near-silence -- the curve above
 * can push a very light qualifying strike's raw output below this, so
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
 * NOT applied to the velocity path above: velocity is a one-shot
 * peak-acceleration estimate over a handful of samples during a ~15-60ms
 * window, where smoothing would blunt the exact transient it's trying
 * to measure; aftertouch is a continuous signal sent for as long as a
 * note is held, where smoothing is what makes it feel like modulation
 * instead of jitter. Unmeasured -- a starting guess at the right amount
 * of smoothing, not derived from the capture session above (that only
 * measured static full-press depth, not how noisy a held reading is). */
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
     * against -- set from the FIRST fresh Hall sample gathered after
     * touch begins (see the AWAITING_STRIKE branch below), not read
     * immediately at touch-down. An untouched pad is only covered by
     * hall.c's slow background round-robin, so whatever depth happens to
     * be cached the instant touch starts can be stale by many scan
     * cycles; touch immediately switches this pad to hall.c's
     * every-call priority scan, so comparing a fresh in-window sample
     * against that stale one could read as "movement" from nothing more
     * than ordinary drift over the stale gap, not a real press. Using
     * the first sample actually taken at the new fast rate as the
     * reference means every comparison is apples-to-apples, both sides
     * measured within the same strike-detection window. */
    float touch_start_depth;
    bool has_touch_start_depth;

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

    /* Up to 3 most recent (time, depth) samples seen since touch began. */
    uint32_t t[3];
    float d[3];
    uint8_t sample_count;
    uint32_t last_seen_sample_time_ms;

    float peak_accel;

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

static void begin_awaiting_strike(pad_expr_t *s, uint32_t now_ms) {
    s->state = PAD_STATE_AWAITING_STRIKE;
    s->touch_start_ms = now_ms;
    s->has_touch_start_depth = false;
    s->peak_depth_delta = 0.0f;
    s->sample_count = 0;
    s->peak_accel = 0.0f;
    s->last_seen_sample_time_ms = 0;
}

/* Pushes a fresh (time, depth) sample into the pad's short history and
 * updates its peak-acceleration-so-far once 3 samples are available. */
static void update_strike_history(pad_expr_t *s, uint32_t t_ms, float depth) {
    s->t[0] = s->t[1];
    s->d[0] = s->d[1];
    s->t[1] = s->t[2];
    s->d[1] = s->d[2];
    s->t[2] = t_ms;
    s->d[2] = depth;
    if (s->sample_count < 3u) {
        s->sample_count++;
    }

    if (s->sample_count < 3u) {
        return;
    }

    uint32_t dt1 = s->t[1] - s->t[0];
    uint32_t dt2 = s->t[2] - s->t[1];
    if (dt1 == 0u || dt2 == 0u) {
        return; /* duplicate timestamps -- shouldn't happen given the caller only pushes on a new sample_time_ms, but avoid a divide-by-zero regardless */
    }

    float v1 = (s->d[1] - s->d[0]) / (float)dt1;
    float v2 = (s->d[2] - s->d[1]) / (float)dt2;
    float accel = (v2 - v1) / ((float)(dt1 + dt2) / 2.0f);
    if (accel > s->peak_accel) {
        s->peak_accel = accel;
    }
}

static uint8_t velocity_from_peak_accel(float peak_accel) {
    if (peak_accel <= 0.0f) {
        return (uint8_t)MIN_VELOCITY;
    }
    if (peak_accel >= ACCEL_FULL_VELOCITY) {
        return 127u;
    }
    /* Power curve, not linear -- see this section's own header comment.
     * normalized in (0, 1), exponent > 1 suppresses the low end relative
     * to a straight line. */
    float normalized = peak_accel / ACCEL_FULL_VELOCITY;
    float curved = powf(normalized, VELOCITY_CURVE_EXPONENT);
    int vel = (int)(curved * 127.0f);
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
                begin_awaiting_strike(s, now_ms);
            }
            continue;
        }

        if (s->state == PAD_STATE_AWAITING_STRIKE) {
            /* Only read a fresh Hall sample while still touched -- once
             * released there's nothing new to gather, the commit
             * decision below just uses whatever peak was already
             * measured. */
            if (touched) {
                tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
                if (hs.valid && hs.sample_time_ms != s->last_seen_sample_time_ms) {
                    s->last_seen_sample_time_ms = hs.sample_time_ms;
                    float depth = (float)tiles_hall_get_depth(pad);
                    if (!s->has_touch_start_depth) {
                        /* First fresh sample since touch began -- see this
                         * field's comment for why this, not a read taken
                         * immediately at touch-down, is the right reference. */
                        s->touch_start_depth = depth;
                        s->has_touch_start_depth = true;
                    }
                    update_strike_history(s, hs.sample_time_ms, depth);
                    float delta = depth - s->touch_start_depth;
                    if (delta > s->peak_depth_delta) {
                        s->peak_depth_delta = delta;
                    }
                }
            }

            /* Gated on the PEAK depth reached, not the current instant --
             * see peak_depth_delta's own comment for why: a fast,
             * percussive strike can spring back (or end touch) before a
             * reading taken *right now* would still show it past
             * threshold, which silently lost real hard strikes before
             * this fix ("strong hard presses don't trigger anything"). */
            bool pressed = s->peak_depth_delta >= MIN_STRIKE_DEPTH_DELTA;
            bool have_accel_data = s->sample_count >= MIN_STRIKE_SAMPLES;

            /* "ready": a real press has been measured and there's enough
             * sample history to trust the accel estimate -- fires the
             * instant both are true, however fast or slow that took, no
             * fixed elapsed-ms floor. "timed_out": a real press has been
             * measured but accel history still isn't there yet after
             * MAX_STRIKE_WINDOW_MS (e.g. the background scan couldn't
             * keep up) -- commits anyway with whatever peak_accel was
             * observed rather than leaving a clearly-struck pad silent.
             * "commit_on_release": touch already ended, but a real press
             * was measured before it did -- commit now with whatever
             * accel data exists rather than discarding a genuine hit
             * just because contact happened to end first (a real,
             * common shape for a fast percussive strike). */
            uint32_t elapsed = now_ms - s->touch_start_ms;
            bool ready = touched && pressed && have_accel_data;
            bool timed_out = touched && pressed && !have_accel_data && (elapsed >= MAX_STRIKE_WINDOW_MS);
            bool commit_on_release = !touched && pressed;

            if (ready || timed_out || commit_on_release) {
                s->active_note = tiles_note_map_get_note(pad);
                uint8_t velocity = velocity_from_peak_accel(s->peak_accel);
                /* Temporary bring-up visibility -- prints exactly what
                 * the commit decision was based on, so a real-hardware
                 * session can read off actual numbers instead of
                 * guessing constants blind. Replace with a real
                 * usb_vendor/ diagnostics stream once that exists, same
                 * reasoning as touch.c/standby.c's own temporary
                 * prints. */
                printf("[expression] pad %u note-on: %s, peak_depth_delta=%d peak_accel=%d velocity=%u\n", pad,
                       commit_on_release ? "commit_on_release" : (timed_out ? "timed_out" : "ready"),
                       (int)s->peak_depth_delta, (int)s->peak_accel, velocity);
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
        if (s->has_touch_start_depth && (now_ms - s->note_on_ms) >= RETRIGGER_GRACE_MS &&
            (raw_depth - s->touch_start_depth) <= RETRIGGER_ARM_DEPTH_DELTA) {
            tiles_midi_note_off(s->active_note);
            tiles_haptics_stop(pad);
            begin_awaiting_strike(s, now_ms);
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
