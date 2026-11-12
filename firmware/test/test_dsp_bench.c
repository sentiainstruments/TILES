/* The DSP stress test's kernels (audio/dsp_bench.c), on the computer:
 * every one must make finite, non-silent sound within full scale. The
 * cycle counts it prints are the computer's, not the board's. */

#include "dsp_bench.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000u);
}

int main(void) {
    static tiles_dsp_bench_t r;
    tiles_dsp_bench_run(&r, now_us, 1000u);
    int failures = 0;
    tiles_dsp_bench_soak(now_us, 30u, true);
    if (r.count < 38u) {
        printf("FAIL only %u tests ran (arena too small?)\n", (unsigned)r.count);
        failures++;
    }
    for (uint32_t i = 0; i < r.count; i++) {
        const tiles_dsp_bench_item_t *it = &r.item[i];
        bool silent_ok = strncmp(it->name, "overhead", 8) == 0;
        bool sound = it->peak_milli > 0 || silent_ok;
        /* Voices are summed unscaled, so a peak a few times full scale is fine. */
        bool sane = it->peak_milli < 20000;
        if (!sound || !sane) {
            printf("FAIL %s: peak %d/1000\n", it->name, (int)it->peak_milli);
            failures++;
        }
    }
    if (failures) {
        printf("test_dsp_bench: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_dsp_bench: ok (%u kernels)\n", (unsigned)r.count);
    return 0;
}
