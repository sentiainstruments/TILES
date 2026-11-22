#include "grain_voice.h"

#include "audio_hot.h"

#include <string.h>

/* Our own code after the published technique: C. Roads, Microsound
 * (2001), asynchronous granular synthesis; the window a smoothstep on a
 * triangle (a Hann-like bell whose peak can move forward). */

#define INLINE static inline __attribute__((always_inline))
/* Loops kept rolled: unrolled, each grain's code copied eight times cost
 * ~7 KB of RAM (core 1 runs from RAM) for little speed. */
#define ROLLED _Pragma("GCC unroll 1")
/* Once a control step or a grain: small rather than fast (RAM again). */
#if defined(__clang__)
#define SMALL __attribute__((noinline))
#else
#define SMALL __attribute__((noinline, optimize("Os")))
#endif
#define UNROLL _Pragma("GCC unroll 2")

/* As the wavetable voice: inputs smoothed over ~8 ms, note 0 in cycles a
 * sample. */
#define INPUT_SMOOTH 0.12f
#define F_NOTE0 (440.0f * 0.018581361f / SYNTH_RATE)
#define STEP_MS ((float)SYNTH_CTRL * 1000.0f / SYNTH_RATE)
#define STEPS_PER_S (SYNTH_RATE / (float)SYNTH_CTRL)
#define UNSET (-1000.0f)
/* Shorter grains click; longer ones outgrow the cloud's 8. */
#define SIZE_MIN_MS 5.0f
#define SIZE_MAX_MS 1000.0f
#define DENSITY_MIN 0.5f
#define DENSITY_MAX 400.0f
/* The tape: grains need room to move, and read at most 3 octaves up, so 8
 * samples' travel never crosses it twice. */
#define TAPE_MIN_FRAMES 1024u
#define TAPE_STEP_MAX 8.0f
#define LEVEL_UNSET 0xFFu

static float ms_coef(float ms) {
    return ms <= 0.0f ? 1.0f : 1.0f - expf(-STEP_MS / ms);
}

INLINE float rand01(uint32_t *rng) {
    return (float)(synth_rand(rng) >> 8) * (1.0f / 16777216.0f);
}

/* 0..1 and back, for the wave's place (a sweep turns at the last frame). */
INLINE float fold01(float x) {
    x -= 2.0f * floorf(x * 0.5f);
    return x > 1.0f ? 2.0f - x : x;
}

INLINE float wrap01(float x) {
    return x - floorf(x);
}

INLINE float smooth(float u) {
    return u * u * (3.0f - 2.0f * u);
}

SMALL void tiles_grain_voice_start(tiles_grain_voice_t *v, const synth_patch_t *patch, const synth_input_t *in,
                                   uint32_t seed, bool sounding, bool legato) {
    legato = legato && sounding;
    if (!sounding) {
        memset(v, 0, sizeof(*v));
        v->rng = seed | 1u;
        v->oct = UNSET;
        v->pressure_s = in->pressure;
        v->slide_s = in->slide;
        v->pitch = in->note;
        float rate = patch->lfo.rate > 0.0f ? patch->lfo.rate : 1.0f;
        v->lfo_step = patch->lfo.synced ? 1.0f / (rate * STEPS_PER_S) : rate / STEPS_PER_S;
    }
    bool glide = legato || (sounding && patch->voice_mode == SYNTH_MONO);
    v->glide_coef = glide ? ms_coef(patch->glide_ms) : 1.0f;
    if (!glide) {
        v->pitch = in->note;
    }
    v->oct_base = patch->velocity_oct * in->velocity;
    v->level_base = patch->level * (1.0f - patch->velocity_level + patch->velocity_level * in->velocity * in->velocity);
    synth_env_start(&v->amp, &patch->amp, !legato);
    synth_env_start(&v->mod, &patch->mod, !legato);
    if (!legato && patch->lfo.retrigger) {
        v->lfo_phase = 0.0f;
    }
}

void tiles_grain_voice_release(tiles_grain_voice_t *v) {
    synth_env_release(&v->amp);
    synth_env_release(&v->mod);
}

