#include "dsp_bench.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

/* Kernels run from RAM on the board (the XIP cache is shared with core 0);
 * one flash-resident copy of the analog voice shows the difference. Never
 * inlined: GCC would otherwise fold a kernel into its flash-resident caller. */
#ifdef PICO_ON_DEVICE
#include "pico.h"
#define HOT(f) __no_inline_not_in_flash_func(f)
#else
#define HOT(f) __attribute__((noinline)) f
#endif

/* Helpers are forced inline so they run where their caller runs. */
#define INL static inline __attribute__((always_inline))

#define FS 48000.0f
#define BLOCK 32u
#define BLOCKS 300u /* 9600 samples, 0.2 s of audio per test */
#define PI_F 3.14159265f
#define HZ_TO_INC (4294967296.0f / FS)

/* ---- shared helpers ---- */

#define ARENA_BYTES (180u * 1024u)
static uint32_t s_arena[ARENA_BYTES / 4u];
static uint32_t s_arena_used;

static void *arena_alloc(uint32_t bytes) {
    bytes = (bytes + 7u) & ~7u;
    if (s_arena_used + bytes > ARENA_BYTES) {
        return NULL;
    }
    void *p = (uint8_t *)s_arena + s_arena_used;
    s_arena_used += bytes;
    memset(p, 0, bytes);
    return p;
}

static float s_sin[2049];
static uint32_t s_rng = 0x12345678u;
static volatile float s_sink;

INL float sin_lut(uint32_t phase) {
    uint32_t i = phase >> 21;
    float f = (float)(phase & 0x1FFFFFu) * (1.0f / 2097152.0f);
    return s_sin[i] + f * (s_sin[i + 1u] - s_sin[i]);
}

