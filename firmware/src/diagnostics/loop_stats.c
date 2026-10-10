#include "loop_stats.h"

#include <stdbool.h>

#define WINDOW_US 1000000u
#define BOOT_SETTLE_US 1000000u

static bool s_started;
static uint32_t s_boot_us, s_last_us, s_window_start_us;
static uint32_t s_sum_us, s_count, s_max_us;
static tiles_loop_stats_t s_done;

void tiles_loop_stats_mark(uint32_t now_us) {
    if (!s_started) {
        s_started = true;
        s_boot_us = s_last_us = s_window_start_us = now_us;
        return;
    }
    uint32_t pass = now_us - s_last_us;
    s_last_us = now_us;
    s_sum_us += pass;
    s_count++;
    if (pass > s_max_us) {
        s_max_us = pass;
    }
    if (now_us - s_boot_us > BOOT_SETTLE_US && pass > s_done.max_ever_us) {
        s_done.max_ever_us = pass;
    }
    if (now_us - s_window_start_us >= WINDOW_US) {
        s_done.avg_us = s_count ? s_sum_us / s_count : 0u;
        s_done.max_us = s_max_us;
        s_done.passes = s_count;
        s_window_start_us = now_us;
        s_sum_us = s_count = s_max_us = 0u;
    }
}

tiles_loop_stats_t tiles_loop_stats_get(void) {
    return s_done;
}