void tiles_grain_voice_kill(tiles_grain_voice_t *v) {
    synth_env_kill(&v->amp);
}

uint32_t tiles_grain_voice_grains(const tiles_grain_voice_t *v) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < TILES_GRAIN_MAX; i++) {
        n += v->g[i].on ? 1u : 0u;
    }
    return n;
}

float tiles_grain_overlap(const tiles_grain_params_t *g) {
    return synth_clamp(g->density * g->size_ms * 0.001f, 1.0f, (float)TILES_GRAIN_MAX);
}

/* What one control step hands its grains and filter. */
typedef struct {
    float f;         /* the voice's pitch, cycles a sample */
    float tape_step; /* tape samples a sample at the voice's pitch */
    float place;     /* 0..1 */
    float size;      /* samples */
    float spray;     /* 0..1 */
    float scatter;   /* cents */
    float level_from, level_step;
} ctrl_t;

/* A new grain in a free slot (none free: skipped). */
static SMALL void spawn(tiles_grain_voice_t *v, const synth_patch_t *p, const tiles_grain_params_t *gp, bool tape,
                  uint32_t frames, const ctrl_t *c) {
    tiles_grain_t *g = NULL;
    for (uint32_t i = 0; i < TILES_GRAIN_MAX; i++) {
        if (!v->g[i].on) {
            g = &v->g[i];
            break;
        }
    }
    if (g == NULL || (tape && frames < TAPE_MIN_FRAMES)) {
        return;
    }
    float ratio = c->scatter != 0.0f ? synth_exp2(c->scatter * (1.0f / 1200.0f) * synth_rand_bipolar(&v->rng)) : 1.0f;
    float pick = rand01(&v->rng);
    if (pick < gp->octave_up) {
        ratio *= 2.0f;
    } else if ((pick -= gp->octave_up) < gp->octave_down) {
        ratio *= 0.5f;
    } else if ((pick -= gp->octave_down) < gp->fifth) {
        ratio *= 1.5f;
    }
    bool back = rand01(&v->rng) < gp->reverse;
    float len = c->size;
    if (tape) {
        float step = synth_clamp(ratio * c->tape_step, 0.0f, TAPE_STEP_MAX);
        float room = (float)(frames - 2u);
        float span = len * step;
        if (span > room) {
            len = room / step;
            span = room;
        }
        float start = wrap01(c->place + 0.5f * c->spray * synth_rand_bipolar(&v->rng)) * room;
        if (!back && start + span > room) {
            start = room - span;
        } else if (back && start < span) {
            start = span;
        }
        g->phase = (uint32_t)(start * 65536.0f);
    } else {
        float fp = fold01(c->place + c->spray * synth_rand_bipolar(&v->rng)) * (float)(TILES_WT_FRAMES - 1u);
        uint32_t fi = (uint32_t)fp;
        if (fi < TILES_WT_FRAMES - 1u && rand01(&v->rng) < fp - (float)fi) {
            fi++; /* between two frames: either, by nearness */
        }
        g->frame = (uint8_t)fi;
        g->level = LEVEL_UNSET;
        g->phase = p->phase_reset ? 0u : synth_rand(&v->rng);
    }
    g->ratio = back ? -ratio : ratio;
    float pan = 0.5f + 0.5f * synth_clamp(gp->spread, 0.0f, 1.0f) * synth_rand_bipolar(&v->rng);
    g->gl = (int16_t)(sqrtf(synth_clamp(1.0f - pan, 0.0f, 1.0f)) * 32767.0f);
    g->gr = (int16_t)(sqrtf(synth_clamp(pan, 0.0f, 1.0f)) * 32767.0f);
    float rise = 0.5f - 0.45f * synth_clamp(gp->skew, 0.0f, 1.0f);
    g->u = 0.0f;
    g->slope = (float)SYNTH_CTRL / (rise * (len > 64.0f ? len : 64.0f));
    g->on = 1u;
}

