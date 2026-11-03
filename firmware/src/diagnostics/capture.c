#include "capture.h"

#include <stddef.h>

static tiles_capture_record_t s_records[TILES_CAPTURE_MAX];
static uint32_t s_total; /* records since start; the newest is at (s_total - 1) % MAX */
static uint8_t s_pad;
static bool s_running;
static bool s_watching;
static tiles_capture_record_t s_current; /* the pass being recorded */
static bool s_open;

static void begin(uint8_t pad, bool watching) {
    s_total = 0u;
    s_open = false;
    s_pad = pad;
    s_running = pad >= 1u && pad <= 24u;
    s_watching = s_running && watching;
}

void tiles_capture_start(uint8_t pad) {
    begin(pad, false);
}

void tiles_capture_watch(uint8_t pad) {
    begin(pad, true);
}

void tiles_capture_stop(void) {
    s_running = false;
    s_watching = false;
    s_open = false;
}

bool tiles_capture_running(void) {
    return s_running;
}

bool tiles_capture_watching(void) {
    return s_watching;
}

uint8_t tiles_capture_pad(void) {
    return s_pad;
}

uint16_t tiles_capture_count(void) {
    return (uint16_t)(s_total < TILES_CAPTURE_MAX ? s_total : TILES_CAPTURE_MAX);
}

uint32_t tiles_capture_total(void) {
    return s_total;
}

const tiles_capture_record_t *tiles_capture_get(uint32_t seq) {
    if (seq >= s_total || s_total - seq > TILES_CAPTURE_MAX) {
        return NULL;
    }
    return &s_records[seq % TILES_CAPTURE_MAX];
}

void tiles_capture_pass_begin(uint8_t pad, uint32_t now_ms, int16_t x, int16_t y, int16_t z, bool valid,
                              bool touched, uint16_t depth, uint8_t state) {
    if (!s_running || pad != s_pad) {
        return;
    }
    if (!s_watching && s_total >= TILES_CAPTURE_MAX) {
        s_running = false;
        return;
    }
    s_current = (tiles_capture_record_t){
        .t_ms = now_ms,
        .x = x,
        .y = y,
        .z = z,
        .depth = depth,
        .flags = (uint16_t)((valid ? TILES_CAPTURE_VALID : 0u) | (touched ? TILES_CAPTURE_TOUCHED : 0u)),
        .state = state,
    };
    s_open = true;
}

static bool recording(uint8_t pad) {
    return s_open && pad == s_pad;
}

void tiles_capture_stroke_begun(uint8_t pad) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_STROKE_BEGIN;
    }
}

void tiles_capture_stroke_sampled(uint8_t pad) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_STROKE;
    }
}

void tiles_capture_note_on(uint8_t pad, uint8_t velocity, uint32_t strike_ms, float peak_depth) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_NOTE_ON;
        s_current.velocity = velocity;
        s_current.strike_ms = (uint16_t)(strike_ms < 65535u ? strike_ms : 65535u);
        s_current.peak_depth = (uint16_t)(peak_depth < 65535.0f ? peak_depth : 65535.0f);
    }
}

void tiles_capture_note_off(uint8_t pad, uint8_t release_velocity) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_NOTE_OFF;
        s_current.velocity = release_velocity;
    }
}

void tiles_capture_bend_started(uint8_t pad, uint16_t center) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_STARTED;
        s_current.bend = center;
    }
}

void tiles_capture_pressure(uint8_t pad, float smoothed_depth, uint8_t aftertouch) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_PRESSURE;
        s_current.smoothed_depth = smoothed_depth;
        s_current.aftertouch = aftertouch;
    }
}

void tiles_capture_bend_stepped(uint8_t pad, bool produced, uint16_t bend) {
    if (recording(pad)) {
        s_current.flags |= TILES_CAPTURE_STEPPED;
        if (produced) {
            s_current.flags |= TILES_CAPTURE_BENT;
            s_current.bend = bend;
        }
    }
}

/* Watching: a pass that only repeats the last one (an untouched pad's
 * round-robin sample, re-seen every pass) isn't kept, so the live stream
 * fits the USB pipe and catches up at rest. */
static bool repeats_last(const tiles_capture_record_t *r) {
    if (s_total == 0u) {
        return false;
    }
    const tiles_capture_record_t *last = &s_records[(s_total - 1u) % TILES_CAPTURE_MAX];
    const uint16_t events = TILES_CAPTURE_STROKE | TILES_CAPTURE_STROKE_BEGIN | TILES_CAPTURE_NOTE_ON |
                            TILES_CAPTURE_STARTED | TILES_CAPTURE_NOTE_OFF;
    return (r->flags & events) == 0u && r->x == last->x && r->y == last->y && r->z == last->z &&
           r->depth == last->depth && r->state == last->state &&
           (r->flags & (TILES_CAPTURE_VALID | TILES_CAPTURE_TOUCHED)) ==
               (last->flags & (TILES_CAPTURE_VALID | TILES_CAPTURE_TOUCHED));
}

void tiles_capture_pass_end(void) {
    if (s_open && s_watching && repeats_last(&s_current)) {
        s_open = false;
        return;
    }
    if (s_open) {
        s_records[s_total % TILES_CAPTURE_MAX] = s_current;
        s_total++;
        s_open = false;
    }
}