INL float noise(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static inline float rand01(void) {
    return noise(&s_rng) * 0.5f + 0.5f;
}

INL float soft_clip(float x) {
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Division-free clip: cubic soft knee, flat at +-1. */
INL float fast_clip(float x) {
    if (x > 1.5f) {
        return 1.0f;
    }
    if (x < -1.5f) {
        return -1.0f;
    }
    return x - 0.148148f * x * x * x;
}

/* tan(x) for 0 <= x < 1.5 (filter prewarp), Pade 5/4. */
INL float tan_pade(float x) {
    float x2 = x * x;
    return x * (945.0f - 105.0f * x2 + x2 * x2) / (945.0f - 420.0f * x2 + 15.0f * x2 * x2);
}

INL float polyblep(float t, float dt, float inv_dt) {
    if (t < dt) {
        t *= inv_dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) * inv_dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* ADSR as one-pole segments: attack aims past 1 so it lands in finite time. */
typedef struct {
    float v;
    uint8_t stage; /* 0 attack, 1 decay/sustain, 2 release */
    float a, d, r, sustain;
} env_t;

static void env_init(env_t *e, float attack_ms, float decay_ms, float sustain, float release_ms) {
    e->v = 0.0f;
    e->stage = 0;
    e->a = 1.0f - expf(-1.0f / (attack_ms * 0.001f * FS));
    e->d = 1.0f - expf(-1.0f / (decay_ms * 0.001f * FS));
    e->r = 1.0f - expf(-1.0f / (release_ms * 0.001f * FS));
    e->sustain = sustain;
}

INL float env_tick(env_t *e) {
    if (e->stage == 0u) {
        e->v += (1.2f - e->v) * e->a;
        if (e->v >= 1.0f) {
            e->v = 1.0f;
            e->stage = 1;
        }
    } else if (e->stage == 1u) {
        e->v += (e->sustain - e->v) * e->d;
    } else {
        e->v -= e->v * e->r;
    }
    return e->v;
}

/* Simper's state-variable filter (zero-delay feedback). */
typedef struct {
    float ic1, ic2, a1, a2, a3, k;
} svf_t;

static void svf_set(svf_t *f, float hz, float q) {
    float g = tan_pade(PI_F * hz / FS);
    f->k = 1.0f / q;
    f->a1 = 1.0f / (1.0f + g * (g + f->k));
    f->a2 = g * f->a1;
    f->a3 = g * f->a2;
}

/* Returns the low-pass; *bp and *hp the others when wanted. */
INL float svf_tick(svf_t *f, float x, float *bp, float *hp) {
    float v3 = x - f->ic2;
    float v1 = f->a1 * f->ic1 + f->a2 * v3;
    float v2 = f->ic2 + f->a2 * f->ic1 + f->a3 * v3;
    f->ic1 = 2.0f * v1 - f->ic1;
    f->ic2 = 2.0f * v2 - f->ic2;
    if (bp) {
        *bp = v1;
    }
    if (hp) {
        *hp = x - f->k * v1 - v2;
    }
    return v2;
}

static inline void pan_gains(float pan, float *gl, float *gr) {
    *gl = sqrtf(0.5f * (1.0f - pan));
    *gr = sqrtf(0.5f * (1.0f + pan));
}

static float note_hz(float note) {
    return 440.0f * exp2f((note - 69.0f) / 12.0f);
}

/* ---- wavetable voice: 8 frames morphed by pressure, SVF low-pass ---- */

#define WT_LEN 2048u
#define WT_FRAMES 8u
static int16_t *s_wt;

static bool wt_tables(void) {
    s_wt = arena_alloc(WT_FRAMES * WT_LEN * sizeof(int16_t));
    if (!s_wt) {
        return false;
    }
    for (uint32_t f = 0; f < WT_FRAMES; f++) {
        uint32_t harmonics = 1u + f * 4u;
        for (uint32_t i = 0; i < WT_LEN; i++) {
            float s = 0.0f;
            for (uint32_t h = 1; h <= harmonics; h++) {
                s += sin_lut((uint32_t)(i * h) << 21) / (float)h;
            }
            s_wt[f * WT_LEN + i] = (int16_t)(soft_clip(s * 0.6f) * 32000.0f);
        }
    }
    return true;
}

typedef struct {
    uint32_t phase, inc;
    float morph, morph_target;
    svf_t lp;
    env_t amp;
    float gl, gr;
} wt_voice_t;

static void wt_init(wt_voice_t *v, float note, float pan) {
    v->phase = 0;
    v->inc = (uint32_t)(note_hz(note) * HZ_TO_INC);
    v->morph = 0.0f;
    v->morph_target = 0.8f;
    memset(&v->lp, 0, sizeof(v->lp));
    svf_set(&v->lp, 4000.0f, 0.8f);
    env_init(&v->amp, 4.0f, 300.0f, 0.7f, 400.0f);
    pan_gains(pan, &v->gl, &v->gr);
}

static void HOT(wt_process)(wt_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        v->morph += (v->morph_target - v->morph) * 0.001f;
        float mf = v->morph * (float)(WT_FRAMES - 1u) * 0.999f;
        uint32_t f0 = (uint32_t)mf;
        float mm = mf - (float)f0;
        const int16_t *a = s_wt + f0 * WT_LEN;
        const int16_t *b = a + WT_LEN;
        uint32_t i0 = v->phase >> 21, i1 = (i0 + 1u) & (WT_LEN - 1u);
        float fr = (float)(v->phase & 0x1FFFFFu) * (1.0f / 2097152.0f);
        float sa = (float)a[i0] + fr * (float)(a[i1] - a[i0]);
        float sb = (float)b[i0] + fr * (float)(b[i1] - b[i0]);
        float s = (sa + mm * (sb - sa)) * (1.0f / 32768.0f);
        float y = svf_tick(&v->lp, s, NULL, NULL) * env_tick(&v->amp);
        l[i] += y * v->gl;
        r[i] += y * v->gr;
        v->phase += v->inc;
    }
}

/* ---- analog voice: 2 anti-aliased saws, ZDF 4-pole ladder, 2 envelopes, LFO ---- */

typedef struct {
    float p1, p2, dt1, dt2;
    float s[4];
    float cutoff, res;
    env_t amp, fenv;
    uint32_t lfo, lfo_inc;
    float gl, gr;
} va_voice_t;

static void va_init(va_voice_t *v, float note, float pan) {
    float hz = note_hz(note);
    v->p1 = 0.0f;
    v->p2 = 0.37f;
    v->dt1 = hz / FS;
    v->dt2 = hz * 1.006f / FS;
    memset(v->s, 0, sizeof(v->s));
    v->cutoff = 900.0f;
    v->res = 2.8f; /* of 4 = self-oscillation */
    env_init(&v->amp, 8.0f, 500.0f, 0.75f, 600.0f);
    env_init(&v->fenv, 2.0f, 350.0f, 0.3f, 500.0f);
    v->lfo = 0;
    v->lfo_inc = (uint32_t)(5.5f * HZ_TO_INC);
    pan_gains(pan, &v->gl, &v->gr);
}

INL void va_process_impl(va_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    float lfo_block = sin_lut(v->lfo);
    v->lfo += v->lfo_inc * n;
    float vib = 1.0f + 0.002f * lfo_block;
    float dt1 = v->dt1 * vib, dt2 = v->dt2 * vib;
    float inv1 = 1.0f / dt1, inv2 = 1.0f / dt2;
    for (uint32_t i = 0; i < n; i++) {
        float saw1 = 2.0f * v->p1 - 1.0f - polyblep(v->p1, dt1, inv1);
        float saw2 = 2.0f * v->p2 - 1.0f - polyblep(v->p2, dt2, inv2);
        v->p1 += dt1;
        if (v->p1 >= 1.0f) {
            v->p1 -= 1.0f;
        }
        v->p2 += dt2;
        if (v->p2 >= 1.0f) {
            v->p2 -= 1.0f;
        }
        float x = 0.5f * (saw1 + saw2);

        float hz = v->cutoff * (1.0f + 6.0f * env_tick(&v->fenv)) * (1.0f + 0.15f * lfo_block);
        if (hz > 18000.0f) {
            hz = 18000.0f;
        }
        float g = tan_pade(PI_F * hz / FS);
        float G = g / (1.0f + g);
        float inv1g = 1.0f / (1.0f + g);
        float S1 = v->s[0] * inv1g, S2 = v->s[1] * inv1g, S3 = v->s[2] * inv1g, S4 = v->s[3] * inv1g;
        float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        float sigma = G3 * S1 + G2 * S2 + G * S3 + S4;
        float y4 = (G4 * x + sigma) / (1.0f + v->res * G4);
        float u = soft_clip(x - v->res * y4); /* the ladder's input drive */
        float y = u;
        for (uint32_t k = 0; k < 4u; k++) {
            float vv = (y - v->s[k]) * G;
            float out = vv + v->s[k];
            v->s[k] = out + vv;
            y = out;
        }
        y *= env_tick(&v->amp) * (1.0f + 0.25f * v->res);
        l[i] += y * v->gl;
        r[i] += y * v->gr;
    }
}

/* The same voice with the ladder's coefficients worked out every 8 samples
 * (a 6 kHz control rate) and a division-free input clip. */
static void HOT(va_fast_process)(va_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    float lfo_block = sin_lut(v->lfo);
    v->lfo += v->lfo_inc * n;
    float vib = 1.0f + 0.002f * lfo_block;
    float dt1 = v->dt1 * vib, dt2 = v->dt2 * vib;
    float inv1 = 1.0f / dt1, inv2 = 1.0f / dt2;
    for (uint32_t j = 0; j < n; j += 8u) {
        float fe = 0.0f;
        for (uint32_t k = 0; k < 8u; k++) {
            fe = env_tick(&v->fenv);
        }
        float hz = v->cutoff * (1.0f + 6.0f * fe) * (1.0f + 0.15f * lfo_block);
        if (hz > 18000.0f) {
            hz = 18000.0f;
        }
        float g = tan_pade(PI_F * hz / FS);
        float G = g / (1.0f + g), inv1g = 1.0f / (1.0f + g);
        float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
        float inv_den = 1.0f / (1.0f + v->res * G4);
        for (uint32_t i = j; i < j + 8u && i < n; i++) {
            float saw1 = 2.0f * v->p1 - 1.0f - polyblep(v->p1, dt1, inv1);
            float saw2 = 2.0f * v->p2 - 1.0f - polyblep(v->p2, dt2, inv2);
            v->p1 += dt1;
            if (v->p1 >= 1.0f) {
                v->p1 -= 1.0f;
            }
            v->p2 += dt2;
            if (v->p2 >= 1.0f) {
                v->p2 -= 1.0f;
            }
            float x = 0.5f * (saw1 + saw2);
            float sigma = (G3 * v->s[0] + G2 * v->s[1] + G * v->s[2] + v->s[3]) * inv1g;
            float y4 = (G4 * x + sigma) * inv_den;
            float y = fast_clip(x - v->res * y4);
            for (uint32_t k = 0; k < 4u; k++) {
                float vv = (y - v->s[k]) * G;
                float out = vv + v->s[k];
                v->s[k] = out + vv;
                y = out;
            }
            y *= env_tick(&v->amp) * (1.0f + 0.25f * v->res);
            l[i] += y * v->gl;
            r[i] += y * v->gr;
        }
    }
}

static void HOT(va_process)(va_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    va_process_impl(v, l, r, n);
}

static void va_process_flash(va_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    va_process_impl(v, l, r, n);
}

/* ---- FM voice: 4 operators as two stacks (2->1, 4->3), op 4 feedback ---- */

typedef struct {
    uint32_t ph[4], inc[4];
    env_t env[4];
    float index[4];
    float fb1, fb2;
    float gl, gr;
} fm_voice_t;

static void fm_init(fm_voice_t *v, float note, float pan) {
    static const float ratio[4] = {1.0f, 14.0f, 1.0f, 1.0f};
    static const float decay[4] = {1800.0f, 260.0f, 1500.0f, 900.0f};
    float hz = note_hz(note);
    for (uint32_t i = 0; i < 4u; i++) {
        v->ph[i] = 0;
        v->inc[i] = (uint32_t)(hz * ratio[i] * (i == 3u ? 1.003f : 1.0f) * HZ_TO_INC);
        env_init(&v->env[i], 1.0f, decay[i], i == 1u ? 0.0f : 0.25f, 500.0f);
    }
    v->index[1] = 0.35f;
    v->index[3] = 0.6f;
    v->fb1 = v->fb2 = 0.0f;
    pan_gains(pan, &v->gl, &v->gr);
}

INL uint32_t pm(float mod, float index) {
    return (uint32_t)(int32_t)(mod * index * 2147483647.0f);
}

static void HOT(fm_process)(fm_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float e1 = env_tick(&v->env[0]), e2 = env_tick(&v->env[1]);
        float e3 = env_tick(&v->env[2]), e4 = env_tick(&v->env[3]);
        float m4 = sin_lut(v->ph[3] + pm(0.5f * (v->fb1 + v->fb2), 0.2f)) * e4;
        v->fb2 = v->fb1;
        v->fb1 = m4;
        float c3 = sin_lut(v->ph[2] + pm(m4, v->index[3])) * e3;
        float m2 = sin_lut(v->ph[1]) * e2;
        float c1 = sin_lut(v->ph[0] + pm(m2, v->index[1])) * e1;
        for (uint32_t k = 0; k < 4u; k++) {
            v->ph[k] += v->inc[k];
        }
        float y = 0.5f * (c1 + c3);
        l[i] += y * v->gl;
        r[i] += y * v->gr;
    }
}

/* ---- FM voice, 6 operators (DX7-style) ---- */

/* Operator 1..6: an electric-piano-like patch. Level is a modulator's
 * index or a carrier's mix level. */
static const float k_fm6_ratio[6] = {1.0f, 14.0f, 1.0f, 1.0f, 1.0f, 1.0f};
static const float k_fm6_decay[6] = {1800.0f, 260.0f, 1500.0f, 900.0f, 1200.0f, 700.0f};
static const float k_fm6_level[6] = {0.33f, 0.35f, 0.33f, 0.6f, 0.33f, 0.5f};

typedef struct {
    uint32_t ph[6], inc[6];
    env_t env[6];
    float level[6];
    float fb1, fb2;
    float gl, gr;
} fm6_voice_t;

static void fm6_init(fm6_voice_t *v, float note, float pan) {
    float hz = note_hz(note);
    for (uint32_t i = 0; i < 6u; i++) {
        v->ph[i] = 0;
        v->inc[i] = (uint32_t)(hz * k_fm6_ratio[i] * (i == 5u ? 1.003f : 1.0f) * HZ_TO_INC);
        env_init(&v->env[i], 1.0f, k_fm6_decay[i], i == 1u ? 0.0f : 0.25f, 500.0f);
        v->level[i] = k_fm6_level[i];
    }
    v->fb1 = v->fb2 = 0.0f;
    pan_gains(pan, &v->gl, &v->gr);
}

/* Algorithm 5 written out (6->5, 4->3, 2->1, feedback on 6), every
 * envelope ticked per sample: the 4-operator voice made bigger. */
static void HOT(fm6_process)(fm6_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float e[6];
        for (uint32_t k = 0; k < 6u; k++) {
            e[k] = env_tick(&v->env[k]) * v->level[k];
        }
        float m6 = sin_lut(v->ph[5] + pm(0.5f * (v->fb1 + v->fb2), 0.2f)) * e[5];
        v->fb2 = v->fb1;
        v->fb1 = m6;
        float c5 = sin_lut(v->ph[4] + pm(m6, 1.0f)) * e[4];
        float m4 = sin_lut(v->ph[3]) * e[3];
        float c3 = sin_lut(v->ph[2] + pm(m4, 1.0f)) * e[2];
        float m2 = sin_lut(v->ph[1]) * e[1];
        float c1 = sin_lut(v->ph[0] + pm(m2, 1.0f)) * e[0];
        for (uint32_t k = 0; k < 6u; k++) {
            v->ph[k] += v->inc[k];
        }
        float y = c1 + c3 + c5;
        l[i] += y * v->gl;
        r[i] += y * v->gr;
    }
}

/* The way a full 32-algorithm engine runs: any algorithm as a routing
 * table, one operator over the whole block at a time, envelopes once per
 * block (1.5 kHz) with the gain ramped across it. */
#define FM6_NO_IN 0xFFu
typedef struct {
    uint8_t in;  /* bus 0/1 it is modulated by, or FM6_NO_IN */
    uint8_t out; /* bus 0/1, or 2: the voice's output */
    uint8_t add; /* add to the bus instead of replacing it */
} fm6_route_t;

