#pragma once

/* DSP stress test (bench builds only, cmake -DTILES_DSP_BENCH=ON): times
 * high-quality prototypes of each planned engine and effect on core 1 and
 * reports cycles per sample. Plain C; test/test_dsp_bench.c runs it on the
 * computer to check every kernel makes finite, non-silent sound.
 * docs/architecture/audio-engine.md has the plan these numbers check. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_DSP_BENCH_MAX_ITEMS 40u

typedef struct {
    char name[24];
    uint32_t units;        /* voices, grains or 1 */
    uint32_t milli_cycles; /* cycles per output sample per unit, x1000 */
    int32_t peak_milli;    /* output peak x1000, a sanity check */
} tiles_dsp_bench_item_t;

typedef struct {
    uint32_t count;
    uint32_t clk_mhz;
    tiles_dsp_bench_item_t item[TILES_DSP_BENCH_MAX_ITEMS];
} tiles_dsp_bench_t;

/* Runs every test (a few seconds). now_us: a free-running microsecond
 * clock; clk_mhz: the CPU clock the cycles are counted in. */
void tiles_dsp_bench_run(tiles_dsp_bench_t *out, uint32_t (*now_us)(void), uint32_t clk_mhz);

/* Plays the 8 sample slots from flash in real time for `ms`, through the
 * XIP cache core 0 shares or around it, to see what that does to core 0's
 * loop. */
void tiles_dsp_bench_soak(uint32_t (*now_us)(void), uint32_t ms, bool cached);
