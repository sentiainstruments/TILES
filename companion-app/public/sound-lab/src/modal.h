#ifndef TILES_MODAL_H
#define TILES_MODAL_H

#include <stdint.h>

#define MODAL_COUNT 12
#define MODAL_CONTROL_INTERVAL 64
#define MODAL_SAMPLE_RATE 48000u

typedef struct {
    float material;   /* 0 wood, .5 glass, 1 metal */
    float decay;      /* 0..1, maps to a nominal T60 in seconds */
    float brightness; /* 0..1, relative excitation of upper modes */
    float spread;     /* 0..1, stereo distribution */
} ModalParams;

typedef struct {
    float pressure;   /* 0..1, opens and sustains the resonances */
    float slide;      /* 0..1, .5 neutral, morphs material */
    float bend;       /* semitones */
} ModalExpression;

typedef struct {
    float re, im, c, s, gain, pan_l, pan_r;
} ModalMode;

typedef struct {
    ModalMode modes[MODAL_COUNT];
    ModalParams params;
    float sample_rate, note, velocity, pressure, slide, bend;
    float excitation;
    uint32_t rng, control_count, age;
    int released;
} ModalVoice;

/* Call once per note. State is caller-owned; no allocation or platform I/O. */
void modal_start(ModalVoice *v, float sample_rate, float midi_note,
                 float velocity, ModalParams params, uint32_t seed);
void modal_release(ModalVoice *v);
/* Adds one sample to the caller's stereo accumulators. */
void modal_render(ModalVoice *v, ModalExpression expression, float *left, float *right);
float modal_ratio(unsigned mode, float material);
float modal_t60(float decay);

#endif