/* Processing order: operator 6 first, which carries the feedback. */
static const fm6_route_t k_fm6_alg1[6] = {/* 6->5->4->3, 2->1 */
                                          {FM6_NO_IN, 0, 0}, {0, 0, 0}, {0, 0, 0},
                                          {0, 2, 0},         {FM6_NO_IN, 0, 0}, {0, 2, 1}};
static const fm6_route_t k_fm6_alg5[6] = {/* 6->5, 4->3, 2->1 */
                                          {FM6_NO_IN, 0, 0}, {0, 2, 0}, {FM6_NO_IN, 0, 0},
                                          {0, 2, 1},         {FM6_NO_IN, 0, 0}, {0, 2, 1}};

typedef struct {
    fm6_route_t alg[6];     /* copied: the tables live in flash */
    uint32_t ph[6], inc[6]; /* processing order */
    env_t env[6];
    float level[6], gain[6];
    float fb1, fb2;
    float gl, gr;
} fm6b_voice_t;

/* The same envelope ticked once per BLOCK samples. */
static void env_to_block_rate(env_t *e) {
    e->a = 1.0f - powf(1.0f - e->a, (float)BLOCK);
    e->d = 1.0f - powf(1.0f - e->d, (float)BLOCK);
    e->r = 1.0f - powf(1.0f - e->r, (float)BLOCK);
}

static void fm6b_init(fm6b_voice_t *v, float note, float pan, const fm6_route_t *alg) {
    float hz = note_hz(note);
    memcpy(v->alg, alg, sizeof(v->alg));
    for (uint32_t k = 0; k < 6u; k++) {
        uint32_t op = 5u - k;
        v->ph[k] = 0;
        v->inc[k] = (uint32_t)(hz * k_fm6_ratio[op] * (op == 5u ? 1.003f : 1.0f) * HZ_TO_INC);
        env_init(&v->env[k], 1.0f, k_fm6_decay[op], op == 1u ? 0.0f : 0.25f, 500.0f);
        env_to_block_rate(&v->env[k]);
        v->level[k] = k_fm6_level[op];
        v->gain[k] = 0.0f;
    }
    v->fb1 = v->fb2 = 0.0f;
    pan_gains(pan, &v->gl, &v->gr);
}

static float s_fm6_bus[3][BLOCK];

/* n <= BLOCK. */
static void HOT(fm6b_process)(fm6b_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    float inv_n = 1.0f / (float)n;
    for (uint32_t k = 0; k < 6u; k++) {
        fm6_route_t rt = v->alg[k];
        float g = v->gain[k];
        float g1 = env_tick(&v->env[k]) * v->level[k];
        float dg = (g1 - g) * inv_n;
        v->gain[k] = g1;
        uint32_t ph = v->ph[k], inc = v->inc[k];
        float *dst = s_fm6_bus[rt.out];
        if (k == 0u) {
            float f1 = v->fb1, f2 = v->fb2;
            for (uint32_t i = 0; i < n; i++) {
                float y = sin_lut(ph + pm(0.5f * (f1 + f2), 0.2f)) * g;
                f2 = f1;
                f1 = y;
                dst[i] = y;
                ph += inc;
                g += dg;
            }
            v->fb1 = f1;
            v->fb2 = f2;
        } else if (rt.in == FM6_NO_IN) {
            float keep = rt.add ? 1.0f : 0.0f;
            for (uint32_t i = 0; i < n; i++) {
                dst[i] = keep * dst[i] + sin_lut(ph) * g;
                ph += inc;
                g += dg;
            }
        } else {
            const float *src = s_fm6_bus[rt.in];
            float keep = rt.add ? 1.0f : 0.0f;
            for (uint32_t i = 0; i < n; i++) {
                dst[i] = keep * dst[i] + sin_lut(ph + pm(src[i], 1.0f)) * g;
                ph += inc;
                g += dg;
            }
        }
        v->ph[k] = ph;
    }
    const float *out = s_fm6_bus[2];
    for (uint32_t i = 0; i < n; i++) {
        l[i] += out[i] * v->gl;
        r[i] += out[i] * v->gr;
    }
}

/* ---- plucked string: Karplus-Strong with allpass tuning, loop filter, DC block ---- */

#define PLUCK_LEN 1024u

typedef struct {
    float *buf;
    uint32_t w, di;
    float ap_c, ap_x1, ap_y1;
    float lp, damp, decay;
    float dc_x1, dc_y1;
    float gl, gr;
    uint32_t rng;
} pluck_voice_t;

static bool pluck_init(pluck_voice_t *v, float note, float pan) {
    v->buf = arena_alloc(PLUCK_LEN * sizeof(float));
    if (!v->buf) {
        return false;
    }
    float period = FS / note_hz(note);
    v->di = (uint32_t)period;
    float frac = period - (float)v->di;
    if (frac < 0.1f) {
        frac += 1.0f;
        v->di--;
    }
    v->ap_c = (1.0f - frac) / (1.0f + frac);
    v->ap_x1 = v->ap_y1 = 0.0f;
    v->lp = 0.0f;
    v->damp = 0.5f;
    v->decay = 0.996f;
    v->dc_x1 = v->dc_y1 = 0.0f;
    v->rng = 0x9E3779B9u ^ (uint32_t)note;
    for (uint32_t i = 0; i < PLUCK_LEN; i++) {
        v->buf[i] = noise(&v->rng) * 0.6f;
    }
    v->w = 0;
    pan_gains(pan, &v->gl, &v->gr);
    return true;
}

static void HOT(pluck_process)(pluck_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float x = v->buf[(v->w - v->di) & (PLUCK_LEN - 1u)];
        float y = v->ap_c * x + v->ap_x1 - v->ap_c * v->ap_y1;
        v->ap_x1 = x;
        v->ap_y1 = y;
        v->lp += v->damp * (y - v->lp);
        float s = v->lp * v->decay;
        v->buf[v->w] = s;
        v->w = (v->w + 1u) & (PLUCK_LEN - 1u);
        float out = s - v->dc_x1 + 0.995f * v->dc_y1;
        v->dc_x1 = s;
        v->dc_y1 = out;
        l[i] += out * v->gl;
        r[i] += out * v->gr;
    }
}

/* ---- granular: grains read from a recorded buffer through a Hann window ---- */

#define GRAIN_SRC_LEN 12000u
static int16_t *s_grain_src;

typedef struct {
    float pos, rate, wph, winc;
    float gl, gr;
} grain_t;

static bool grain_source(void) {
    s_grain_src = arena_alloc(GRAIN_SRC_LEN * sizeof(int16_t));
    if (!s_grain_src) {
        return false;
    }
    uint32_t rng = 0xC0FFEEu;
    float lp = 0.0f;
    for (uint32_t i = 0; i < GRAIN_SRC_LEN; i++) {
        lp += 0.05f * (noise(&rng) - lp);
        float tone = sin_lut((uint32_t)(i * 220.0f * HZ_TO_INC)) + 0.5f * sin_lut((uint32_t)(i * 331.0f * HZ_TO_INC));
        s_grain_src[i] = (int16_t)(soft_clip(0.6f * tone + 2.0f * lp) * 30000.0f);
    }
    return true;
}

static void grain_spawn(grain_t *g) {
    g->pos = rand01() * (float)(GRAIN_SRC_LEN - 4000u);
    g->rate = 0.5f + rand01() * 1.5f;
    g->wph = 0.0f;
    g->winc = 1.0f / ((0.03f + rand01() * 0.09f) * FS);
    pan_gains(rand01() * 2.0f - 1.0f, &g->gl, &g->gr);
}

static void HOT(grain_process)(grain_t *g, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ip = (uint32_t)g->pos;
        float fr = g->pos - (float)ip;
        float s = ((float)s_grain_src[ip] + fr * (float)(s_grain_src[ip + 1u] - s_grain_src[ip])) * (1.0f / 32768.0f);
        float w = sin_lut((uint32_t)(g->wph * 2147483648.0f));
        w *= w;
        float y = s * w;
        l[i] += y * g->gl;
        r[i] += y * g->gr;
        g->pos += g->rate;
        g->wph += g->winc;
        if (g->wph >= 1.0f || g->pos >= (float)(GRAIN_SRC_LEN - 2u)) {
            grain_spawn(g);
        }
    }
}

/* ---- drums (synthesized, retriggered so one is always sounding) ---- */

typedef struct {
    uint32_t phase, rng;
    float penv, pcoef, aenv, acoef, click, ccoef, hp_x1, hp_y1;
} kick_t;

static void kick_trigger(kick_t *k) {
    k->penv = 1.0f;
    k->aenv = 1.0f;
    k->click = 1.0f;
}

static void kick_init(kick_t *k) {
    memset(k, 0, sizeof(*k));
    k->rng = 0xBEEFu;
    k->pcoef = expf(-1.0f / (0.03f * FS));
    k->acoef = expf(-1.0f / (0.35f * FS));
    k->ccoef = expf(-1.0f / (0.002f * FS));
    kick_trigger(k);
}

