#include "expression.h"

#include "board_pins.h"

#include "hall.h"
#include "touch.h"
#include "note_map.h"
#include "midi_out.h"

#include "pico/time.h"

/* Need at least this many fresh Hall samples after touch-down before
 * trusting an acceleration estimate (2 velocity values need 3 depth
 * samples), and won't commit before this many ms even if 3 samples
 * arrive faster than that -- guards against committing off noise on an
 * unusually fast first couple of samples. */
#define MIN_STRIKE_SAMPLES 3u
#define MIN_STRIKE_WINDOW_MS 15u

/* Safety timeout: commit regardless once this much time has passed
 * since touch-down, even without 3 clean samples -- e.g. if the
 * background/priority scan couldn't keep up. Ensures a note always
 * either fires or gets cancelled by release, never hangs forever. */
#define MAX_STRIKE_WINDOW_MS 60u

/* V1 PLACEHOLDER, unmeasured: raw Hall LSB-per-ms^2 that maps to full
 * MIDI velocity (127). There's no calibrated mT/LSB relationship yet
 * (see hall.h's "V1 scope"), so this is a starting guess, not a
 * derived value -- retune once real strikes can be measured. */
#define ACCEL_TO_VELOCITY_SCALE 0.05f

/* Even a strike weak enough to bottom out near MIN_STRIKE_WINDOW_MS's
 * detection floor should produce an audible note, not near-silence. */
#define MIN_VELOCITY 8u

/* V1 PLACEHOLDER, unmeasured: raw Hall LSB depth magnitude that maps to
 * full aftertouch (127). Same caveat as ACCEL_TO_VELOCITY_SCALE above. */
#define DEPTH_TO_AFTERTOUCH_FULL_SCALE 2000u

typedef enum {
    PAD_STATE_IDLE = 0,
    PAD_STATE_AWAITING_STRIKE,
    PAD_STATE_NOTE_ON,
} pad_expr_state_t;

typedef struct {
    pad_expr_state_t state;
    uint32_t touch_start_ms;

    /* Up to 3 most recent (time, depth) samples seen since touch began. */
    uint32_t t[3];
    float d[3];
    uint8_t sample_count;
    uint32_t last_seen_sample_time_ms;

    float peak_accel;

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
                update_strike_history(s, hs.sample_time_ms, (float)tiles_hall_get_depth(pad));
            }

            uint32_t elapsed = now_ms - s->touch_start_ms;
            bool ready = (s->sample_count >= MIN_STRIKE_SAMPLES) && (elapsed >= MIN_STRIKE_WINDOW_MS);
            bool timed_out = elapsed >= MAX_STRIKE_WINDOW_MS;

            if (ready || timed_out) {
                s->active_note = tiles_note_map_get_note(pad);
                tiles_midi_note_on(s->active_note, velocity_from_peak_accel(s->peak_accel));
                s->last_sent_aftertouch = 0xFFu;
                s->state = PAD_STATE_NOTE_ON;
            }
            continue;
        }

        /* PAD_STATE_NOTE_ON */
        if (!touched) {
            tiles_midi_note_off(s->active_note);
            s->state = PAD_STATE_IDLE;
            continue;
        }

        uint8_t at = aftertouch_from_depth(tiles_hall_get_depth(pad));
        if (at != s->last_sent_aftertouch) {
            s->last_sent_aftertouch = at;
            tiles_midi_send_poly_aftertouch(s->active_note, at);
        }
    }
}
