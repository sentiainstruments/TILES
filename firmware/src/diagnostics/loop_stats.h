#pragma once

/* Main-loop pass timing, for finding lag: the average and longest pass of
 * the last completed 1 s window, and the longest since boot. main.c marks
 * each pass; the settings shell's INFO reports it (loop.*). A flash write
 * or a slow I2C burst shows up as a long pass. */

#include <stdint.h>

typedef struct {
    uint32_t avg_us;      /* last 1 s window */
    uint32_t max_us;      /* last 1 s window */
    uint32_t passes;      /* in that window */
    uint32_t max_ever_us; /* since boot (the first second excluded) */
} tiles_loop_stats_t;

/* Call once at the top of every pass with time_us_32(). */
void tiles_loop_stats_mark(uint32_t now_us);

tiles_loop_stats_t tiles_loop_stats_get(void);