/* The same kick with its state held in locals for the block. */
static void HOT(kick_local_process)(kick_t *k, float *restrict l, float *restrict r, uint32_t n) {
    uint32_t phase = k->phase, rng = k->rng;
    float penv = k->penv, aenv = k->aenv, click = k->click, hx = k->hp_x1, hy = k->hp_y1;
    const float pcoef = k->pcoef, acoef = k->acoef, ccoef = k->ccoef;
    for (uint32_t i = 0; i < n; i++) {
        float hz = 45.0f + 180.0f * penv;
        penv *= pcoef;
        phase += (uint32_t)(hz * HZ_TO_INC);
        float body = sin_lut(phase) * aenv;
        aenv *= acoef;
        float c = noise(&rng) * click;
        click *= ccoef;
        float hp = 0.95f * (hy + c - hx);
        hx = c;
        hy = hp;
        float y = fast_clip(1.6f * (body + 0.25f * hp));
        l[i] += y;
        r[i] += y;
        if (aenv < 0.0005f) {
            penv = aenv = click = 1.0f;
        }
    }
    k->phase = phase;
    k->rng = rng;
    k->penv = penv;
    k->aenv = aenv;
    k->click = click;
    k->hp_x1 = hx;
    k->hp_y1 = hy;
}

static void HOT(kick_process)(kick_t *k, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float hz = 45.0f + 180.0f * k->penv;
        k->penv *= k->pcoef;
        k->phase += (uint32_t)(hz * HZ_TO_INC);
        float body = sin_lut(k->phase) * k->aenv;
        k->aenv *= k->acoef;
        float c = noise(&k->rng) * k->click;
        k->click *= k->ccoef;
        float hp = 0.95f * (k->hp_y1 + c - k->hp_x1);
        k->hp_x1 = c;
        k->hp_y1 = hp;
        float y = soft_clip(1.6f * (body + 0.25f * hp));
        l[i] += y;
        r[i] += y;
        if (k->aenv < 0.0005f) {
            kick_trigger(k);
        }
    }
}

typedef struct {
    uint32_t ph1, ph2, inc1, inc2, rng;
    float tenv, tcoef, nenv, ncoef;
    svf_t bp;
} snare_t;

static void snare_trigger(snare_t *s) {
    s->tenv = 1.0f;
    s->nenv = 1.0f;
}

static void snare_init(snare_t *s) {
    memset(s, 0, sizeof(*s));
    s->rng = 0xFACEu;
    s->inc1 = (uint32_t)(185.0f * HZ_TO_INC);
    s->inc2 = (uint32_t)(330.0f * HZ_TO_INC);
    s->tcoef = expf(-1.0f / (0.08f * FS));
    s->ncoef = expf(-1.0f / (0.18f * FS));
    svf_set(&s->bp, 2500.0f, 0.7f);
    snare_trigger(s);
}

static void HOT(snare_process)(snare_t *s, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float tone = (sin_lut(s->ph1) + 0.6f * sin_lut(s->ph2)) * s->tenv;
        s->ph1 += s->inc1;
        s->ph2 += s->inc2;
        s->tenv *= s->tcoef;
        float bp, hp;
        (void)svf_tick(&s->bp, noise(&s->rng), &bp, &hp);
        float y = 0.5f * tone + (0.7f * bp + 0.4f * hp) * s->nenv;
        s->nenv *= s->ncoef;
        l[i] += y;
        r[i] += y;
        if (s->nenv < 0.0005f) {
            snare_trigger(s);
        }
    }
}

/* Metallic hat: six anti-aliased squares, band-pass then high-pass. */
typedef struct {
    float p[6], dt[6], inv[6];
    float env, coef;
    svf_t bp, hp;
} hat_t;

static void hat_init(hat_t *h) {
    static const float hz[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
    memset(h, 0, sizeof(*h));
    for (uint32_t i = 0; i < 6u; i++) {
        h->dt[i] = hz[i] / FS;
        h->inv[i] = 1.0f / h->dt[i];
        h->p[i] = (float)i * 0.13f;
    }
    h->coef = expf(-1.0f / (0.06f * FS));
    h->env = 1.0f;
    svf_set(&h->bp, 10000.0f, 1.2f);
    svf_set(&h->hp, 7000.0f, 0.7f);
}

static void HOT(hat_process)(hat_t *h, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float sum = 0.0f;
        for (uint32_t k = 0; k < 6u; k++) {
            float t = h->p[k];
            float t2 = t + 0.5f >= 1.0f ? t - 0.5f : t + 0.5f;
            float sq = (t < 0.5f ? 1.0f : -1.0f) + polyblep(t, h->dt[k], h->inv[k]) - polyblep(t2, h->dt[k], h->inv[k]);
            sum += sq;
            t += h->dt[k];
            h->p[k] = t >= 1.0f ? t - 1.0f : t;
        }
        float bp;
        (void)svf_tick(&h->bp, sum * 0.15f, &bp, NULL);
        float hp;
        (void)svf_tick(&h->hp, bp, NULL, &hp);
        float y = hp * h->env;
        h->env *= h->coef;
        if (h->env < 0.0005f) {
            h->env = 1.0f;
        }
        l[i] += y * 0.8f;
        r[i] += y;
    }
}

/* Cheaper hat: white noise through band-pass and high-pass. */
typedef struct {
    uint32_t rng;
    float env, coef;
    svf_t bp, hp;
} nhat_t;

static void nhat_init(nhat_t *h) {
    memset(h, 0, sizeof(*h));
    h->rng = 0xD00Du;
    h->coef = expf(-1.0f / (0.06f * FS));
    h->env = 1.0f;
    svf_set(&h->bp, 10000.0f, 1.2f);
    svf_set(&h->hp, 7000.0f, 0.7f);
}

static void HOT(nhat_process)(nhat_t *h, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        float bp, hp;
        (void)svf_tick(&h->bp, noise(&h->rng), &bp, NULL);
        (void)svf_tick(&h->hp, bp, NULL, &hp);
        float y = hp * h->env;
        h->env *= h->coef;
        if (h->env < 0.0005f) {
            h->env = 1.0f;
        }
        l[i] += y * 0.8f;
        r[i] += y;
    }
}

/* Clap: band-passed noise in three quick bursts and a tail. */
typedef struct {
    uint32_t rng, t;
    float env;
    svf_t bp;
} clap_t;

static void clap_init(clap_t *c) {
    memset(c, 0, sizeof(*c));
    c->rng = 0xCAFEu;
    svf_set(&c->bp, 1200.0f, 2.0f);
}

static void HOT(clap_process)(clap_t *c, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = c->t++;
        if (t == 0u || t == 480u || t == 960u || t == 1440u) {
            c->env = 1.0f;
        }
        c->env *= t < 1440u ? 0.9975f : 0.99975f;
        float bp;
        (void)svf_tick(&c->bp, noise(&c->rng), &bp, NULL);
        float y = bp * c->env * 1.5f;
        l[i] += y;
        r[i] += y;
        if (t > 24000u) {
            c->t = 0;
        }
    }
}

/* ---- sample voice: 16-bit mono read straight from flash, linear
 * interpolation for tuning ---- */

#define SMP_VOICES 8u             /* the 8 drum sample slots */
#define SMP_REGION_SAMPLES 12288u /* 24 KB each, more than one test reads */
#define SMP_LOOP_SAMPLES (SMP_REGION_SAMPLES - 4u)

#ifdef PICO_ON_DEVICE
#include "hardware/regs/addressmap.h"
/* Any flash past the image: only the read cost matters, not the sound. */
#define SMP_FLASH_OFFSET (1024u * 1024u)
static const uint32_t *smp_region(uint32_t voice, bool cached) {
    uint32_t base = cached ? XIP_BASE : XIP_NOCACHE_NOALLOC_BASE;
    return (const uint32_t *)(base + SMP_FLASH_OFFSET + voice * SMP_REGION_SAMPLES * 2u);
}
#else
static uint32_t s_host_flash[SMP_VOICES * SMP_REGION_SAMPLES / 2u];
static const uint32_t *smp_region(uint32_t voice, bool cached) {
    (void)cached;
    if (s_host_flash[0] == 0u) {
        uint32_t rng = 0xD00Du;
        for (uint32_t i = 0; i < SMP_VOICES * SMP_REGION_SAMPLES / 2u; i++) {
            s_host_flash[i] = (uint32_t)(int32_t)(noise(&rng) * 2147483647.0f) & 0x3FFF3FFFu;
        }
    }
    return s_host_flash + voice * SMP_REGION_SAMPLES / 2u;
}
#endif

typedef struct {
    const uint32_t *src; /* int16 pairs */
    uint32_t loop;       /* samples it plays before starting over */
    uint32_t pos, step;  /* 16.16, in samples */
    uint32_t w, a, b;    /* the pair word under pos and the next one */
    float gl, gr;
} smp_voice_t;

static const float k_smp_ratio[SMP_VOICES] = {1.0f, 1.0f, 1.0f, 1.0f, 1.06f, 0.94f, 1.0f, 1.12f};

static void smp_init(smp_voice_t *v, const uint32_t *src, uint32_t slot, uint32_t loop) {
    v->src = src;
    v->loop = loop;
    v->pos = 0;
    v->step = (uint32_t)(k_smp_ratio[slot] * 65536.0f);
    v->w = 0;
    v->a = src[0];
    v->b = src[1];
    pan_gains((float)slot / 4.0f - 1.0f, &v->gl, &v->gr);
}

