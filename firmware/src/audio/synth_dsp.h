#pragma once

/* The synth engines' building blocks: filters, envelopes, LFO, shapers.
 * Our own code after the published techniques (docs/architecture/
 * synth-engines.md, "Techniques"). Plain C, inline, float; coefficients
 * are worked out at control rate (every SYNTH_CTRL samples), never per
 * sample. */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define SYNTH_RATE 48000.0f
#define SYNTH_CTRL 8u /* samples per control step: 6 kHz */
#define SYNTH_PI 3.14159265358979f

/* fmaxf/fminf: one branch-free instruction each on the RP2350 (VMAXNM). */
static inline float synth_clamp(float x, float lo, float hi) {
    return __builtin_fminf(__builtin_fmaxf(x, lo), hi);
}

/* tan(pi * f), f in cycles per sample (clamped below Nyquist): Lambert's
 * continued fraction to x^7, within 1e-6 up to 15 kHz at 48 kHz. Control
 * rate only (one division). */
static inline float synth_tan_pi(float f) {
    float x = SYNTH_PI * synth_clamp(f, 1e-6f, 0.49f);
    float x2 = x * x;
    float num = x * (135135.0f + x2 * (-17325.0f + x2 * (378.0f - x2)));
    float den = 135135.0f + x2 * (-62370.0f + x2 * (3150.0f - 28.0f * x2));
    return num / den;
}

/* 2^x (clamped to +-126): the integer part into the exponent, 2^frac by its
 * series to the 6th power (0.03 cents at worst). Inline, so audio code in
 * RAM never calls into flash. */
static inline float synth_exp2(float x) {
    x = synth_clamp(x, -126.0f, 126.0f);
    float xi = floorf(x);
    float f = (x - xi) * 0.69314718f;
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.04166667f + f * (0.00833333f + f * 0.00138889f)))));
    union {
        float f;
        int32_t i;
    } u = {p};
    u.i += (int32_t)xi * (1 << 23);
    return u.f;
}

/* sin(2 pi t) for any t, by its series to x^7 after folding to a quarter
 * turn (1.6e-4 at worst): LFOs and the wavefolder. */
static inline float synth_sin_turns(float t) {
    t -= floorf(t + 0.5f);
    /* The fold to a quarter turn without branches (each costs this core a
     * stall): the same values, -0.5 - t being -(0.5 - |t|) exactly. */
    float a = __builtin_fabsf(t);
    t = __builtin_copysignf(__builtin_fminf(a, 0.5f - a), t);
    float x = t * (2.0f * SYNTH_PI), x2 = x * x;
    return x * (1.0f - x2 * (1.0f / 6.0f) * (1.0f - x2 * (1.0f / 20.0f) * (1.0f - x2 * (1.0f / 42.0f))));
}

/* Soft saturation without a division (per sample on the audio path): odd
 * 5th-order polynomial, slope 1 at 0, flat in slope and curve at +-15/8,
 * where it reaches +-1. */
#define SYNTH_SAT_CEILING 1.875f
static inline float synth_sat(float x) {
    x = synth_clamp(x, -SYNTH_SAT_CEILING, SYNTH_SAT_CEILING);
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.18962963f + x2 * 0.016181728f));
}

/* Division-free cubic soft clip for the audio path: odd, slope 1 at 0,
 * flat at +-1.5 -> +-1. */
static inline float synth_clip(float x) {
    x = synth_clamp(x, -1.5f, 1.5f);
    return x - (4.0f / 27.0f) * x * x * x;
}

/* Sine wavefolder: folds a driven signal back on itself (bass growls). */
static inline float synth_fold(float x, float amount) {
    return amount <= 0.0f ? x : (1.0f - amount) * x + amount * synth_sin_turns(x * (1.0f + 3.0f * amount) * 0.25f);
}

/* ---- State-variable filter, topology-preserving transform ----------------
 * Trapezoidal integration of the analog SVF (Simper; Zavalishin): exact
 * cutoff, stable to Nyquist, all outputs at once. */

