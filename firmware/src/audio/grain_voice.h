#pragma once

/* The granular engine's voice: a cloud of up to TILES_GRAIN_MAX short
 * grains, each a windowed slice of a source at its own pitch, place and
 * side. The source is a wavetable bank (a frame a grain, band-limited) or
 * the tape: a drum slot copied into the delay's memory (audio_engine.c).
 * Pressure and slide move the place, size, density and spray; a filter
 * and the envelopes follow, as in the wavetable voice
 * (docs/architecture/synth-engines.md, "Granular"). Plain C, tested on
 * the computer (test/test_grain.c). */

#include "synth_voice.h"

#define TILES_GRAIN_MAX 8u

enum { TILES_GRAIN_SRC_WAVE = 0, TILES_GRAIN_SRC_TAPE };

/* A granular sound's own settings (synth_patch_t.grain). The place is the
 * patch's wave position and what moves it; the filter, envelopes, LFO and
 * levels are the patch's too. */
typedef struct tiles_grain_params {
    uint8_t source;        /* TILES_GRAIN_SRC_* */
    uint8_t slot;          /* the tape's drum slot, 0-7 */
    uint8_t root;          /* the tape plays at its own speed on this note */
    float size_ms;         /* a grain's length */
    float density;         /* grains a second */
    float jitter;          /* 0 evenly spaced .. 1 at random */
    float spray;           /* 0..1: how far each grain's place wanders */
    float scan;            /* the place moving: tape, x its own speed; wave, sweeps a second */
    float scatter_cents;   /* each grain's pitch, at random +- */
    float octave_up;       /* a grain's chance of an octave up */
    float octave_down;     /* ... of an octave down */
    float fifth;           /* ... of a fifth up */
    float reverse;         /* ... of playing backwards */
    float spread;          /* 0..1: across the stereo field */
    float skew;            /* 0 a bell .. 1 a strike and a tail */
    float size_pressure;   /* octaves at full pressure */
    float density_pressure;
    float spray_pressure;  /* spray added at full pressure */
    float spray_slide;
    float scatter_slide;   /* cents added at full slide */
} tiles_grain_params_t;

typedef struct {
    uint32_t phase;      /* wave: a 32-bit cycle; tape: samples, 16.16 */
    float ratio;         /* pitch against the voice's; negative: backwards */
    const int16_t *src;  /* wave: its frame in its level */
    float u, slope;      /* the window's triangle 0..1..0, its step a control step (falling: negative) */
    int16_t gl, gr;      /* its side, Q15 */
    uint8_t frame, level, fshift, on;
} tiles_grain_t;

/* 364 bytes on the board: the voices' union's size before it (FM's). */
typedef struct {
    tiles_grain_t g[TILES_GRAIN_MAX];
    synth_env_t amp, mod;
    synth_svf_t svf;     /* coefficients, and the left channel's state */
    float right[2];      /* the right channel's filter state */
    float oct;           /* the cutoff octaves the coefficients are for */
    float level;         /* last control step's output level */
    float pressure_s, slide_s;
    float pitch, glide_coef;
    float semis;         /* the pitch `rate` is for */
    float rate;          /* wave: cycles a sample; tape: its samples a sample */
    float lfo_phase, lfo_held;
    float next;          /* samples to the next grain */
    float scan_pos;      /* the scan's offset of the place */
    float norm;          /* 1 / sqrt(grains overlapping), set as grains start */
    uint32_t rng;
} tiles_grain_voice_t;

void tiles_grain_voice_start(tiles_grain_voice_t *v, const synth_patch_t *patch, const synth_input_t *in,
                             uint32_t seed, bool sounding, bool legato);
void tiles_grain_voice_release(tiles_grain_voice_t *v);
void tiles_grain_voice_kill(tiles_grain_voice_t *v);

static inline bool tiles_grain_voice_active(const tiles_grain_voice_t *v) {
    return v->amp.stage != SYNTH_ENV_IDLE;
}

static inline float tiles_grain_voice_level(const tiles_grain_voice_t *v) {
    return v->amp.value;
}

/* Grains sounding now. */
uint32_t tiles_grain_voice_grains(const tiles_grain_voice_t *v);

/* Adds n (a multiple of SYNTH_CTRL) frames into l and r. */
void tiles_grain_voice_render(tiles_grain_voice_t *v, const synth_patch_t *patch, const synth_context_t *ctx,
                              const synth_input_t *in, float *l, float *r, uint32_t n);

/* Grains overlapping on average at rest (a first cost estimate). */
float tiles_grain_overlap(const tiles_grain_params_t *g);

/* The granular factory sounds' settings (audio/grain_patches.c). */
enum {
    TILES_GRAIN_CLOUD = 0,
    TILES_GRAIN_DUST,
    TILES_GRAIN_SHIMMER,
    TILES_GRAIN_SCATTER,
    TILES_GRAIN_WHISPER,
    TILES_GRAIN_FACTORY_COUNT
};
extern const tiles_grain_params_t g_grain_factory[TILES_GRAIN_FACTORY_COUNT];
