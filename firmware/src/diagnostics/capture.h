#pragma once

/* Sensor capture for tuning: one pad's raw Hall reading and what the
 * expression code made of it, once per main-loop pass, into RAM. Armed and
 * read over the settings interface (CAPTURE ...); tools/tiles_control.py
 * saves it as a CSV that firmware/test/bend_replay.c replays, and
 * tools/studio/ watches it live. Never prints. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_CAPTURE_MAX 4000u /* ~8 s at one record per ~2 ms pass */

#define TILES_CAPTURE_VALID 0x01u        /* the Hall sample is valid */
#define TILES_CAPTURE_TOUCHED 0x02u      /* raw touch */
#define TILES_CAPTURE_STARTED 0x04u      /* bend_core started (pitch bend on) */
#define TILES_CAPTURE_STEPPED 0x08u      /* bend_core stepped */
#define TILES_CAPTURE_BENT 0x10u         /* ...and gave `bend` */
#define TILES_CAPTURE_STROKE 0x20u       /* this sample went to the press path learner */
#define TILES_CAPTURE_STROKE_BEGIN 0x40u /* a new stroke began (touch, or retrigger) */
#define TILES_CAPTURE_NOTE_ON 0x80u      /* the note fired (stroke learned or refused); `velocity` */
#define TILES_CAPTURE_PRESSURE 0x100u    /* a held note's pass: `smoothed_depth`, `aftertouch` */

typedef struct {
    uint32_t t_ms;
    int16_t x, y, z;
    uint16_t depth;       /* Hall depth from the rest baseline */
    float smoothed_depth; /* pressure, and what bend_core stepped with */
    uint16_t bend;        /* bend_core's output (BENT), or the centre (STARTED) */
    uint16_t flags;
    uint8_t state; /* expression's pad state at the start of the pass */
    uint8_t velocity;
    uint8_t aftertouch;
} tiles_capture_record_t;

/* Clears and starts recording pad 1-24. START stops by itself when full;
 * WATCH never stops, keeping the newest TILES_CAPTURE_MAX. */
void tiles_capture_start(uint8_t pad);
void tiles_capture_watch(uint8_t pad);
void tiles_capture_stop(void);
bool tiles_capture_running(void);
bool tiles_capture_watching(void);
uint8_t tiles_capture_pad(void); /* 0 = never started */
uint16_t tiles_capture_count(void); /* records held */
uint32_t tiles_capture_total(void); /* records since start: the next record's sequence number */
/* By sequence number since start; NULL if not held (not yet, or overwritten
 * while watching). Without watching, sequence = index. */
const tiles_capture_record_t *tiles_capture_get(uint32_t seq);

/* From expression.c's scan, for the recorded pad: pass_begin first and
 * pass_end after the pad loop, the rest as they happen. */
void tiles_capture_pass_begin(uint8_t pad, uint32_t now_ms, int16_t x, int16_t y, int16_t z, bool valid,
                              bool touched, uint16_t depth, uint8_t state);
void tiles_capture_stroke_begun(uint8_t pad);
void tiles_capture_stroke_sampled(uint8_t pad);
void tiles_capture_note_on(uint8_t pad, uint8_t velocity);
void tiles_capture_bend_started(uint8_t pad, uint16_t center);
void tiles_capture_pressure(uint8_t pad, float smoothed_depth, uint8_t aftertouch);
void tiles_capture_bend_stepped(uint8_t pad, bool produced, uint16_t bend);
void tiles_capture_pass_end(void);