typedef struct {
    float g, k, a1, a2, a3; /* coefficients */
    float ic[2];             /* integrator states */
} synth_svf_t;

enum { SYNTH_SVF_LP = 0, SYNTH_SVF_BP, SYNTH_SVF_HP };

/* res 0..1: Q from 0.5 to ~25. */
static inline void synth_svf_set(synth_svf_t *s, float hz, float res) {
    s->g = synth_tan_pi(hz * (1.0f / SYNTH_RATE));
    s->k = 2.0f - 1.96f * synth_clamp(res, 0.0f, 1.0f);
    s->a1 = 1.0f / (1.0f + s->g * (s->g + s->k));
    s->a2 = s->g * s->a1;
    s->a3 = s->g * s->a2;
}

/* One sample on loose coefficients and state (the voice keeps them in
 * registers for a whole step). */
static inline float synth_svf_step(float a1, float a2, float a3, float k, float *ic1, float *ic2, float x, int mode) {
    float v3 = x - *ic2;
    float v1 = a1 * *ic1 + a2 * v3;
    float v2 = *ic2 + a2 * *ic1 + a3 * v3;
    *ic1 = 2.0f * v1 - *ic1;
    *ic2 = 2.0f * v2 - *ic2;
    return mode == SYNTH_SVF_LP ? v2 : mode == SYNTH_SVF_BP ? v1 : x - k * v1 - v2;
}

static inline float synth_svf_tick(synth_svf_t *s, float x, int mode) {
    return synth_svf_step(s->a1, s->a2, s->a3, s->k, &s->ic[0], &s->ic[1], x, mode);
}

/* ---- 4-pole ladder, zero-delay feedback ------------------------------------
 * Four TPT one-poles in a resonant loop, the loop solved each sample
 * (Zavalishin, "The Art of VA Filter Design", ch. 5); the loop's input is
 * saturated after the linear solve, so resonance squelches and limits
 * instead of exploding. res 0..1, self-oscillation near 1. */

typedef struct {
    float G, beta, k, comp;
    float G4comp, inv; /* G^4 comp and the loop's 1 / (1 + k G^4) */
    float s[4];
} synth_ladder_t;

static inline void synth_ladder_set(synth_ladder_t *l, float hz, float res) {
    float g = synth_tan_pi(hz * (1.0f / SYNTH_RATE));
    l->beta = 1.0f / (1.0f + g);
    l->G = g * l->beta;
    l->k = 4.1f * synth_clamp(res, 0.0f, 1.0f);
    l->comp = 1.0f + 0.5f * l->k; /* passband makeup */
    float G2 = l->G * l->G, G4 = G2 * G2;
    l->G4comp = G4 * l->comp;
    l->inv = 1.0f / (1.0f + l->k * G4);
}

/* One sample on loose coefficients and state st[4]. */
static inline float synth_ladder_step(float G, float b, float k, float comp, float G4comp, float inv, float *st,
                                      float x) {
    float s0 = st[0], s1 = st[1], s2 = st[2], s3 = st[3];
    float S = b * (G * (G * (G * s0 + s1) + s2) + s3);
    float y4 = (G4comp * x + S) * inv;
    float u = synth_sat(x * comp - k * y4);
    float w = (u - s0) * G;
    float y = w + s0;
    st[0] = y + w;
    w = (y - s1) * G;
    u = w + s1;
    st[1] = u + w;
    w = (u - s2) * G;
    y = w + s2;
    st[2] = y + w;
    w = (y - s3) * G;
    u = w + s3;
    st[3] = u + w;
    return u;
}

static inline float synth_ladder_tick(synth_ladder_t *l, float x) {
    return synth_ladder_step(l->G, l->beta, l->k, l->comp, l->G4comp, l->inv, l->s, x);
}

/* ---- Envelope ---------------------------------------------------------------
 * ADSR with analog-style exponential segments, run at control rate: the
 * attack aims past 1 (an RC charging to a higher rail) so it is fast then
 * rounds off; decay and release fall exponentially, their times to -60 dB. */

