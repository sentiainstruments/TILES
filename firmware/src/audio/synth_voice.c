#include "synth_voice.h"

#include "audio_hot.h"

#include <string.h>

/* Pressure and slide arrive in steps; smoothed over ~8 ms. */
#define INPUT_SMOOTH 0.12f
/* Drift: a new target every ~0.7 s, approached slowly. */
#define DRIFT_SMOOTH 0.0006f
#define DRIFT_CHANCE 0.0002f
/* MIDI note 0 in cycles per sample: 440 Hz * 2^(-69/12) / 48 kHz. */
#define F_NOTE0 (440.0f * 0.018581361f / SYNTH_RATE)
/* Frame crossfade: Q24 ramp, read as Q14. */
#define FF_ONE 16777216.0f
/* Unison copies sum louder: keep a chord's level whatever the count. */
static const float k_unison_norm[SYNTH_UNISON_MAX + 1u] = {1.0f, 1.0f, 0.78f, 0.66f};

#define INLINE static inline __attribute__((always_inline))
/* Fully unrolled, a step's loops spill registers; by 2 is as fast. */
#define UNROLL _Pragma("GCC unroll 2")

/* Control steps a millisecond, and per second. */
#define STEP_MS ((float)SYNTH_CTRL * 1000.0f / SYNTH_RATE)
#define STEPS_PER_S (SYNTH_RATE / (float)SYNTH_CTRL)
/* A new drift target with this chance a step (one every ~0.7 s). */
#define DRIFT_STEP_CHANCE ((uint32_t)(DRIFT_CHANCE * (float)SYNTH_CTRL * 4294967296.0f))
/* Never a pitch or cutoff: the first step works them out. */
#define UNSET (-1000.0f)

static float ms_coef(float ms) {
    if (ms <= 0.0f) {
        return 1.0f;
    }
    return 1.0f - expf(-STEP_MS / ms);
}

static uint32_t copies_of(const synth_patch_t *p) {
    return p->unison < 1u ? 1u : p->unison > SYNTH_UNISON_MAX ? SYNTH_UNISON_MAX : p->unison;
}

void synth_voice_start(synth_voice_t *v, const synth_patch_t *patch, const synth_input_t *in, uint32_t seed,
                       bool legato) {
    /* A voice still sounding keeps its phases, filter and level: a retrigger
     * or a steal rises from where it is, without a click. */
    bool sounding = synth_voice_active(v) && v->patch == patch;
    legato = legato && sounding;
    v->patch = patch;
    if (!sounding) {
        v->rng = seed | 1u;
        uint32_t copies = copies_of(patch);
        for (uint32_t i = 0; i < SYNTH_UNISON_MAX; i++) {
            /* Copies at -d, +d and 0 (the centre last: one copy plays straight). */
            float spread = copies == 1u ? 0.0f : i == 0u ? -1.0f : i == 1u ? 1.0f : 0.0f;
            v->ratio[i] = exp2f(spread * patch->detune_cents / 1200.0f);
            float pan = 0.5f + 0.5f * spread * patch->width;
            float norm = i < copies ? k_unison_norm[copies] : 0.0f;
            v->gain_l[i] = norm * sqrtf(1.0f - pan);
            v->gain_r[i] = norm * sqrtf(pan);
            v->phase[i] = patch->phase_reset ? 0u : synth_rand(&v->rng);
        }
        memset(&v->svf, 0, sizeof(v->svf));
        memset(&v->ladder, 0, sizeof(v->ladder));
        memset(v->right, 0, sizeof(v->right));
        v->semis = UNSET;
        v->oct = UNSET;
        v->lfo_used = patch->lfo_oct != 0.0f || patch->wave_lfo != 0.0f || patch->vibrato_cents != 0.0f;
        v->mod_used = patch->env_oct != 0.0f || patch->wave_env != 0.0f;
        float rate = patch->lfo.rate > 0.0f ? patch->lfo.rate : 1.0f;
        v->lfo_step = patch->lfo.synced ? 1.0f / (rate * STEPS_PER_S) : rate / STEPS_PER_S;
        v->lfo_delay_steps = (uint32_t)(patch->lfo.delay_ms / STEP_MS);
        v->amp.value = 0.0f;
        v->mod.value = 0.0f;
        v->amp.stage = SYNTH_ENV_IDLE;
        v->mod.stage = SYNTH_ENV_IDLE;
        v->level = 0.0f;
        v->pressure_s = in->pressure;
        v->slide_s = in->slide;
        v->wave_pos = synth_clamp(patch->wave, 0.0f, 1.0f) * (float)(TILES_WT_FRAMES - 1u);
        v->drift = 0.0f;
        v->drift_target = 0.0f;
        v->lfo_phase = 0.0f;
        v->lfo_held = 0.0f;
        v->pitch = in->note;
    }
    /* Glide on a legato step, or always in mono mode. */
    bool glide = legato || (sounding && patch->voice_mode == SYNTH_MONO);
    v->glide_coef = glide ? ms_coef(patch->glide_ms) : 1.0f;
    if (!glide) {
        v->pitch = in->note;
    }
    v->oct_base = patch->velocity_oct * in->velocity;
    v->level_base = patch->level * (1.0f - patch->velocity_level + patch->velocity_level * in->velocity * in->velocity);
    synth_env_start(&v->amp, &patch->amp, !legato);
    synth_env_start(&v->mod, &patch->mod, !legato);
    if (!legato) {
        v->elapsed_steps = 0;
        if (patch->lfo.retrigger) {
            v->lfo_phase = 0.0f;
        }
        v->lfo_fade = patch->lfo.delay_ms > 0.0f ? 0.0f : 1.0f;
        v->pitch_env = patch->pitch_env_semis;
        v->pitch_env_k = expf(-STEP_MS / (patch->pitch_env_ms > 1.0f ? patch->pitch_env_ms : 1.0f));
    }
}