static void HOT(smp_process)(smp_voice_t *v, float *restrict l, float *restrict r, uint32_t n) {
    const uint32_t *src = v->src;
    uint32_t loop = v->loop;
    uint32_t pos = v->pos, step = v->step, w = v->w, a = v->a, b = v->b;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = pos >> 16, wi = idx >> 1;
        if (wi != w) {
            a = wi == w + 1u ? b : src[wi];
            b = src[wi + 1u];
            w = wi;
        }
        int32_t s0, s1;
        if (idx & 1u) {
            s0 = (int16_t)(a >> 16);
            s1 = (int16_t)b;
        } else {
            s0 = (int16_t)a;
            s1 = (int16_t)(a >> 16);
        }
        float fr = (float)(pos & 0xFFFFu) * (1.0f / 65536.0f);
        float y = ((float)s0 + fr * (float)(s1 - s0)) * (1.0f / 32768.0f);
        l[i] += y * v->gl;
        r[i] += y * v->gr;
        pos += step;
        if ((pos >> 16) >= loop) {
            pos -= loop << 16;
        }
    }
    v->pos = pos;
    v->w = w;
    v->a = a;
    v->b = b;
}

/* ---- effects ---- */

/* Plate reverb (Dattorro's topology), run at 24 kHz; lengths scaled from
 * the original 29.761 kHz. */
#define RV_SCALE (24000.0f / 29761.0f)
typedef struct {
    float *buf;
    uint32_t len, w;
} line_t;

static bool line_init(line_t *d, uint32_t len) {
    d->len = len;
    d->w = 0;
    d->buf = arena_alloc(len * sizeof(float));
    return d->buf != NULL;
}

INL float line_tap(const line_t *d, uint32_t back) {
    uint32_t i = d->w + d->len - back;
    return d->buf[i >= d->len ? i - d->len : i];
}

INL void line_push(line_t *d, float x) {
    d->buf[d->w] = x;
    d->w = d->w + 1u == d->len ? 0u : d->w + 1u;
}

INL float allpass(line_t *d, float x, float g) {
    float delayed = line_tap(d, d->len);
    float v = x + g * delayed;
    line_push(d, v);
    return delayed - g * v;
}

/* A modulated allpass: the read point moves with an LFO (linear interpolation). */
INL float allpass_mod(line_t *d, float x, float g, float back) {
    uint32_t bi = (uint32_t)back;
    float fr = back - (float)bi;
    float a = line_tap(d, bi), b = line_tap(d, bi + 1u);
    float delayed = a + fr * (b - a);
    float v = x - g * delayed;
    line_push(d, v);
    return delayed + g * v;
}

typedef struct {
    line_t in[4], ap_l, dl_l, ap2_l, d2_l, ap_r, dl_r, ap2_r, d2_r;
    uint32_t tap[14];
    float bw, damp_l, damp_r, decay;
    uint32_t lfo;
    float last_l, last_r, prev_l, prev_r, hold;
    bool odd;
} reverb_t;

static uint32_t rv(float n) {
    return (uint32_t)(n * RV_SCALE);
}

static bool reverb_init(reverb_t *v) {
    memset(v, 0, sizeof(*v));
    static const float in_len[4] = {142.0f, 107.0f, 379.0f, 277.0f};
    for (uint32_t i = 0; i < 4u; i++) {
        if (!line_init(&v->in[i], rv(in_len[i]))) {
            return false;
        }
    }
    static const float taps[14] = {266, 2974, 1913, 1996, 1990, 187, 1066, 353, 3627, 1228, 2673, 2111, 335, 121};
    for (uint32_t i = 0; i < 14u; i++) {
        v->tap[i] = rv(taps[i]);
    }
    return line_init(&v->ap_l, rv(672.0f) + 20u) && line_init(&v->dl_l, rv(4453.0f)) &&
           line_init(&v->ap2_l, rv(1800.0f)) && line_init(&v->d2_l, rv(3720.0f)) &&
           line_init(&v->ap_r, rv(908.0f) + 20u) && line_init(&v->dl_r, rv(4217.0f)) &&
           line_init(&v->ap2_r, rv(2656.0f)) && line_init(&v->d2_r, rv(3163.0f));
}

static void HOT(reverb_tick)(reverb_t *v, float in, float *out_l, float *out_r) {
    v->bw += 0.7f * (in - v->bw);
    float x = v->bw;
    x = allpass(&v->in[0], x, 0.75f);
    x = allpass(&v->in[1], x, 0.75f);
    x = allpass(&v->in[2], x, 0.625f);
    x = allpass(&v->in[3], x, 0.625f);
    v->lfo += (uint32_t)(1.0f * 4294967296.0f / 24000.0f);
    float m = sin_lut(v->lfo) * 12.0f;
    float decay = 0.7f;

    float l = x + line_tap(&v->d2_r, v->d2_r.len) * decay;
    l = allpass_mod(&v->ap_l, l, 0.7f, (float)(v->ap_l.len - 20u) + m);
    line_push(&v->dl_l, l);
    l = line_tap(&v->dl_l, v->dl_l.len);
    v->damp_l += 0.4f * (l - v->damp_l);
    l = allpass(&v->ap2_l, v->damp_l * decay, 0.5f);
    line_push(&v->d2_l, l);

    float r = x + line_tap(&v->d2_l, v->d2_l.len) * decay;
    r = allpass_mod(&v->ap_r, r, 0.7f, (float)(v->ap_r.len - 20u) - m);
    line_push(&v->dl_r, r);
    r = line_tap(&v->dl_r, v->dl_r.len);
    v->damp_r += 0.4f * (r - v->damp_r);
    r = allpass(&v->ap2_r, v->damp_r * decay, 0.5f);
    line_push(&v->d2_r, r);

    const uint32_t *t = v->tap;
    *out_l = 0.6f * (line_tap(&v->dl_r, t[0]) + line_tap(&v->dl_r, t[1]) - line_tap(&v->ap2_r, t[2]) +
                     line_tap(&v->d2_r, t[3]) - line_tap(&v->dl_l, t[4]) - line_tap(&v->ap2_l, t[5]) -
                     line_tap(&v->d2_l, t[6]));
    *out_r = 0.6f * (line_tap(&v->dl_l, t[7]) + line_tap(&v->dl_l, t[8]) - line_tap(&v->ap2_l, t[9]) +
                     line_tap(&v->d2_l, t[10]) - line_tap(&v->dl_r, t[11]) - line_tap(&v->ap2_r, t[12]) -
                     line_tap(&v->d2_r, t[13]));
}

/* Adds the wet signal for n 48 kHz frames, running the tank every other frame. */
static void HOT(reverb_process)(reverb_t *v, const float *restrict in_l, const float *restrict in_r, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!v->odd) {
            v->hold = 0.5f * (in_l[i] + in_r[i]);
        } else {
            float wl, wr;
            reverb_tick(v, 0.5f * (v->hold + 0.5f * (in_l[i] + in_r[i])), &wl, &wr);
            v->prev_l = v->last_l;
            v->prev_r = v->last_r;
            v->last_l = wl;
            v->last_r = wr;
        }
        /* linear interpolation back to 48 kHz */
        float t = v->odd ? 1.0f : 0.5f;
        l[i] += v->prev_l + t * (v->last_l - v->prev_l);
        r[i] += v->prev_r + t * (v->last_r - v->prev_r);
        v->odd = !v->odd;
    }
}

/* Ping-pong delay at 24 kHz with 16-bit storage, feedback through a tone filter. */
#define DELAY_LEN 12000u /* 0.5 s here; the plan's 2 s costs the same per sample */
typedef struct {
    int16_t *bl, *br;
    uint32_t w;
    float time, fb, tone_l, tone_r;
    float last_l, last_r, prev_l, prev_r, hold_l, hold_r;
    bool odd;
} delay_t;

static bool delay_init(delay_t *d) {
    memset(d, 0, sizeof(*d));
    d->bl = arena_alloc(DELAY_LEN * sizeof(int16_t));
    d->br = arena_alloc(DELAY_LEN * sizeof(int16_t));
    d->time = 3000.5f; /* 125 ms, so repeats land inside a 0.2 s test; the cost is the same at any length */
    d->fb = 0.55f;
    return d->bl && d->br;
}

INL float delay_read(const int16_t *b, uint32_t w, float back) {
    uint32_t bi = (uint32_t)back;
    float fr = back - (float)bi;
    uint32_t i0 = w >= bi ? w - bi : w + DELAY_LEN - bi;
    uint32_t i1 = i0 == 0u ? DELAY_LEN - 1u : i0 - 1u;
    return ((float)b[i0] + fr * (float)(b[i1] - b[i0])) * (1.0f / 32768.0f);
}

static void HOT(delay_process)(delay_t *d, const float *restrict in_l, const float *restrict in_r, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!d->odd) {
            d->hold_l = in_l[i];
            d->hold_r = in_r[i];
        } else {
            float xl = 0.5f * (d->hold_l + in_l[i]), xr = 0.5f * (d->hold_r + in_r[i]);
            float rl = delay_read(d->bl, d->w, d->time), rr = delay_read(d->br, d->w, d->time);
            d->tone_l += 0.45f * (rl - d->tone_l);
            d->tone_r += 0.45f * (rr - d->tone_r);
            float wl = fast_clip(xl + d->fb * d->tone_r); /* ping-pong: each side feeds the other */
            float wr = fast_clip(xr + d->fb * d->tone_l);
            d->bl[d->w] = (int16_t)(wl * 32000.0f);
            d->br[d->w] = (int16_t)(wr * 32000.0f);
            d->w = d->w + 1u == DELAY_LEN ? 0u : d->w + 1u;
            d->prev_l = d->last_l;
            d->prev_r = d->last_r;
            d->last_l = rl;
            d->last_r = rr;
        }
        float t = d->odd ? 1.0f : 0.5f;
        l[i] += d->prev_l + t * (d->last_l - d->prev_l);
        r[i] += d->prev_r + t * (d->last_r - d->prev_r);
        d->odd = !d->odd;
    }
}