typedef struct {
    float attack_ms, decay_ms, sustain, release_ms;
} synth_adsr_t;

enum { SYNTH_ENV_IDLE = 0, SYNTH_ENV_ATTACK, SYNTH_ENV_DECAY, SYNTH_ENV_RELEASE };

typedef struct {
    float value;
    float a_coef, d_coef, r_coef, sustain;
    uint8_t stage;
} synth_env_t;

#define SYNTH_ENV_ATTACK_TARGET 1.3f

static inline float synth_env_coef(float ms, float ln_ratio) {
    float steps = synth_clamp(ms, 0.3f, 60000.0f) * 0.001f * SYNTH_RATE / (float)SYNTH_CTRL;
    return 1.0f - expf(ln_ratio / steps);
}

static inline void synth_env_start(synth_env_t *e, const synth_adsr_t *p, bool retrigger) {
    /* Attack: from 0 to 1 on the way to 1.3 takes the attack time. */
    e->a_coef = synth_env_coef(p->attack_ms, logf(1.0f - 1.0f / SYNTH_ENV_ATTACK_TARGET));
    e->d_coef = synth_env_coef(p->decay_ms, -6.9078f);
    e->r_coef = synth_env_coef(p->release_ms, -6.9078f);
    e->sustain = synth_clamp(p->sustain, 0.0f, 1.0f);
    if (retrigger || e->stage == SYNTH_ENV_IDLE || e->stage == SYNTH_ENV_RELEASE) {
        e->stage = SYNTH_ENV_ATTACK;
    }
}

static inline void synth_env_release(synth_env_t *e) {
    if (e->stage != SYNTH_ENV_IDLE) {
        e->stage = SYNTH_ENV_RELEASE;
    }
}

/* Ends quickly (a voice given away): 5 ms to silence. */
static inline void synth_env_kill(synth_env_t *e) {
    e->r_coef = synth_env_coef(5.0f, -6.9078f);
    e->stage = SYNTH_ENV_RELEASE;
}

/* One control step; returns the new value. */
static inline float synth_env_step(synth_env_t *e) {
    switch (e->stage) {
    case SYNTH_ENV_ATTACK:
        e->value += (SYNTH_ENV_ATTACK_TARGET - e->value) * e->a_coef;
        if (e->value >= 1.0f) {
            e->value = 1.0f;
            e->stage = SYNTH_ENV_DECAY;
        }
        break;
    case SYNTH_ENV_DECAY:
        e->value += (e->sustain - e->value) * e->d_coef;
        break;
    case SYNTH_ENV_RELEASE:
        e->value -= e->value * e->r_coef;
        if (e->value < 0.0001f) {
            e->value = 0.0f;
            e->stage = SYNTH_ENV_IDLE;
        }
        break;
    default:
        break;
    }
    return e->value;
}

/* ---- LFO ------------------------------------------------------------------- */

enum { SYNTH_LFO_SINE = 0, SYNTH_LFO_TRIANGLE, SYNTH_LFO_SAW_DOWN, SYNTH_LFO_SQUARE, SYNTH_LFO_RANDOM };

/* Bipolar -1..1 at phase 0..1; RANDOM holds `held` (drawn at each wrap). */
static inline float synth_lfo_shape(int shape, float phase, float held) {
    switch (shape) {
    case SYNTH_LFO_TRIANGLE:
        return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
    case SYNTH_LFO_SAW_DOWN:
        return 1.0f - 2.0f * phase;
    case SYNTH_LFO_SQUARE:
        return phase < 0.5f ? 1.0f : -1.0f;
    case SYNTH_LFO_RANDOM:
        return held;
    default:
        return synth_sin_turns(phase);
    }
}

/* xorshift32: noise, unison phases, drift. */
static inline uint32_t synth_rand(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static inline float synth_rand_bipolar(uint32_t *state) {
    return (float)(int32_t)synth_rand(state) * (1.0f / 2147483648.0f);
}
