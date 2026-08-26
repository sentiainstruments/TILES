#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "midi_out.h"
#include "haptics.h"

#include "pico/time.h"

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

/* Real captured data, not a blind placeholder: the same ~140-touch
 * debug-console session referenced in this section's header comment
 * measured peak_accel spanning roughly 0-23 (raw Hall units per ms^2)
 * across real strikes, the vast majority under 18 -- at the old 0.05
 * scale, every single one of those floored to velocity 8, which is
 * exactly the "it's all the same velocity" bug reported. 4.5 maps that
 * observed range across most of MIDI's useful velocity span (23*4.5 ≈
 * 104, 18*4.5=81, 10*4.5=45) while leaving headroom for a genuinely
 * harder strike than anything in this particular capture to still climb
 * higher before clipping at 127. There's still no calibrated mT/LSB
 * relationship (see hall.h's "V1 scope"), so this is real-data-informed
 * rather than derived from first principles -- retune if strikes
 * clearly harder than this session's still cluster near one end. */
#define ACCEL_TO_VELOCITY_SCALE 4.5f

/* Even a strike weak enough to barely clear MIN_STRIKE_DEPTH_DELTA
 * should produce an audible note, not near-silence. */
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

    /* Up to 3 most recent (time, depth) samples seen since touch began. */
    uint32_t t[3];
    float d[3];
    uint8_t sample_count;
    uint32_t last_seen_sample_time_ms;

    float peak_accel;

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
    int vel = (int)(peak_accel * ACCEL_TO_VELOCITY_SCALE);
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
        bool touched = tiles_touch_is_touched(pad);

        if (s->state == PAD_STATE_IDLE) {
            if (touched) {
                begin_awaiting_strike(s, now_ms);
            }
            continue;
        }

        if (s->state == PAD_STATE_AWAITING_STRIKE) {
            if (!touched) {
                /* Released before we ever committed to a note -- a
                 * light tap, not a real press. No note was ever sent. */
                s->state = PAD_STATE_IDLE;
                continue;
            }

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
            }

            float depth_delta = s->has_touch_start_depth ? (float)tiles_hall_get_depth(pad) - s->touch_start_depth
                                                          : 0.0f;
            bool pressed = s->has_touch_start_depth && depth_delta >= MIN_STRIKE_DEPTH_DELTA;
            bool have_accel_data = s->sample_count >= MIN_STRIKE_SAMPLES;

            /* "ready": a real press has been measured and there's enough
             * sample history to trust the accel estimate -- fires the
             * instant both are true, however fast or slow that took, no
             * fixed elapsed-ms floor. "timed_out": a real press has been
             * measured but accel history still isn't there yet after
             * MAX_STRIKE_WINDOW_MS (e.g. the background scan couldn't
             * keep up) -- commits anyway with whatever peak_accel was
             * observed rather than leaving a clearly-struck pad silent. */
            uint32_t elapsed = now_ms - s->touch_start_ms;
            bool ready = pressed && have_accel_data;
            bool timed_out = pressed && !have_accel_data && (elapsed >= MAX_STRIKE_WINDOW_MS);

            if (ready || timed_out) {
                s->active_note = tiles_note_map_get_note(pad);
                uint8_t velocity = velocity_from_peak_accel(s->peak_accel);
                /* Temporary bring-up visibility (real feedback: touch
                 * alone was firing notes) -- prints exactly what
                 * MIN_STRIKE_DEPTH_DELTA's decision was based on, so a
                 * real-hardware session can read off actual depth-delta/
                 * accel numbers for both light touches and real presses
                 * instead of guessing the threshold again. Replace with a
                 * real usb_vendor/ diagnostics stream once that exists,
                 * same reasoning as touch.c/standby.c's own temporary
                 * prints. */
                printf("[expression] pad %u note-on: %s, depth_delta=%d peak_accel=%d velocity=%u\n", pad,
                       timed_out ? "timed_out" : "ready", (int)depth_delta, (int)s->peak_accel, velocity);
                tiles_midi_note_on(s->active_note, velocity);
                /* Same velocity value driving both -- "mapped to the
                 * velocity curve by default" means the kick and the MIDI
                 * note agree exactly, not two independent estimates. */
                tiles_haptics_trigger_kick(pad, velocity);
                s->last_sent_aftertouch = 0xFFu;
                /* Seed the smoother with the real depth right now rather
                 * than 0 -- see the field's own comment. */
                s->smoothed_depth = (float)tiles_hall_get_depth(pad);
                s->state = PAD_STATE_NOTE_ON;
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

        /* EMA toward this scan's raw depth -- both a strengthening press
         * (more pressure past the strike) and an easing-off one (less
         * pressure, still touching) move it, smoothly. */
        float raw_depth = (float)tiles_hall_get_depth(pad);
        s->smoothed_depth += AFTERTOUCH_SMOOTHING_ALPHA * (raw_depth - s->smoothed_depth);

        uint8_t at = aftertouch_from_depth((uint16_t)s->smoothed_depth);
        if (at != s->last_sent_aftertouch) {
            s->last_sent_aftertouch = at;
            tiles_midi_send_poly_aftertouch(s->active_note, at);
            tiles_haptics_set_sustain_level(pad, at);
        }
    }
}