void synth_voice_release(synth_voice_t *v) {
    synth_env_release(&v->amp);
    synth_env_release(&v->mod);
}

void synth_voice_kill(synth_voice_t *v) {
    synth_env_kill(&v->amp);
}

/* What one control step hands its SYNTH_CTRL samples. */
typedef struct {
    const int16_t *table; /* the mip level, at the first of two frames */
    int32_t ff, ff_step;  /* the second frame's share, Q24 */
    float level_from, level_step;
} ctrl_t;

/* Envelopes, LFO, pitch, wave position, filter and level for the next
 * SYNTH_CTRL samples. Pitch and cutoff are worked out again only when
 * they move; nothing here divides. */
INLINE void control(synth_voice_t *v, const synth_context_t *ctx, const synth_input_t *in,
                    const tiles_wt_bank_t *bank, uint32_t copies, ctrl_t *c) {
    const synth_patch_t *p = v->patch;
    float pressure = v->pressure_s + (in->pressure - v->pressure_s) * INPUT_SMOOTH;
    float slide = v->slide_s + (in->slide - v->slide_s) * INPUT_SMOOTH;
    v->pressure_s = pressure;
    v->slide_s = slide;
    float amp = synth_env_step(&v->amp);
    float mod = v->mod_used ? synth_env_step(&v->mod) : 0.0f;

    float lfo = 0.0f;
    if (v->lfo_used) {
        v->lfo_phase += p->lfo.synced ? ctx->beats_per_second * v->lfo_step : v->lfo_step;
        if (v->lfo_phase >= 1.0f) {
            v->lfo_phase -= floorf(v->lfo_phase);
            v->lfo_held = synth_rand_bipolar(&v->rng);
        }
        if (v->lfo_fade < 1.0f && v->elapsed_steps >= v->lfo_delay_steps) {
            v->lfo_fade = synth_clamp(v->lfo_fade + 0.004f, 0.0f, 1.0f); /* ~40 ms fade in */
        }
        lfo = synth_lfo_shape(p->lfo.shape, v->lfo_phase, v->lfo_held) * v->lfo_fade;
    }
    v->elapsed_steps++;

    /* Pitch: glide, pitch envelope, vibrato, drift. */
    v->pitch += (in->note - v->pitch) * v->glide_coef;
    float semis = v->pitch + p->vibrato_cents * 0.01f * lfo;
    if (v->pitch_env != 0.0f) {
        semis += v->pitch_env;
        v->pitch_env = __builtin_fabsf(v->pitch_env) > 0.001f ? v->pitch_env * v->pitch_env_k : 0.0f;
    }
    if (p->drift_cents > 0.0f) {
        if (synth_rand(&v->rng) < DRIFT_STEP_CHANCE) {
            v->drift_target = synth_rand_bipolar(&v->rng) * p->drift_cents * 0.01f;
        }
        v->drift += (v->drift_target - v->drift) * (DRIFT_SMOOTH * (float)SYNTH_CTRL);
        semis += v->drift;
    }
    if (semis != v->semis) {
        v->semis = semis;
        float f = F_NOTE0 * synth_exp2(semis * (1.0f / 12.0f));
        float f_max = 0.0f;
        for (uint32_t u = 0; u < copies; u++) {
            float fu = synth_clamp(f * v->ratio[u], 0.0f, 0.49f);
            v->inc[u] = (uint32_t)(fu * 4294967296.0f);
            f_max = fu > f_max ? fu : f_max;
        }
        uint32_t level = tiles_wt_level(f_max); /* the top copy decides */
        v->fshift = 32u - tiles_wt_level_bits(level) - 14u;
        v->table_offset = tiles_wt_level_offset(level) * TILES_WT_FRAMES;
    }

    /* Wave position: the frame pair holding the step's middle; the share
     * ramps across it (a step crossing a frame edge rounds at the edge). */
    float wave = p->wave + p->wave_pressure * pressure + p->wave_slide * slide + p->wave_env * mod + p->wave_lfo * lfo;
    wave = synth_clamp(wave, 0.0f, 1.0f) * (float)(TILES_WT_FRAMES - 1u);
    float from = v->wave_pos;
    v->wave_pos = wave;
    int32_t fi = (int32_t)(0.5f * (from + wave));
    fi = fi > (int32_t)TILES_WT_FRAMES - 2 ? (int32_t)TILES_WT_FRAMES - 2 : fi;
    float a = synth_clamp(from - (float)fi, 0.0f, 1.0f);
    float b = synth_clamp(wave - (float)fi, 0.0f, 1.0f);
    c->ff = (int32_t)(a * FF_ONE);
    c->ff_step = (int32_t)((b - a) * (FF_ONE / (float)SYNTH_CTRL));
    c->table = &bank->data[v->table_offset + (uint32_t)fi];

    /* Filter */
    float oct = v->oct_base + p->env_oct * mod + p->pressure_oct * pressure + p->slide_oct * slide + p->lfo_oct * lfo +
                p->key_track * (1.0f / 12.0f) * (semis - 60.0f);
    if (oct != v->oct) {
        v->oct = oct;
        float cutoff = synth_clamp(p->cutoff_hz * synth_exp2(synth_clamp(oct, -12.0f, 12.0f)), 20.0f, 20000.0f);
        if (p->filter == SYNTH_FILTER_LADDER24) {
            synth_ladder_set(&v->ladder, cutoff, p->resonance);
        } else {
            synth_svf_set(&v->svf, cutoff, p->resonance);
        }
    }

    /* Level: envelope, velocity, pressure. */
    float target = amp * v->level_base * (1.0f + p->pressure_level * pressure);
    c->level_from = v->level;
    c->level_step = (target - v->level) * (1.0f / (float)SYNTH_CTRL);
    v->level = target;
}

