#pragma once

/* TILES's effects on core 1 (docs/architecture/synth-engines.md, "Effects"):
 * per track, overdrive and bit-crush inserts and the sidechain pump; shared
 * by every track through sends, an ensemble, a ping-pong delay and a plate
 * reverb. Plain C, tested on the computer (test/test_fx.c). */

#include <stdbool.h>
#include <stdint.h>

/* The most a block holds (the engine's 16). */
#define TILES_FX_BLOCK_MAX 16u

/* A sound's effects (synth_patch_t.fx), 0..1 each; a track can change its
 * own copy. */
typedef struct {
    float reverb, delay, chorus; /* sends */
    float drive;                 /* overdrive */
    float dark;                  /* overdrive's tone: 0 open, 1 dark */
    float crush;                 /* bit-crush: fewer bits and a lower rate together */
    float pump;                  /* ducked by the kick, or by the beat */
} tiles_fx_amounts_t;

/* The shared effects' settings. */
typedef struct {
    float reverb_decay;       /* 0..1 */
    float reverb_damping;     /* 0..1: the highs fade sooner */
    float reverb_predelay_ms; /* 0..50 */
    float reverb_mod;         /* 0..1: the tank's slow movement */
    float delay_sixteenths;   /* echo time; 3 = dotted 1/8 (halved until it fits 0.5 s) */
    float delay_feedback;     /* 0..0.95 */
    float delay_tone;         /* 0 dark .. 1 bright */
    float delay_to_reverb;    /* how much of the echoes the plate hears */
    float chorus_rate;        /* 0..1 */
    float chorus_depth;       /* 0..1 */
    float pump_release;       /* beats the pump takes to recover */
} tiles_fx_bus_t;

extern const tiles_fx_bus_t g_tiles_fx_bus_default;

/* Takes the delay memory (TILES_FX_MEMORY_BYTES, fx_memory.h), clears it,
 * and ends any bypass. Settings are kept. */
void tiles_fx_init(void *memory);
void tiles_fx_set_bus(const tiles_fx_bus_t *bus);
const tiles_fx_bus_t *tiles_fx_bus(void);

/* Bypassed (the memory lent out), the shared effects return nothing and
 * touch no memory. */
void tiles_fx_set_bypass(bool bypass);

/* A track's insert state. */
typedef struct {
    float x_prev[2], f_prev[2]; /* the overdrive's last input and antiderivative */
    float dc_x[2], dc_y[2];     /* its DC blocker */
    float tone[2];              /* its tone low-pass */
    float hold[2], crush_phase; /* bit-crush */
} tiles_fx_insert_t;

void tiles_fx_insert_reset(tiles_fx_insert_t *s);

/* Once a block, before the inserts: moves the pump on. */
void tiles_fx_block_begin(uint32_t n, float beats_per_second);

/* Overdrive, bit-crush and the pump on a track's block, in place. */
void tiles_fx_insert(tiles_fx_insert_t *s, const tiles_fx_amounts_t *fx, float *l, float *r, uint32_t n);

/* The pump: the kick (drum slot 1) ducks now; a beat ducks unless a kick
 * just did. */
void tiles_fx_pump_kick(void);
void tiles_fx_pump_beat(void);

/* The shared effects: mono sends in, their returns added into l and r. */
void tiles_fx_busses(const float *chorus_in, const float *delay_in, const float *reverb_in, float *l, float *r,
                     uint32_t n, float beats_per_second);

/* A test: cycles per sample of one effect on noise (TILES_FX_BENCH_*),
 * on the effects' own state: only while core 1 isn't rendering. */
enum {
    TILES_FX_BENCH_CHORUS = 0,
    TILES_FX_BENCH_DELAY,
    TILES_FX_BENCH_REVERB,
    TILES_FX_BENCH_DRIVE,
    TILES_FX_BENCH_CRUSH,
    TILES_FX_BENCH_PUMP,
    TILES_FX_BENCH_COUNT,
};
uint32_t tiles_fx_bench(uint32_t which, uint32_t frames, uint32_t (*cycles)(void));

/* The plate's lines' extent in its front and tank rings (a test). */
uint32_t tiles_fx_reverb_extent(bool tank);
