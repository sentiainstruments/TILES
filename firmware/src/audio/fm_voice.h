#pragma once

/* The FM engine's voice: six operators that play DX7 voice data (the
 * synthesizer's 155 parameters, its 32 algorithms, envelopes, scaling,
 * LFO and pitch envelope), with TILES's pressure and slide brightening
 * the modulators. Our own code after the published DX7 knowledge
 * (docs/architecture/synth-engines.md, "FM"); tested on the computer
 * (test/test_fm.c). */

#include <stdbool.h>
#include <stdint.h>

/* A DX7 voice, unpacked: 21 bytes for each operator, operator 6 first
 * (rates 1-4, levels 1-4, break point, left/right depth, left/right
 * curve, rate scaling, amp mod sensitivity, velocity sensitivity, output
 * level, mode, coarse, fine, detune), then pitch envelope rates and
 * levels, algorithm, feedback, oscillator sync, LFO speed, delay, pitch
 * depth, amp depth, sync, wave, pitch sensitivity, transpose, the name
 * (10 characters). */
#define TILES_DX7_VOICE_BYTES 155u
#define TILES_DX7_PACKED_BYTES 128u
#define TILES_DX7_BANK_VOICES 32u
/* A 32-voice bank as DX7 sends it: F0 43 0n 09 20 00, the voices, a
 * checksum, F7. */
#define TILES_DX7_BANK_SYSEX_BYTES 4104u

enum {
    TILES_DX7_OP_BYTES = 21,
    TILES_DX7_PEG_RATE = 126,
    TILES_DX7_PEG_LEVEL = 130,
    TILES_DX7_ALGORITHM = 134,
    TILES_DX7_FEEDBACK = 135,
    TILES_DX7_OSC_SYNC = 136,
    TILES_DX7_LFO_SPEED = 137,
    TILES_DX7_LFO_DELAY = 138,
    TILES_DX7_LFO_PMD = 139,
    TILES_DX7_LFO_AMD = 140,
    TILES_DX7_LFO_SYNC = 141,
    TILES_DX7_LFO_WAVE = 142,
    TILES_DX7_PITCH_MOD_SENS = 143,
    TILES_DX7_TRANSPOSE = 144,
    TILES_DX7_NAME = 145,
};

/* Unpacks one 128-byte voice of a bank. */
void tiles_dx7_unpack(const uint8_t packed[TILES_DX7_PACKED_BYTES], uint8_t voice[TILES_DX7_VOICE_BYTES]);

/* A bank's 4,096 voice bytes from its sysex (NULL: not a DX7 32-voice
 * bank, or its checksum is wrong). */
const uint8_t *tiles_dx7_bank_voices(const uint8_t *sysex, uint32_t bytes);

/* A voice's name, 10 characters, trailing spaces dropped. */
void tiles_dx7_name(const uint8_t voice[TILES_DX7_VOICE_BYTES], char name[11]);

/* What TILES adds to a DX7 voice: pressure and slide (CC74) raise the
 * modulators (brightness), in dB at full. */
typedef struct {
    uint8_t dx7[TILES_DX7_VOICE_BYTES];
    float pressure_db, slide_db;
} tiles_fm_patch_t;

typedef struct {
    int32_t level, target, inc; /* log2, Q24 */
    uint8_t ix;                 /* stage 0-3; 4 = done */
    bool rising, down;
} tiles_fm_env_t;

typedef struct {
    uint32_t phase, inc;
    int32_t gain;      /* Q24 at the end of the last block */
    int32_t logfreq;   /* Q24 log2 Hz, before pitch modulation */
    int32_t outlevel;  /* output level with scaling and velocity, microsteps */
    int32_t rate_scaling;
    tiles_fm_env_t env;
    bool fixed;
} tiles_fm_op_t;

typedef struct {
    const tiles_fm_patch_t *patch;
    tiles_fm_op_t op[6]; /* operator 6 first, as the DX7 computes them */
    int32_t fb[2];       /* the feedback operator's last two outputs, Q24 */
    int32_t peg_level, peg_target, peg_inc; /* pitch envelope, Q24 octaves */
    uint8_t peg_ix;
    bool peg_rising, peg_down;
    uint32_t lfo_phase, lfo_inc, lfo_delay; /* delay: Q24 ramp in */
    uint32_t lfo_delay_inc;
    uint8_t lfo_held;
    float pitch;      /* the note glided so far */
    float glide_coef;
    float pressure_s, slide_s;
    float level;      /* last block's output level */
    bool sounding;
} tiles_fm_voice_t;

/* Everything a note needs: its pitch (note + bend), velocity 0..1,
 * pressure and slide 0..1. */
typedef struct {
    float note, velocity, pressure, slide;
} tiles_fm_input_t;

/* Fills the sine table (once, before any voice plays). */
void tiles_fm_init(void);

/* glide_ms > 0 with legato: glide from the last note. */
void tiles_fm_voice_start(tiles_fm_voice_t *v, const tiles_fm_patch_t *patch, const tiles_fm_input_t *in,
                          float glide_ms, bool legato);
void tiles_fm_voice_release(tiles_fm_voice_t *v);
/* Ends quickly (a voice given away): ~5 ms. */
void tiles_fm_voice_kill(tiles_fm_voice_t *v);
bool tiles_fm_voice_active(const tiles_fm_voice_t *v);
/* The loudest carrier's gain now, 0..~2 (what the stealing order compares). */
float tiles_fm_voice_level(const tiles_fm_voice_t *v);

/* Adds n (<= 16) frames into l and r at `gain` (the sound's level, its
 * velocity and pressure shares). */
void tiles_fm_voice_render(tiles_fm_voice_t *v, const tiles_fm_input_t *in, float gain, float *l, float *r,
                           uint32_t n);