/* Envelopes, LFO, pitch, the place and the grains' settings, new grains,
 * the filter and the level for the next SYNTH_CTRL samples. */
static SMALL void control(tiles_grain_voice_t *v, const synth_patch_t *p, const tiles_grain_params_t *gp,
                    const synth_context_t *ctx, const synth_input_t *in, bool tape, uint32_t frames, ctrl_t *c) {
    float pressure = v->pressure_s + (in->pressure - v->pressure_s) * INPUT_SMOOTH;
    float slide = v->slide_s + (in->slide - v->slide_s) * INPUT_SMOOTH;
    v->pressure_s = pressure;
    v->slide_s = slide;
    float amp = synth_env_step(&v->amp);
    float mod = p->env_oct != 0.0f || p->wave_env != 0.0f ? synth_env_step(&v->mod) : 0.0f;
    float lfo = 0.0f;
    if (p->lfo_oct != 0.0f || p->wave_lfo != 0.0f || p->vibrato_cents != 0.0f) {
        v->lfo_phase += p->lfo.synced ? ctx->beats_per_second * v->lfo_step : v->lfo_step;
        if (v->lfo_phase >= 1.0f) {
            v->lfo_phase -= floorf(v->lfo_phase);
            v->lfo_held = synth_rand_bipolar(&v->rng);
        }
        lfo = synth_lfo_shape(p->lfo.shape, v->lfo_phase, v->lfo_held);
    }

    v->pitch += (in->note - v->pitch) * v->glide_coef;
    float semis = v->pitch + p->vibrato_cents * 0.01f * lfo;
    c->f = F_NOTE0 * synth_exp2(semis * (1.0f / 12.0f));
    c->tape_step = synth_exp2((semis - (float)gp->root) * (1.0f / 12.0f));

    if (gp->scan != 0.0f) {
        /* Tape: x1 crosses it at its own speed; wave: sweeps a second. */
        float per_step = tape ? gp->scan * (float)SYNTH_CTRL / (float)(frames > 0u ? frames : 1u)
                              : gp->scan * ((float)SYNTH_CTRL / SYNTH_RATE);
        v->scan_pos = wrap01(v->scan_pos + per_step);
    }
    float place = p->wave + p->wave_pressure * pressure + p->wave_slide * slide + p->wave_env * mod +
                  p->wave_lfo * lfo + v->scan_pos;
    c->place = tape ? wrap01(place) : fold01(place);
    float size_ms = synth_clamp(gp->size_ms * synth_exp2(gp->size_pressure * pressure), SIZE_MIN_MS, SIZE_MAX_MS);
    float density = synth_clamp(gp->density * synth_exp2(gp->density_pressure * pressure), DENSITY_MIN, DENSITY_MAX);
    c->size = size_ms * (SYNTH_RATE / 1000.0f);
    c->spray = synth_clamp(gp->spray + gp->spray_pressure * pressure + gp->spray_slide * slide, 0.0f, 1.0f);
    c->scatter = gp->scatter_cents + gp->scatter_slide * slide;

    /* Grains at the density's pace, the spacing jittered; at most two a
     * step (6,000 a second is past the cloud's 8 anyway). */
    v->next -= (float)SYNTH_CTRL;
    float gap = SYNTH_RATE / density;
    for (uint32_t k = 0; k < 2u && v->next <= 0.0f; k++) {
        spawn(v, p, gp, tape, frames, c);
        v->next += gap * (1.0f + 0.9f * synth_clamp(gp->jitter, 0.0f, 1.0f) * synth_rand_bipolar(&v->rng));
    }
    if (v->next < 0.0f) {
        v->next = 0.0f;
    }

    float oct = v->oct_base + p->env_oct * mod + p->pressure_oct * pressure + p->slide_oct * slide +
                p->lfo_oct * lfo + p->key_track * (1.0f / 12.0f) * (semis - 60.0f);
    if (oct != v->oct) {
        v->oct = oct;
        float cutoff = synth_clamp(p->cutoff_hz * synth_exp2(synth_clamp(oct, -12.0f, 12.0f)), 20.0f, 20000.0f);
        synth_svf_set(&v->svf, cutoff, p->resonance);
    }

    /* Unrelated grains add up by power: the cloud keeps its level as the
     * overlap grows. */
    float overlap = synth_clamp(density * size_ms * 0.001f, 1.0f, (float)TILES_GRAIN_MAX);
    float target = amp * v->level_base * (1.0f + p->pressure_level * pressure) / sqrtf(overlap);
    c->level_from = v->level;
    c->level_step = (target - v->level) * (1.0f / (float)SYNTH_CTRL);
    v->level = target;
}

