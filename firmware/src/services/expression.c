#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "midi_out.h"
#include "haptics.h"

#include "pico/time.h"

#include <stdio.h>

/* Need at least this many fresh Hall samples after touch-down before
 * trusting an acceleration estimate (2 velocity values need 3 depth
 * samples), and won't commit before this many ms even if 3 samples
 * arrive faster than that -- guards against committing off noise on an
 * unusually fast first couple of samples. */
#define MIN_STRIKE_SAMPLES 3u
#define MIN_STRIKE_WINDOW_MS 15u

/* Safety timeout: once a real press has actually been detected (see
 * MIN_STRIKE_DEPTH_DELTA below), commit regardless of sample count once
 * this much time has passed since touch-down -- e.g. if the background/
 * priority scan couldn't keep up and never gathered 3 clean samples.
 * Does NOT by itself make a bare touch fire a note -- see below. */
#define MAX_STRIKE_WINDOW_MS 60u

/* Minimum real depth travel (Hall units) since touch-down before a
 * touch counts as an actual press worth firing a note for, rather than
 * a light touch/rest with no real key motion. Real feedback: "touch is
 * triggering notes not press velocity... the lightest touch of
 * capacitance is doing this without even getting to a velocity curve"
 * -- MAX_STRIKE_WINDOW_MS's safety-timeout fallback used to fire a note
 * at floor velocity purely because touch had lasted 60ms, with zero
 * regard for whether the pad had actually moved at all; a bare
 * capacitive touch with no press reliably hit that path. Now both the
 * "ready" and "timed out" commit conditions below require this much
 * measured travel first -- a touch that never presses just sits in
 * PAD_STATE_AWAITING_STRIKE until release cancels it with no note ever
 * sent, matching how a real key requires an actual press, not just
 * contact.
 *
 * First attempt at this (15) still fired on a light touch on real
 * hardware -- traced to a second, independent bug: the reference depth
 * was being read immediately at touch-down, from whatever hall.c had
 * cached from its slow background round-robin (an untouched pad isn't
 * scanned every call), while the comparison reading came from the fast
 * every-call priority scan touch switches this pad to. Comparing a
 * stale, possibly many-scans-old reading against a fresh one could read
 * as "movement" from nothing more than ordinary drift over that stale
 * gap. Fixed below (see has_touch_start_depth) by using the first
 * sample actually taken at the new fast rate as the reference instead,
 * so both sides of the comparison are apples-to-apples. Raised to 30 at
 * the same time as a wider safety margin while that fix is unverified
 * on hardware. Still unmeasured -- a starting guess for "clearly more
 * than capacitive-only noise," a small fraction of the ~900-unit
 * full-press range the real calibration session measured (that session
 * measured full press, not the noise floor of an untouched-but-contacted
 * pad, so there's no equivalent real data for this specific number yet).
 * `[expression] pad N committed...` below prints the actual measured
 * delta on every note-on specifically so the next real-hardware session
 * can read off real numbers instead of guessing a third time. */
#define MIN_STRIKE_DEPTH_DELTA 30.0f

/* V1 PLACEHOLDER, unmeasured: raw Hall LSB-per-ms^2 that maps to full
 * MIDI velocity (127). There's no calibrated mT/LSB relationship yet
 * (see hall.h's "V1 scope"), so this is a starting guess, not a
 * derived value -- retune once real strikes can be measured. */
#define ACCEL_TO_VELOCITY_SCALE 0.05f

/* Even a strike weak enough to bottom out near MIN_STRIKE_WINDOW_MS's
 * detection floor should produce an audible note, not near-silence. */
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

            uint32_t elapsed = now_ms - s->touch_start_ms;
            bool ready = pressed && (s->sample_count >= MIN_STRIKE_SAMPLES) && (elapsed >= MIN_STRIKE_WINDOW_MS);
            bool timed_out = pressed && (elapsed >= MAX_STRIKE_WINDOW_MS);

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
