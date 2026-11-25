#pragma once

/* Band-limited wavetables: each frame is stored once per octave, every
 * copy holding only the harmonics that stay below Nyquist for the notes
 * it plays (a mip-map), so the oscillator reads one copy with linear
 * interpolation and never aliases. Frames are built from spectra at load
 * by inverse FFT. 16-bit, in RAM. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_WT_FRAMES 8u
#define TILES_WT_LEVELS 8u /* 128, 64 ... 1 harmonics */
#define TILES_WT_TOP_HARMONICS 128u
/* Every level of one frame, guard points included (tiles_wt_level_bits()). */
#define TILES_WT_FRAME_SAMPLES 2376u

/* Interleaved: sample i of every frame side by side, so one pointer reaches
 * the two frames a voice crossfades and the next sample of each. */
typedef struct {
    int16_t data[TILES_WT_FRAME_SAMPLES * TILES_WT_FRAMES];
    float gain; /* int16 -> float, same for every frame */
} tiles_wt_bank_t;

static inline int16_t tiles_wt_sample(const tiles_wt_bank_t *bank, uint32_t frame, uint32_t i) {
    return bank->data[i * TILES_WT_FRAMES + frame];
}

/* A frame's spectrum: amplitude and phase (radians) of harmonics 1..n. */
typedef struct {
    float amp[TILES_WT_TOP_HARMONICS];
    float phase[TILES_WT_TOP_HARMONICS];
} tiles_wt_spectrum_t;

/* Fills `spectrum` for frame `frame` (0..TILES_WT_FRAMES-1). */
typedef void (*tiles_wt_design_fn)(void *ctx, uint32_t frame, tiles_wt_spectrum_t *spectrum);

#define TILES_WT_FFT_MAX 1024u
#define TILES_WT_SCRATCH_FLOATS (2u * TILES_WT_FFT_MAX)

/* Builds every frame and level; each frame is brought to the same loudness
 * (RMS), then the bank scaled to fit 16 bits. `spectrum` and `scratch`
 * (TILES_WT_SCRATCH_FLOATS) are working space. */
void tiles_wt_build(tiles_wt_bank_t *bank, tiles_wt_design_fn design, void *ctx, tiles_wt_spectrum_t *spectrum,
                    float *scratch);

/* The same build a step at a time (a frame's design, or one level of it;
 * twice over: the bank's peak is measured first), so it can run between
 * other work;
 * `spectrum` and `scratch` are the builder's until it is done. */
typedef struct {
    tiles_wt_bank_t *bank;
    tiles_wt_design_fn design;
    void *ctx;
    tiles_wt_spectrum_t *spectrum;
    float *scratch;
    float peak;
    float frame_gain[TILES_WT_FRAMES];
    uint8_t pass, frame, level; /* level 0: the design, then 1 + each level */
} tiles_wt_builder_t;

void tiles_wt_build_begin(tiles_wt_builder_t *b, tiles_wt_bank_t *bank, tiles_wt_design_fn design, void *ctx,
                          tiles_wt_spectrum_t *spectrum, float *scratch);
/* True once the bank is built. */
bool tiles_wt_build_step(tiles_wt_builder_t *b);

/* The spectrum of one cycle of a waveform drawn in time: TILES_WT_FFT_MAX
 * samples in `scratch`'s first half (the rest is working space). */
void tiles_wt_analyze(float *scratch, tiles_wt_spectrum_t *spectrum);

/* Each level's log2 length and offset inside a frame, and the highest
 * note frequency it may play (cycles per sample); in RAM, filled by the
 * first tiles_wt_build(). */
extern uint8_t g_wt_level_bits[TILES_WT_LEVELS];
extern uint16_t g_wt_level_offset[TILES_WT_LEVELS];
extern float g_wt_level_max_f[TILES_WT_LEVELS];

void tiles_wt_init_levels(void);

/* The copy for a note: f is the highest frequency it will play in the
 * block (cycles per sample). */
static inline uint32_t tiles_wt_level(float f) {
    uint32_t level = 0;
    while (level < TILES_WT_LEVELS - 1u && f > g_wt_level_max_f[level]) {
        level++;
    }
    return level;
}

static inline uint32_t tiles_wt_level_offset(uint32_t level) {
    return g_wt_level_offset[level];
}

static inline uint32_t tiles_wt_level_bits(uint32_t level) {
    return g_wt_level_bits[level];
}

/* The library (audio/wavetable_banks.c): a preset names its bank by this
 * number, so they never move. Only TILES_WT_SLOTS are in RAM at once
 * (audio_engine.c builds the others when a sound needs them). */
enum {
    TILES_WT_BANK_CLASSIC = 0, /* sine, triangle, saw, square, pulses: subtractive */
    TILES_WT_BANK_GLASS,       /* sine to glassy organ partials: bells, airy pads */
    TILES_WT_BANK_VOICE,       /* vowels and digital sync sweeps: choirs, growls */
    TILES_WT_BANK_TALK,        /* eight vowels set for bass notes, closed to open: talking basses */
    TILES_WT_BANK_GROWL,       /* phase modulation, gentle to harsh: growls, metallic leads */
    TILES_WT_BANK_FOLD,        /* a sine folded harder and harder: grit, hard techno */
    TILES_WT_BANK_BITS,        /* a sine at fewer steps and levels: digital, lo-fi */
    TILES_WT_BANK_SWEEP,       /* a saw through a resonant low-pass, closed to open */
    TILES_WT_BANK_ORGAN,       /* drawbar registrations, soft to full: keys, organ stabs */
    TILES_WT_BANK_NEURO,       /* fed-back FM, synced saws, folds, rings: neuro, tearout */
    TILES_WT_BANK_HARD,        /* saws and squares driven into clipping, rings: heavy techno */
    TILES_WT_BANK_COUNT,
};
#define TILES_WT_NAME_MAX 7u
void tiles_wt_design(uint32_t bank, uint32_t frame, tiles_wt_spectrum_t *spectrum, float *scratch);
const char *tiles_wt_bank_name(uint32_t bank);