/* The window over this step: its value at both ends, and whether the
 * grain ends with it. */
INLINE bool window_step(tiles_grain_t *g, float fall_ratio, float *w0, float *w1) {
    float u0 = g->u, u1 = u0 + g->slope;
    bool ends = false;
    if (g->slope > 0.0f) {
        if (u1 >= 1.0f) {
            u1 = 1.0f;
            g->slope = -g->slope * fall_ratio;
        }
    } else if (u1 <= 0.0f) {
        u1 = 0.0f;
        ends = true;
    }
    g->u = u1;
    *w0 = smooth(u0);
    *w1 = smooth(u1);
    return ends;
}

/* One wave grain's step into bl/br: its frame at its mip level, linear
 * between table points. */
INLINE void wave_grain(tiles_grain_t *g, const tiles_wt_bank_t *bank, float f, float wl, float wr, float dwl,
                       float dwr, float *bl, float *br) {
    float fg = synth_clamp(f * __builtin_fabsf(g->ratio), 0.0f, 0.49f);
    uint32_t level = tiles_wt_level(fg);
    if (level != g->level) {
        g->level = (uint8_t)level;
        g->fshift = (uint8_t)(32u - tiles_wt_level_bits(level) - 14u);
        g->src = &bank->data[tiles_wt_level_offset(level) * TILES_WT_FRAMES + g->frame];
    }
    uint32_t inc = (uint32_t)(fg * 4294967296.0f);
    inc = g->ratio < 0.0f ? 0u - inc : inc;
    const int16_t *src = g->src;
    const uint32_t fs = g->fshift;
    uint32_t ph = g->phase;
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        uint32_t j = ph >> fs;
        const int16_t *q = src + (j >> 14) * TILES_WT_FRAMES;
        int32_t x0 = q[0];
        int32_t x = x0 + (((q[TILES_WT_FRAMES] - x0) * (int32_t)(j & 0x3FFFu)) >> 14);
        float xf = (float)x;
        bl[s] += xf * wl;
        br[s] += xf * wr;
        wl += dwl;
        wr += dwr;
        ph += inc;
    }
    g->phase = ph;
}

/* One tape grain's step: a grain about to leave the tape turns back
 * (a bend can push it past where it was placed). */
INLINE bool tape_grain(tiles_grain_t *g, const int16_t *t, uint32_t frames, float tape_step, float wl, float wr,
                       float dwl, float dwr, float *bl, float *br) {
    float step = synth_clamp(g->ratio * tape_step, -TAPE_STEP_MAX, TAPE_STEP_MAX);
    const float hi = (float)(frames - 2u) * 65536.0f;
    float end = (float)g->phase + (float)SYNTH_CTRL * step * 65536.0f;
    if (end < 0.0f || end > hi) {
        g->ratio = -g->ratio;
        step = -step;
        end = (float)g->phase + (float)SYNTH_CTRL * step * 65536.0f;
        if (end < 0.0f || end > hi || (float)g->phase > hi) {
            return false;
        }
    }
    int32_t inc = (int32_t)(step * 65536.0f);
    uint32_t ph = g->phase;
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        uint32_t i = ph >> 16;
        int32_t x0 = t[i];
        int32_t x = x0 + (((t[i + 1u] - x0) * (int32_t)((ph & 0xFFFFu) >> 1)) >> 15);
        float xf = (float)x;
        bl[s] += xf * wl;
        br[s] += xf * wr;
        wl += dwl;
        wr += dwr;
        ph += (uint32_t)inc; /* past 2^31 at 32,768 samples: wraps as unsigned */
    }
    g->phase = ph;
    return true;
}