/* The DSP extension's dual 16-bit multiply-add and high-word multiplies;
 * the same in C for the computer. */
#if defined(__ARM_FEATURE_DSP)
INLINE int32_t smuad(uint32_t a, uint32_t b) {
    int32_t r;
    __asm("smuad %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
INLINE int32_t smmul(int32_t a, int32_t b) {
    int32_t r;
    __asm("smmul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}
INLINE int32_t smmla(int32_t a, int32_t b, int32_t acc) {
    int32_t r;
    __asm("smmla %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(acc));
    return r;
}
#else
INLINE int32_t smuad(uint32_t a, uint32_t b) {
    return (int16_t)a * (int16_t)b + (int16_t)(a >> 16) * (int16_t)(b >> 16);
}
INLINE int32_t smmul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> 32);
}
INLINE int32_t smmla(int32_t a, int32_t b, int32_t acc) {
    return acc + smmul(a, b);
}
#endif

/* Both frames' samples at one table point (unaligned for odd frames: one
 * load still). */
INLINE uint32_t frame_pair(const int16_t *q) {
    uint32_t w;
    __builtin_memcpy(&w, q, sizeof(w));
    return w;
}

/* One copy's sample at phase ph: the frames crossfaded by SMUAD with
 * weights w, then linear interpolation between table points. Q14. */
INLINE int32_t read_copy(const int16_t *t, uint32_t fs, uint32_t ph, uint32_t w) {
    uint32_t j = ph >> fs;
    const int16_t *q = t + (j >> 14) * TILES_WT_FRAMES;
    int32_t x0 = smuad(frame_pair(q), w);
    int32_t x1 = smuad(frame_pair(q + TILES_WT_FRAMES), w);
    int32_t fr = (int32_t)((j & 0x3FFFu) << 17); /* Q31 */
    return x0 + (int32_t)((uint32_t)smmul(x1 - x0, fr) << 1);
}

/* Every copy's step in one loop, sums in registers, into al (and ar).
 * Spread copies are summed at Q31 gains (Q14 samples times gain / 2);
 * copies not spread share one gain, applied later: Q14 samples summed. */
INLINE void osc(synth_voice_t *v, const ctrl_t *c, const int32_t *gl, const int32_t *gr, const uint32_t copies,
                const bool stereo, int32_t *al, int32_t *ar) {
    const int16_t *t = c->table;
    const uint32_t fs = v->fshift;
    uint32_t p0 = v->phase[0], p1 = v->phase[1], p2 = v->phase[2];
    const uint32_t i0 = v->inc[0], i1 = v->inc[1], i2 = v->inc[2];
    int32_t ffa = c->ff;
    const int32_t ffs = c->ff_step;
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        /* The second frame's share Q14 in the top half, the first's below. */
        uint32_t w = 16384u + (uint32_t)(ffa >> 10) * 65535u;
        int32_t x = read_copy(t, fs, p0, w);
        int32_t sl = stereo ? smmul(x, gl[0]) : x;
        int32_t sr = stereo ? smmul(x, gr[0]) : 0;
        if (copies > 1u) {
            x = read_copy(t, fs, p1, w);
            sl = stereo ? smmla(x, gl[1], sl) : sl + x;
            sr = stereo ? smmla(x, gr[1], sr) : 0;
        }
        if (copies > 2u) {
            x = read_copy(t, fs, p2, w);
            sl = stereo ? smmla(x, gl[2], sl) : sl + x;
            sr = stereo ? smmla(x, gr[2], sr) : 0;
        }
        al[s] = sl;
        if (stereo) {
            ar[s] = sr;
        }
        p0 += i0;
        p1 += i1;
        p2 += i2;
        ffa += ffs;
    }
    v->phase[0] = p0;
    if (copies > 1u) {
        v->phase[1] = p1;
    }
    if (copies > 2u) {
        v->phase[2] = p2;
    }
}

/* To float, through the drive and folder when the sound has them. */
INLINE void to_float(const int32_t *acc, float *buf, float scale, bool shaped, float fold) {
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        float x = (float)acc[s] * scale;
        buf[s] = shaped ? synth_fold(synth_sat(x), fold) : x;
    }
}

/* The filters run with coefficients and state in registers and add their
 * output into l and r at the step's level ramp: one channel copied to both
 * (copies not spread), or two side by side in one loop, whose independent
 * sums keep the FPU from waiting on each result. */
INLINE void svf_run(const synth_svf_t *f, float *sl, float *sr, const float *bl, const float *br, bool stereo,
                    float *l, float *r, float lvl, float step, int mode) {
    const float a1 = f->a1, a2 = f->a2, a3 = f->a3, k = f->k;
    float l1 = sl[0], l2 = sl[1], r1 = sr[0], r2 = sr[1];
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        float yl = synth_svf_step(a1, a2, a3, k, &l1, &l2, bl[s], mode) * lvl;
        float yr = stereo ? synth_svf_step(a1, a2, a3, k, &r1, &r2, br[s], mode) * lvl : yl;
        l[s] += yl;
        r[s] += yr;
        lvl += step;
    }
    sl[0] = l1;
    sl[1] = l2;
    if (stereo) {
        sr[0] = r1;
        sr[1] = r2;
    }
}

