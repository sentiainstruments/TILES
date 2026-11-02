#include "capture.h"

#include <stddef.h>

static tiles_capture_record_t s_records[TILES_CAPTURE_MAX];
static uint16_t s_count;
static uint8_t s_pad;
static bool s_running;
static tiles_capture_record_t s_current; /* the pass being recorded */
static bool s_open;

void tiles_capture_start(uint8_t pad) {
    s_count = 0u;
    s_open = false;
    s_pad = pad;
    s_running = pad >= 1u && pad <= 24u;
}

void tiles_capture_stop(void) {
    s_running = false;
    s_open = false;
}

bool tiles_capture_running(void) {
    return s_running;
}

uint8_t tiles_capture_pad(void) {
    return s_pad;
}

uint16_t tiles_capture_count(void) {
    return s_count;
}

const tiles_capture_record_t *tiles_capture_get(uint16_t index) {
    return index < s_count ? &s_records[index] : NULL;
}

void tiles_capture_pass_begin(uint8_t pad, uint32_t now_ms, int16_t x, int16_t y, int16_t z, bool valid,
                              bool touched, uint16_t depth, uint8_t state) {
    if (!s_running || pad != s_pad) {
        return;
    }
    if (s_count >= TILES_CAPTURE_MAX) {
        s_running = false;
        return;
    }
    s_current = (tiles_capture_record_t){
        .t_ms = now_ms,
        .x = x,
        .y = y,
        .z = z,
        .depth = depth,
        .flags = (uint8_t)((valid ? TILES_CAPTURE_VALID : 0u) | (touched ? TILES_CAPTURE_TOUCHED : 0u)),
        .state = state,
    };
    s_open = true;
}

void tiles_capture_bend_started(uint8_t pad, uint16_t center) {
    if (s_open && pad == s_pad) {
        s_current.flags |= TILES_CAPTURE_STARTED;
        s_current.bend = center;
    }
}

void tiles_capture_bend_stepped(uint8_t pad, float smoothed_depth, bool produced, uint16_t bend) {
    if (s_open && pad == s_pad) {
        s_current.flags |= TILES_CAPTURE_STEPPED;
        s_current.smoothed_depth = smoothed_depth;
        if (produced) {
            s_current.flags |= TILES_CAPTURE_BENT;
            s_current.bend = bend;
        }
    }
}

void tiles_capture_pass_end(void) {
    if (s_open) {
        s_records[s_count++] = s_current;
        s_open = false;
    }
}