/* Both channels through the filter, at the step's level ramp, into l and r. */
INLINE void filter_run(tiles_grain_voice_t *v, int kind, const float *bl, const float *br, float *l, float *r,
                       const ctrl_t *c) {
    const float a1 = v->svf.a1, a2 = v->svf.a2, a3 = v->svf.a3, k = v->svf.k;
    float l1 = v->svf.ic[0], l2 = v->svf.ic[1], r1 = v->right[0], r2 = v->right[1];
    float lvl = c->level_from;
    const float step = c->level_step;
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        l[s] += synth_svf_step(a1, a2, a3, k, &l1, &l2, bl[s], kind) * lvl;
        r[s] += synth_svf_step(a1, a2, a3, k, &r1, &r2, br[s], kind) * lvl;
        lvl += step;
    }
    v->svf.ic[0] = l1;
    v->svf.ic[1] = l2;
    v->right[0] = r1;
    v->right[1] = r2;
}

void TILES_AUDIO_HOT(tiles_grain_voice_render)(tiles_grain_voice_t *v, const synth_patch_t *p,
                                               const synth_context_t *ctx, const synth_input_t *in,
                                               float *restrict l, float *restrict r, uint32_t n) {
    const tiles_grain_params_t *gp = p->grain;
    const bool tape = gp != NULL && gp->source == TILES_GRAIN_SRC_TAPE;
    const tiles_wt_bank_t *bank = ctx->banks[p->bank < TILES_WT_BANK_COUNT ? p->bank : 0u];
    const int16_t *t = ctx->tape;
    const uint32_t frames = t != NULL ? ctx->tape_frames : 0u;
    if (gp == NULL || (!tape && bank == NULL)) {
        v->amp.stage = SYNTH_ENV_IDLE; /* nothing to play: the voice ends */
        return;
    }
    /* int16 to float, and Q15 sides to gains. */
    const float scale = (tape ? 1.0f / 32768.0f : bank->gain) * (1.0f / 32767.0f);
    const float rise = 0.5f - 0.45f * synth_clamp(gp->skew, 0.0f, 1.0f);
    const float fall_ratio = rise / (1.0f - rise);
    const int kind = p->filter == SYNTH_FILTER_BP12 ? SYNTH_SVF_BP : p->filter == SYNTH_FILTER_HP12 ? SYNTH_SVF_HP
                                                                                                   : SYNTH_SVF_LP;
    float bl[SYNTH_CTRL], br[SYNTH_CTRL];
    for (uint32_t base = 0; base < n; base += SYNTH_CTRL) {
        ctrl_t c;
        control(v, p, gp, ctx, in, tape, frames, &c);
        memset(bl, 0, sizeof(bl));
        memset(br, 0, sizeof(br));
        ROLLED
        for (uint32_t i = 0; i < TILES_GRAIN_MAX; i++) {
            tiles_grain_t *g = &v->g[i];
            if (!g->on) {
                continue;
            }
            if (tape && frames < TAPE_MIN_FRAMES) {
                g->on = 0u; /* the tape went (its memory lent, or reloading) */
                continue;
            }
            float w0, w1;
            bool ends = window_step(g, fall_ratio, &w0, &w1);
            float sl = (float)g->gl * scale, sr = (float)g->gr * scale;
            float dw = (w1 - w0) * (1.0f / (float)SYNTH_CTRL);
            bool ok = true;
            if (tape) {
                ok = tape_grain(g, t, frames, c.tape_step, w0 * sl, w0 * sr, dw * sl, dw * sr, bl, br);
            } else {
                wave_grain(g, bank, c.f, w0 * sl, w0 * sr, dw * sl, dw * sr, bl, br);
            }
            if (ends || !ok) {
                g->on = 0u;
            }
        }
        filter_run(v, kind, bl, br, &l[base], &r[base], &c);
    }
}