INLINE void ladder_run(const synth_ladder_t *f, float *sl, float *sr, const float *bl, const float *br, bool stereo,
                       float *l, float *r, float lvl, float step) {
    const float G = f->G, b = f->beta, k = f->k, comp = f->comp, G4comp = f->G4comp, inv = f->inv;
    float stl[4] = {sl[0], sl[1], sl[2], sl[3]};
    float str[4] = {sr[0], sr[1], sr[2], sr[3]};
    UNROLL
    for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
        float yl = synth_ladder_step(G, b, k, comp, G4comp, inv, stl, bl[s]) * lvl;
        float yr = stereo ? synth_ladder_step(G, b, k, comp, G4comp, inv, str, br[s]) * lvl : yl;
        l[s] += yl;
        r[s] += yr;
        lvl += step;
    }
    for (uint32_t i = 0; i < 4u; i++) {
        sl[i] = stl[i];
        if (stereo) {
            sr[i] = str[i];
        }
    }
}

INLINE void filter(synth_voice_t *v, int kind, bool stereo, const float *bl, const float *br, float *l, float *r,
                   const ctrl_t *c) {
    switch (kind) {
    case SYNTH_FILTER_LADDER24:
        ladder_run(&v->ladder, v->ladder.s, v->right, bl, br, stereo, l, r, c->level_from, c->level_step);
        break;
    case SYNTH_FILTER_BP12:
        svf_run(&v->svf, v->svf.ic, v->right, bl, br, stereo, l, r, c->level_from, c->level_step, SYNTH_SVF_BP);
        break;
    case SYNTH_FILTER_HP12:
        svf_run(&v->svf, v->svf.ic, v->right, bl, br, stereo, l, r, c->level_from, c->level_step, SYNTH_SVF_HP);
        break;
    default:
        svf_run(&v->svf, v->svf.ic, v->right, bl, br, stereo, l, r, c->level_from, c->level_step, SYNTH_SVF_LP);
        break;
    }
}

