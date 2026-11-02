#pragma once

/* Sensor capture for tuning: one pad's raw Hall reading and what bend_core
 * was fed, once per main-loop pass, into RAM. Armed and read over the
 * settings interface (CAPTURE ...); tools/tiles_control.py saves it as a
 * CSV that firmware/test/bend_replay.c replays. Never prints. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_CAPTURE_MAX 4000u /* ~8 s at one record per ~2 ms pass */

#define TILES_CAPTURE_VALID 0x01u   /* the Hall sample is valid */
#define TILES_CAPTURE_TOUCHED 0x02u /* raw touch */
#define TILES_CAPTURE_STARTED 0x04u /* bend_core started: a note-on */
#define TILES_CAPTURE_STEPPED 0x08u /* bend_core stepped */
#define TILES_CAPTURE_BENT 0x10u    /* ...and gave `bend` */

typedef struct {
    uint32_t t_ms;
    int16_t x, y, z;
    uint16_t depth;       /* Hall depth from the rest baseline */
    float smoothed_depth; /* what bend_core stepped with */
    uint16_t bend;        /* bend_core's output (BENT), or the centre (STARTED) */
    uint8_t flags;
    uint8_t state; /* expression's pad state at the start of the pass */
} tiles_capture_record_t;

/* Clears and starts recording pad 1-24; stops by itself when full. */
void tiles_capture_start(uint8_t pad);
void tiles_capture_stop(void);
bool tiles_capture_running(void);
uint8_t tiles_capture_pad(void); /* 0 = never started */
uint16_t tiles_capture_count(void);
const tiles_capture_record_t *tiles_capture_get(uint16_t index); /* NULL past the count */

/* From expression.c's scan, for the recorded pad, in this order. */
void tiles_capture_pass_begin(uint8_t pad, uint32_t now_ms, int16_t x, int16_t y, int16_t z, bool valid,
                              bool touched, uint16_t depth, uint8_t state);
void tiles_capture_bend_started(uint8_t pad, uint16_t center);
void tiles_capture_bend_stepped(uint8_t pad, float smoothed_depth, bool produced, uint16_t bend);
void tiles_capture_pass_end(void);