/* Stereo chorus: two modulated taps per side. */
#define CHORUS_LEN 1024u
typedef struct {
    float *bl, *br;
    uint32_t w, lfo;
} chorus_t;

static bool chorus_init(chorus_t *c) {
    memset(c, 0, sizeof(*c));
    c->bl = arena_alloc(CHORUS_LEN * sizeof(float));
    c->br = arena_alloc(CHORUS_LEN * sizeof(float));
    return c->bl && c->br;
}

INL float chorus_tap(const float *b, uint32_t w, float back) {
    uint32_t bi = (uint32_t)back;
    float fr = back - (float)bi;
    float a = b[(w - bi) & (CHORUS_LEN - 1u)], c = b[(w - bi - 1u) & (CHORUS_LEN - 1u)];
    return a + fr * (c - a);
}

static void HOT(chorus_process)(chorus_t *c, const float *restrict in_l, const float *restrict in_r, float *restrict l, float *restrict r, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        c->bl[c->w] = in_l[i];
        c->br[c->w] = in_r[i];
        c->lfo += (uint32_t)(0.7f * HZ_TO_INC);
        float m1 = sin_lut(c->lfo), m2 = sin_lut(c->lfo + 0x40000000u);
        float wl = chorus_tap(c->bl, c->w, 380.0f + 150.0f * m1) + chorus_tap(c->bl, c->w, 520.0f + 150.0f * m2);
        float wr = chorus_tap(c->br, c->w, 380.0f - 150.0f * m1) + chorus_tap(c->br, c->w, 520.0f - 150.0f * m2);
        c->w = (c->w + 1u) & (CHORUS_LEN - 1u);
        l[i] += 0.35f * wl;
        r[i] += 0.35f * wr;
    }
}

/* Master: sum, soft clip, 24-bit conversion. */
static void HOT(master_process)(const float *restrict l, const float *restrict r, int32_t *restrict out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        out[2u * i] = (int32_t)(soft_clip(l[i] * 0.3f) * 8388607.0f) * 256;
        out[2u * i + 1u] = (int32_t)(soft_clip(r[i] * 0.3f) * 8388607.0f) * 256;
    }
}

static void HOT(master_fast_process)(const float *restrict l, const float *restrict r, int32_t *restrict out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        out[2u * i] = (int32_t)(fast_clip(l[i] * 0.3f) * 8388607.0f) * 256;
        out[2u * i + 1u] = (int32_t)(fast_clip(r[i] * 0.3f) * 8388607.0f) * 256;
    }
}

/* ---- the run ---- */

typedef struct {
    tiles_dsp_bench_t *out;
    uint32_t (*now_us)(void);
    uint32_t clk_mhz;
    float l[BLOCK], r[BLOCK];
    float peak;
    uint32_t t0;
} run_t;

static void begin(run_t *x) {
    x->peak = 0.0f;
    x->t0 = x->now_us();
}

static void watch(run_t *x) {
    for (uint32_t i = 0; i < BLOCK; i++) {
        float a = fabsf(x->l[i]), b = fabsf(x->r[i]);
        if (a > x->peak) {
            x->peak = a;
        }
        if (b > x->peak) {
            x->peak = b;
        }
        s_sink += x->l[i] + x->r[i];
    }
}

static void finish(run_t *x, const char *name, uint32_t units) {
    uint32_t elapsed = x->now_us() - x->t0;
    if (x->out->count >= TILES_DSP_BENCH_MAX_ITEMS) {
        return;
    }
    tiles_dsp_bench_item_t *it = &x->out->item[x->out->count++];
    strncpy(it->name, name, sizeof(it->name) - 1u);
    it->name[sizeof(it->name) - 1u] = '\0';
    it->units = units;
    uint64_t samples = (uint64_t)BLOCK * BLOCKS * (units ? units : 1u);
    it->milli_cycles = (uint32_t)((uint64_t)elapsed * x->clk_mhz * 1000u / samples);
    it->peak_milli = (int32_t)(x->peak * 1000.0f);
}

static void clear(run_t *x) {
    memset(x->l, 0, sizeof(x->l));
    memset(x->r, 0, sizeof(x->r));
}

#define VOICES_EACH 8u
#define GRAINS 24u

static void bench_voices(run_t *x) {
    /* Wavetable */
    s_arena_used = 0;
    if (wt_tables()) {
        static wt_voice_t v[VOICES_EACH];
        for (uint32_t i = 0; i < VOICES_EACH; i++) {
            wt_init(&v[i], 48.0f + 5.0f * (float)i, (float)i / 4.0f - 1.0f);
        }
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            for (uint32_t i = 0; i < VOICES_EACH; i++) {
                wt_process(&v[i], x->l, x->r, BLOCK);
            }
            watch(x);
        }
        finish(x, "wavetable_voice", VOICES_EACH);
    }
    /* Analog, from RAM and from flash */
    static va_voice_t va[VOICES_EACH];
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t i = 0; i < VOICES_EACH; i++) {
            va_init(&va[i], 45.0f + 5.0f * (float)i, (float)i / 4.0f - 1.0f);
        }
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            for (uint32_t i = 0; i < VOICES_EACH; i++) {
                if (pass == 0u) {
                    va_process(&va[i], x->l, x->r, BLOCK);
                } else {
                    va_process_flash(&va[i], x->l, x->r, BLOCK);
                }
            }
            watch(x);
        }
        finish(x, pass == 0u ? "analog_voice" : "analog_voice_flash", VOICES_EACH);
    }
    for (uint32_t i = 0; i < VOICES_EACH; i++) {
        va_init(&va[i], 45.0f + 5.0f * (float)i, (float)i / 4.0f - 1.0f);
    }
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        for (uint32_t i = 0; i < VOICES_EACH; i++) {
            va_fast_process(&va[i], x->l, x->r, BLOCK);
        }
        watch(x);
    }
    finish(x, "analog_voice_fast", VOICES_EACH);
    /* FM */
    static fm_voice_t fm[VOICES_EACH];
    for (uint32_t i = 0; i < VOICES_EACH; i++) {
        fm_init(&fm[i], 52.0f + 4.0f * (float)i, (float)i / 4.0f - 1.0f);
    }
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        for (uint32_t i = 0; i < VOICES_EACH; i++) {
            fm_process(&fm[i], x->l, x->r, BLOCK);
        }
        watch(x);
    }
    finish(x, "fm4_voice", VOICES_EACH);
    static fm6_voice_t fm6[VOICES_EACH];
    for (uint32_t i = 0; i < VOICES_EACH; i++) {
        fm6_init(&fm6[i], 52.0f + 4.0f * (float)i, (float)i / 4.0f - 1.0f);
    }
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        for (uint32_t i = 0; i < VOICES_EACH; i++) {
            fm6_process(&fm6[i], x->l, x->r, BLOCK);
        }
        watch(x);
    }
    finish(x, "fm6_voice", VOICES_EACH);
    static fm6b_voice_t fm6b[VOICES_EACH];
    for (uint32_t i = 0; i < VOICES_EACH; i++) {
        fm6b_init(&fm6b[i], 52.0f + 4.0f * (float)i, (float)i / 4.0f - 1.0f, (i & 1u) ? k_fm6_alg1 : k_fm6_alg5);
    }
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        for (uint32_t i = 0; i < VOICES_EACH; i++) {
            fm6b_process(&fm6b[i], x->l, x->r, BLOCK);
        }
        watch(x);
    }
    finish(x, "fm6_voice_block", VOICES_EACH);
    /* Plucked string */
    s_arena_used = 0;
    static pluck_voice_t pl[VOICES_EACH];
    bool ok = true;
    for (uint32_t i = 0; i < VOICES_EACH; i++) {
        ok = ok && pluck_init(&pl[i], 50.0f + 5.0f * (float)i, (float)i / 4.0f - 1.0f);
    }
    if (ok) {
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            for (uint32_t i = 0; i < VOICES_EACH; i++) {
                pluck_process(&pl[i], x->l, x->r, BLOCK);
            }
            watch(x);
        }
        finish(x, "pluck_voice", VOICES_EACH);
    }
    /* Granular */
    s_arena_used = 0;
    if (grain_source()) {
        static grain_t g[GRAINS];
        for (uint32_t i = 0; i < GRAINS; i++) {
            grain_spawn(&g[i]);
            g[i].wph = (float)i / (float)GRAINS;
        }
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            for (uint32_t i = 0; i < GRAINS; i++) {
                grain_process(&g[i], x->l, x->r, BLOCK);
            }
            watch(x);
        }
        finish(x, "grain", GRAINS);
    }
}