void TILES_AUDIO_HOT(synth_voice_render)(synth_voice_t *v, const synth_context_t *ctx, const synth_input_t *in,
                                         float *l, float *r, uint32_t n) {
    const synth_patch_t *p = v->patch;
    const tiles_wt_bank_t *bank = ctx->banks[p->bank < TILES_WT_BANK_COUNT ? p->bank : 0u];
    if (bank == NULL) {
        return;
    }
    const uint32_t copies = copies_of(p);
    const bool stereo = copies > 1u && p->width > 0.0f;
    const bool shaped = p->drive > 0.0f || p->fold > 0.0f;
    /* Q14 samples (at half the Q31 gains when spread) in, the bank's scale
     * (and the drive) out. */
    const float drive = shaped ? 1.0f + p->drive : 1.0f;
    const float scale = stereo ? bank->gain * (1.0f / 8192.0f) * drive
                               : bank->gain * v->gain_l[0] * (1.0f / 16384.0f) * drive;
    const float fold = p->fold;
    const int kind = p->filter;
    int32_t gl[SYNTH_UNISON_MAX], gr[SYNTH_UNISON_MAX];
    for (uint32_t u = 0; u < SYNTH_UNISON_MAX; u++) {
        gl[u] = (int32_t)(synth_clamp(v->gain_l[u], 0.0f, 0.9999f) * 2147483648.0f);
        gr[u] = (int32_t)(synth_clamp(v->gain_r[u], 0.0f, 0.9999f) * 2147483648.0f);
    }
    int32_t al[SYNTH_CTRL], ar[SYNTH_CTRL];
    float bl[SYNTH_CTRL], br[SYNTH_CTRL];

    for (uint32_t base = 0; base < n; base += SYNTH_CTRL) {
        ctrl_t c;
        control(v, ctx, in, bank, copies, &c);
        switch (copies * 2u + (stereo ? 1u : 0u)) {
        case 4u:
            osc(v, &c, gl, gr, 2u, false, al, ar);
            break;
        case 5u:
            osc(v, &c, gl, gr, 2u, true, al, ar);
            break;
        case 6u:
            osc(v, &c, gl, gr, 3u, false, al, ar);
            break;
        case 7u:
            osc(v, &c, gl, gr, 3u, true, al, ar);
            break;
        default:
            osc(v, &c, gl, gr, 1u, false, al, ar);
            break;
        }
        to_float(al, bl, scale, shaped, fold);
        if (stereo) {
            to_float(ar, br, scale, shaped, fold);
            filter(v, kind, true, bl, br, &l[base], &r[base], &c);
        } else {
            filter(v, kind, false, bl, br, &l[base], &r[base], &c);
        }
    }
}
