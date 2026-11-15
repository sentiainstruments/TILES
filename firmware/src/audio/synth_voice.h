#pragma once

/* The wavetable engine's voice: up to 3 unison copies reading a band-
 * limited wavetable, drive and wavefold, a state-variable or ladder
 * filter, two envelopes, an LFO, glide. Pressure opens the filter and
 * sweeps the wavetable, slide (CC74) moves the timbre, bend is pitch
 * (docs/architecture/synth-engines.md). Plain C, tested on the computer
 * (test/test_synth.c). */

#include "synth_dsp.h"
#include "wavetable.h"

#include <stdbool.h>
#include <stdint.h>

#define SYNTH_UNISON_MAX 3u

enum { SYNTH_FILTER_LP12 = 0, SYNTH_FILTER_BP12, SYNTH_FILTER_HP12, SYNTH_FILTER_LADDER24 };
enum { SYNTH_POLY = 0, SYNTH_MONO, SYNTH_LEGATO };

/* LFO rate: free in Hz, or synced to the tempo as beats per cycle. */
typedef struct {
    uint8_t shape;
    bool synced;
    float rate; /* Hz, or beats per cycle when synced (0.25 = a 1/16 note) */
    bool retrigger;
    float delay_ms; /* fades in after this (vibrato) */
} synth_lfo_cfg_t;

typedef struct {
    const char *name;
    uint8_t bank;
    /* Wavetable position 0..1 and what moves it (in table widths). */
    float wave, wave_pressure, wave_slide, wave_env, wave_lfo;
    uint8_t unison;     /* 1..3 */
    float detune_cents; /* outer copies, +- */
    float width;        /* their stereo spread 0..1 */
    bool phase_reset;   /* every note starts in phase (plucks, basses) */
    float drive;        /* 0 = clean; up to ~4 */
    float fold;         /* 0..1 */
    uint8_t filter;
    float cutoff_hz, resonance;
    float key_track;    /* 0..1: cutoff follows the note */
    float env_oct;      /* filter envelope, octaves */
    float velocity_oct; /* velocity opens it */
    float pressure_oct; /* pressure opens it */
    float slide_oct;
    float lfo_oct;
    synth_adsr_t amp, mod;
    synth_lfo_cfg_t lfo;
    float vibrato_cents; /* LFO to pitch */
    float pitch_env_semis, pitch_env_ms; /* a dive at the start (hoover, kick-like basses) */
    uint8_t voice_mode;
    float glide_ms;
    float level;
    float velocity_level; /* 0..1: how much velocity sets the level */
    float pressure_level; /* 0..1: pressure adds this much level */
    float drift_cents;    /* slow random pitch wander, 0 = tight */
} synth_patch_t;

/* Per-note inputs, updated by the engine from MIDI. */
typedef struct {
    float note;      /* MIDI note + bend, semitones */
    float velocity;  /* 0..1 */
    float pressure;  /* 0..1 */
    float slide;     /* 0..1 (CC74) */
} synth_input_t;

typedef struct {
    const synth_patch_t *patch;
    uint32_t phase[SYNTH_UNISON_MAX];
    float ratio[SYNTH_UNISON_MAX];
    float gain_l[SYNTH_UNISON_MAX], gain_r[SYNTH_UNISON_MAX];
    synth_env_t amp, mod;
    synth_svf_t svf[2]; /* left, right (right only when the copies spread) */
    synth_ladder_t ladder[2];
    float lfo_phase, lfo_held, lfo_fade;
    uint32_t elapsed_steps;
    float pitch;      /* the note glided so far */
    float glide_coef;
    float pressure_s, slide_s; /* smoothed */
    float level;      /* last control step's output level */
    float wave_pos;   /* last control step's frame position */
    float drift, drift_target;
    uint32_t rng;
} synth_voice_t;

/* The engine's shared state the voices read. */
typedef struct {
    const tiles_wt_bank_t *banks[TILES_WT_BANK_COUNT];
    float beats_per_second;
} synth_context_t;

void synth_voice_start(synth_voice_t *v, const synth_patch_t *patch, const synth_input_t *in, uint32_t seed,
                       bool legato);
void synth_voice_release(synth_voice_t *v);
void synth_voice_kill(synth_voice_t *v);

static inline bool synth_voice_active(const synth_voice_t *v) {
    return v->amp.stage != SYNTH_ENV_IDLE;
}

/* The level of the amp envelope now (what the stealing order compares). */
static inline float synth_voice_level(const synth_voice_t *v) {
    return v->amp.value;
}

/* Adds n (a multiple of SYNTH_CTRL) frames into l and r. */
void synth_voice_render(synth_voice_t *v, const synth_context_t *ctx, const synth_input_t *in, float *l, float *r,
                        uint32_t n);

/* The factory patches (audio/synth_patches.c). */
extern synth_patch_t g_synth_patches[];
extern const uint32_t g_synth_patch_count;
/* Each track's sound at power-up: bass, lead, pads, bells. */
extern const uint8_t g_synth_track_default[4];