static void bench_drums(run_t *x) {
    static kick_t k;
    static snare_t s;
    static hat_t h;
    static clap_t c;
    kick_init(&k);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        kick_process(&k, x->l, x->r, BLOCK);
        watch(x);
    }
    finish(x, "kick", 1);
    kick_init(&k);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        kick_local_process(&k, x->l, x->r, BLOCK);
        watch(x);
    }
    finish(x, "kick_local", 1);
    snare_init(&s);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        snare_process(&s, x->l, x->r, BLOCK);
        watch(x);
    }
    finish(x, "snare", 1);
    hat_init(&h);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        hat_process(&h, x->l, x->r, BLOCK);
        watch(x);
    }
    finish(x, "hat_metallic", 1);
    static nhat_t nh;
    nhat_init(&nh);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        nhat_process(&nh, x->l, x->r, BLOCK);
        watch(x);
    }
    finish(x, "hat_noise", 1);
    clap_init(&c);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        clap_process(&c, x->l, x->r, BLOCK);
        watch(x);
    }
    finish(x, "clap", 1);
    /* 8 sample slots sounding at once: from RAM (the interpolation alone),
     * from flash through the XIP cache, and from flash uncached. */
    s_arena_used = 0;
    uint32_t *ram = arena_alloc(SMP_REGION_SAMPLES * 2u);
    static smp_voice_t sv[SMP_VOICES];
    for (uint32_t pass = 0; pass < 3u; pass++) {
        if (pass == 0u) {
            if (!ram) {
                continue;
            }
            uint32_t rng = 0xBEEFu;
            for (uint32_t i = 0; i < SMP_REGION_SAMPLES / 2u; i++) {
                ram[i] = (uint32_t)(int32_t)(noise(&rng) * 2147483647.0f) & 0x3FFF3FFFu;
            }
        }
        for (uint32_t i = 0; i < SMP_VOICES; i++) {
            smp_init(&sv[i], pass == 0u ? ram : smp_region(i, pass == 1u), i, SMP_LOOP_SAMPLES);
        }
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            for (uint32_t i = 0; i < SMP_VOICES; i++) {
                smp_process(&sv[i], x->l, x->r, BLOCK);
            }
            watch(x);
        }
        static const char *const names[3] = {"sample_ram", "sample_flash", "sample_flash_nocache"};
        finish(x, names[pass], SMP_VOICES);
    }
}

static float s_dry_l[BLOCK], s_dry_r[BLOCK];
static int32_t s_out[2u * BLOCK];

static void make_dry(void) {
    for (uint32_t i = 0; i < BLOCK; i++) {
        s_dry_l[i] = noise(&s_rng) * 0.3f;
        s_dry_r[i] = noise(&s_rng) * 0.3f;
    }
}

static void bench_effects(run_t *x) {
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        make_dry();
        watch(x);
    }
    finish(x, "overhead_dry", 1);
    s_arena_used = 0;
    static reverb_t rvb;
    if (reverb_init(&rvb)) {
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            make_dry();
            reverb_process(&rvb, s_dry_l, s_dry_r, x->l, x->r, BLOCK);
            watch(x);
        }
        finish(x, "reverb_plate_24k", 1);
    }
    static delay_t dl;
    if (delay_init(&dl)) {
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            make_dry();
            delay_process(&dl, s_dry_l, s_dry_r, x->l, x->r, BLOCK);
            watch(x);
        }
        finish(x, "delay_pingpong_24k", 1);
    }
    static chorus_t ch;
    if (chorus_init(&ch)) {
        begin(x);
        for (uint32_t b = 0; b < BLOCKS; b++) {
            clear(x);
            make_dry();
            chorus_process(&ch, s_dry_l, s_dry_r, x->l, x->r, BLOCK);
            watch(x);
        }
        finish(x, "chorus", 1);
    }
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        make_dry();
        master_process(s_dry_l, s_dry_r, s_out, BLOCK);
        x->l[0] = (float)s_out[0] * (1.0f / 2147483648.0f);
        watch(x);
    }
    finish(x, "master_out", 1);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        make_dry();
        master_fast_process(s_dry_l, s_dry_r, s_out, BLOCK);
        x->l[0] = (float)s_out[0] * (1.0f / 2147483648.0f);
        watch(x);
    }
    finish(x, "master_out_fast", 1);
}

/* Everything at once: `played` voices of one engine, `seq` plucked
 * sequencer voices, drums (kick + snare + hat ringing, or the 8 sample
 * slots from flash), reverb, delay, chorus, master. */
enum { ENGINE_ANALOG, ENGINE_FM, ENGINE_WAVETABLE, ENGINE_ANALOG_FAST, ENGINE_FM6 };
enum { DRUMS_NONE, DRUMS_SYNTH, DRUMS_SAMPLES };

static void bench_scene(run_t *x, int engine, uint32_t played, uint32_t seq, int drums, const char *name) {
    s_arena_used = 0;
    static reverb_t rvb;
    static delay_t dl;
    static chorus_t ch;
    static pluck_voice_t pl[4];
    static va_voice_t va[12];
    static fm_voice_t fm[12];
    static wt_voice_t wt[12];
    static fm6b_voice_t fm6[12];
    static smp_voice_t sd[SMP_VOICES];
    static kick_t k;
    static snare_t s;
    static hat_t h;
    static nhat_t nh;
    bool fast = engine == ENGINE_ANALOG_FAST;
    bool lean = fast || engine == ENGINE_FM6; /* noise hat, fast master */
    if (!reverb_init(&rvb) || !delay_init(&dl) || !chorus_init(&ch)) {
        return;
    }
    if (engine == ENGINE_WAVETABLE && !wt_tables()) {
        return;
    }
    for (uint32_t i = 0; i < seq; i++) {
        if (!pluck_init(&pl[i], 40.0f + 7.0f * (float)i, 0.0f)) {
            return;
        }
    }
    for (uint32_t i = 0; i < played; i++) {
        float note = 48.0f + 3.0f * (float)i, pan = (float)i / 6.0f - 1.0f;
        if (engine == ENGINE_ANALOG || engine == ENGINE_ANALOG_FAST) {
            va_init(&va[i], note, pan);
        } else if (engine == ENGINE_FM) {
            fm_init(&fm[i], note, pan);
        } else if (engine == ENGINE_FM6) {
            fm6b_init(&fm6[i], note, pan, (i & 1u) ? k_fm6_alg1 : k_fm6_alg5);
        } else {
            wt_init(&wt[i], note, pan);
        }
    }
    kick_init(&k);
    snare_init(&s);
    hat_init(&h);
    nhat_init(&nh);
    for (uint32_t i = 0; i < SMP_VOICES; i++) {
        smp_init(&sd[i], smp_region(i, true), i, SMP_LOOP_SAMPLES);
    }
    static float send_l[BLOCK], send_r[BLOCK];
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        for (uint32_t i = 0; i < played; i++) {
            if (engine == ENGINE_ANALOG) {
                va_process(&va[i], x->l, x->r, BLOCK);
            } else if (fast) {
                va_fast_process(&va[i], x->l, x->r, BLOCK);
            } else if (engine == ENGINE_FM) {
                fm_process(&fm[i], x->l, x->r, BLOCK);
            } else if (engine == ENGINE_FM6) {
                fm6b_process(&fm6[i], x->l, x->r, BLOCK);
            } else {
                wt_process(&wt[i], x->l, x->r, BLOCK);
            }
        }
        for (uint32_t i = 0; i < seq; i++) {
            pluck_process(&pl[i], x->l, x->r, BLOCK);
        }
        if (drums == DRUMS_SYNTH) {
            kick_process(&k, x->l, x->r, BLOCK);
            snare_process(&s, x->l, x->r, BLOCK);
            if (lean) {
                nhat_process(&nh, x->l, x->r, BLOCK);
            } else {
                hat_process(&h, x->l, x->r, BLOCK);
            }
        } else if (drums == DRUMS_SAMPLES) {
            for (uint32_t i = 0; i < SMP_VOICES; i++) {
                smp_process(&sd[i], x->l, x->r, BLOCK);
            }
        }
        for (uint32_t i = 0; i < BLOCK; i++) { /* one send level for the whole mix */
            send_l[i] = 0.3f * x->l[i];
            send_r[i] = 0.3f * x->r[i];
        }
        chorus_process(&ch, send_l, send_r, x->l, x->r, BLOCK);
        delay_process(&dl, send_l, send_r, x->l, x->r, BLOCK);
        reverb_process(&rvb, send_l, send_r, x->l, x->r, BLOCK);
        if (lean) {
            master_fast_process(x->l, x->r, s_out, BLOCK);
        } else {
            master_process(x->l, x->r, s_out, BLOCK);
        }
        watch(x);
    }
    finish(x, name, 1);
}

/* Capacity: how many voices of a mix fit next to 5 drums ringing and every
 * effect, under the admission target. Voices are dealt round-robin over
 * the mix's engines; drums are 5 synthesized hits or 5 samples from RAM. */
#define CAP_MAX_VOICES 32u
#define CAP_DRUMS 5u
#define CAP_TARGET_PCT 85u
#define CAP_SMP_RAM_SAMPLES 4096u

static uint32_t capacity_run(run_t *x, const int *mix, uint32_t mix_len, uint32_t n, bool sample_drums) {
    static fm6b_voice_t fm6[CAP_MAX_VOICES];
    static va_voice_t va[CAP_MAX_VOICES];
    static wt_voice_t wt[CAP_MAX_VOICES];
    static int engine_of[CAP_MAX_VOICES];
    static reverb_t rvb;
    static delay_t dl;
    static chorus_t ch;
    static kick_t k1, k2;
    static snare_t sn;
    static clap_t cl;
    static nhat_t nh;
    static smp_voice_t sd[CAP_DRUMS];
    static float send_l[BLOCK], send_r[BLOCK];
    s_arena_used = 0;
    if (!reverb_init(&rvb) || !delay_init(&dl) || !chorus_init(&ch)) {
        return 0u;
    }
    bool need_wt = false;
    for (uint32_t i = 0; i < mix_len; i++) {
        need_wt = need_wt || mix[i] == ENGINE_WAVETABLE;
    }
    if (need_wt && !wt_tables()) {
        return 0u;
    }
    uint32_t *ram = NULL;
    if (sample_drums) {
        ram = arena_alloc(CAP_SMP_RAM_SAMPLES * 2u);
        if (!ram) {
            return 0u;
        }
        uint32_t rng = 0xF00Du;
        for (uint32_t i = 0; i < CAP_SMP_RAM_SAMPLES / 2u; i++) {
            ram[i] = (uint32_t)(int32_t)(noise(&rng) * 2147483647.0f) & 0x3FFF3FFFu;
        }
        for (uint32_t i = 0; i < CAP_DRUMS; i++) {
            smp_init(&sd[i], ram, i, CAP_SMP_RAM_SAMPLES - 4u);
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        float note = 45.0f + 2.0f * (float)i, pan = (float)(i % 9u) / 4.0f - 1.0f;
        engine_of[i] = mix[i % mix_len];
        if (engine_of[i] == ENGINE_FM6) {
            fm6b_init(&fm6[i], note, pan, (i & 1u) ? k_fm6_alg1 : k_fm6_alg5);
        } else if (engine_of[i] == ENGINE_ANALOG_FAST) {
            va_init(&va[i], note, pan);
        } else {
            wt_init(&wt[i], note, pan);
        }
    }
    kick_init(&k1);
    kick_init(&k2);
    snare_init(&sn);
    clap_init(&cl);
    nhat_init(&nh);
    begin(x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(x);
        for (uint32_t i = 0; i < n; i++) {
            if (engine_of[i] == ENGINE_FM6) {
                fm6b_process(&fm6[i], x->l, x->r, BLOCK);
            } else if (engine_of[i] == ENGINE_ANALOG_FAST) {
                va_fast_process(&va[i], x->l, x->r, BLOCK);
            } else {
                wt_process(&wt[i], x->l, x->r, BLOCK);
            }
        }
        if (sample_drums) {
            for (uint32_t i = 0; i < CAP_DRUMS; i++) {
                smp_process(&sd[i], x->l, x->r, BLOCK);
            }
        } else {
            kick_process(&k1, x->l, x->r, BLOCK);
            kick_process(&k2, x->l, x->r, BLOCK);
            snare_process(&sn, x->l, x->r, BLOCK);
            clap_process(&cl, x->l, x->r, BLOCK);
            nhat_process(&nh, x->l, x->r, BLOCK);
        }
        for (uint32_t i = 0; i < BLOCK; i++) {
            send_l[i] = 0.3f * x->l[i];
            send_r[i] = 0.3f * x->r[i];
        }
        chorus_process(&ch, send_l, send_r, x->l, x->r, BLOCK);
        delay_process(&dl, send_l, send_r, x->l, x->r, BLOCK);
        reverb_process(&rvb, send_l, send_r, x->l, x->r, BLOCK);
        master_fast_process(x->l, x->r, s_out, BLOCK);
        watch(x);
    }
    uint32_t elapsed = x->now_us() - x->t0;
    return (uint32_t)((uint64_t)elapsed * x->clk_mhz * 1000u / ((uint64_t)BLOCK * BLOCKS));
}

/* Item: units = voices that fit, cycles = the load with them. 0 voices
 * with 0 cycles: the arena ran out. */
static void bench_capacity(run_t *x, const int *mix, uint32_t mix_len, bool sample_drums, const char *name) {
    uint64_t budget_milli = (uint64_t)x->clk_mhz * 1000000u / 48000u * CAP_TARGET_PCT * 10u;
    uint32_t fit = 0, fit_milli = capacity_run(x, mix, mix_len, 0u, sample_drums);
    float fit_peak = x->peak;
    for (uint32_t n = 1; n <= CAP_MAX_VOICES && fit_milli != 0u; n++) {
        uint32_t m = capacity_run(x, mix, mix_len, n, sample_drums);
        if (m == 0u || m > budget_milli) {
            break;
        }
        fit = n;
        fit_milli = m;
        fit_peak = x->peak;
    }
    if (x->out->count >= TILES_DSP_BENCH_MAX_ITEMS) {
        return;
    }
    tiles_dsp_bench_item_t *it = &x->out->item[x->out->count++];
    strncpy(it->name, name, sizeof(it->name) - 1u);
    it->name[sizeof(it->name) - 1u] = '\0';
    it->units = fit;
    it->milli_cycles = fit_milli;
    it->peak_milli = (int32_t)(fit_peak * 1000.0f);
}

void tiles_dsp_bench_run(tiles_dsp_bench_t *out, uint32_t (*now_us)(void), uint32_t clk_mhz) {
    for (uint32_t i = 0; i <= 2048u; i++) {
        s_sin[i] = sinf(6.2831853f * (float)i / 2048.0f);
    }
    memset(out, 0, sizeof(*out));
    out->clk_mhz = clk_mhz;
    static run_t x;
    memset(&x, 0, sizeof(x));
    x.out = out;
    x.now_us = now_us;
    x.clk_mhz = clk_mhz;
    /* What the timing loop itself costs (clear + peak watch), to subtract. */
    begin(&x);
    for (uint32_t b = 0; b < BLOCKS; b++) {
        clear(&x);
        watch(&x);
    }
    finish(&x, "overhead", 1);
    bench_voices(&x);
    bench_drums(&x);
    bench_effects(&x);
    bench_scene(&x, ENGINE_WAVETABLE, 0, 0, DRUMS_SYNTH, "scene_drums_fx_only");
    bench_scene(&x, ENGINE_WAVETABLE, 8, 0, DRUMS_SYNTH, "scene_wavetable_8");
    bench_scene(&x, ENGINE_ANALOG, 8, 4, DRUMS_SYNTH, "scene_analog_8+4");
    bench_scene(&x, ENGINE_FM, 8, 4, DRUMS_SYNTH, "scene_fm_8+4");
    bench_scene(&x, ENGINE_WAVETABLE, 8, 4, DRUMS_SYNTH, "scene_wavetable_8+4");
    bench_scene(&x, ENGINE_ANALOG, 12, 0, DRUMS_SYNTH, "scene_analog_12");
    bench_scene(&x, ENGINE_ANALOG, 12, 4, DRUMS_SYNTH, "scene_analog_12+4");
    bench_scene(&x, ENGINE_ANALOG_FAST, 8, 4, DRUMS_SYNTH, "scene_analog_fast_8+4");
    bench_scene(&x, ENGINE_ANALOG_FAST, 12, 4, DRUMS_SYNTH, "scene_analog_fast_12+4");
    bench_scene(&x, ENGINE_FM6, 12, 0, DRUMS_NONE, "scene_fm6_12_fx");
    bench_scene(&x, ENGINE_FM6, 8, 4, DRUMS_SYNTH, "scene_fm6_8+4");
    bench_scene(&x, ENGINE_FM6, 8, 4, DRUMS_SAMPLES, "scene_fm6_8+4_smp");
    bench_scene(&x, ENGINE_ANALOG_FAST, 8, 4, DRUMS_SAMPLES, "scene_analog_fast_8+4_smp");
    static const int fm[] = {ENGINE_FM6}, an[] = {ENGINE_ANALOG_FAST}, wtm[] = {ENGINE_WAVETABLE};
    static const int mix3[] = {ENGINE_FM6, ENGINE_ANALOG_FAST, ENGINE_WAVETABLE};
    for (uint32_t d = 0; d < 2u; d++) {
        bool smp = d == 1u;
        bench_capacity(&x, fm, 1u, smp, smp ? "cap_fm6_smpdrums" : "cap_fm6_synthdrums");
        bench_capacity(&x, an, 1u, smp, smp ? "cap_analog_smpdrums" : "cap_analog_synthdrums");
        bench_capacity(&x, wtm, 1u, smp, smp ? "cap_wavetable_smpdrums" : "cap_wavetable_synthdrums");
        bench_capacity(&x, mix3, 3u, smp, smp ? "cap_mix3_smpdrums" : "cap_mix3_synthdrums");
    }
}

void tiles_dsp_bench_soak(uint32_t (*now_us)(void), uint32_t ms, bool cached) {
    static smp_voice_t v[SMP_VOICES];
    static float l[BLOCK], r[BLOCK];
    for (uint32_t i = 0; i < SMP_VOICES; i++) {
        smp_init(&v[i], smp_region(i, cached), i, SMP_LOOP_SAMPLES);
    }
    uint32_t start = now_us();
    uint32_t blocks = 0;
    while (now_us() - start < ms * 1000u) {
        memset(l, 0, sizeof(l));
        memset(r, 0, sizeof(r));
        for (uint32_t i = 0; i < SMP_VOICES; i++) {
            smp_process(&v[i], l, r, BLOCK);
        }
        s_sink += l[0] + r[BLOCK - 1u];
        blocks++;
        uint32_t due = start + (uint32_t)((uint64_t)blocks * BLOCK * 1000000u / 48000u);
        while ((int32_t)(now_us() - due) < 0) {
        }
    }
}
