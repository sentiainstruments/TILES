#include "synth_voice.h"

#include "audio_hot.h"

#include <string.h>

/* 2^32 / 48000: Hz to phase increment. */
#define HZ_TO_INC 89478.485f
/* Pressure and slide arrive in steps; smoothed over ~8 ms. */
#define INPUT_SMOOTH 0.12f
/* Drift: a new target every ~0.7 s, approached slowly. */
#define DRIFT_SMOOTH 0.0006f
#define DRIFT_CHANCE 0.0002f
/* Unison copies sum louder: keep a chord's level whatever the count. */
static const float k_unison_norm[SYNTH_UNISON_MAX + 1u] = {1.0f, 1.0f, 0.78f, 0.66f};

static inline float hz_of(float note) {
    return 440.0f * synth_exp2((note - 69.0f) * (1.0f / 12.0f));
}

static float ms_coef(float ms) {
    if (ms <= 0.0f) {
        return 1.0f;
    }
    return 1.0f - expf(-1.0f / (ms * 0.001f * SYNTH_RATE / (float)SYNTH_CTRL));
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
        uint32_t copies = patch->unison < 1u ? 1u : patch->unison > SYNTH_UNISON_MAX ? SYNTH_UNISON_MAX : patch->unison;
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
        memset(v->svf, 0, sizeof(v->svf));
        memset(v->ladder, 0, sizeof(v->ladder));
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
    synth_env_start(&v->amp, &patch->amp, !legato);
    synth_env_start(&v->mod, &patch->mod, !legato);
    if (!legato) {
        v->elapsed_steps = 0;
        if (patch->lfo.retrigger) {
            v->lfo_phase = 0.0f;
        }
        v->lfo_fade = patch->lfo.delay_ms > 0.0f ? 0.0f : 1.0f;
    }
}

void synth_voice_release(synth_voice_t *v) {
    synth_env_release(&v->amp);
    synth_env_release(&v->mod);
}

void synth_voice_kill(synth_voice_t *v) {
    synth_env_kill(&v->amp);
}

/* One control step: envelopes, LFO, pitch, filter and levels for the next
 * SYNTH_CTRL samples. */
typedef struct {
    uint32_t inc[SYNTH_UNISON_MAX];
    float wave_from, wave_step;
    float level_from, level_step;
    float drive;
} ctrl_t;

static void TILES_AUDIO_HOT(control)(synth_voice_t *v, const synth_context_t *ctx, const synth_input_t *in, ctrl_t *c) {
    const synth_patch_t *p = v->patch;
    v->pressure_s += (in->pressure - v->pressure_s) * INPUT_SMOOTH;
    v->slide_s += (in->slide - v->slide_s) * INPUT_SMOOTH;
    float amp = synth_env_step(&v->amp);
    float mod = synth_env_step(&v->mod);

    /* LFO */
    float rate = p->lfo.synced ? ctx->beats_per_second / (p->lfo.rate > 0.0f ? p->lfo.rate : 1.0f) : p->lfo.rate;
    v->lfo_phase += rate * (float)SYNTH_CTRL / SYNTH_RATE;
    if (v->lfo_phase >= 1.0f) {
        v->lfo_phase -= floorf(v->lfo_phase);
        v->lfo_held = synth_rand_bipolar(&v->rng);
    }
    if (v->lfo_fade < 1.0f) {
        float t_ms = (float)v->elapsed_steps * (float)SYNTH_CTRL * 1000.0f / SYNTH_RATE;
        if (t_ms >= p->lfo.delay_ms) {
            v->lfo_fade += 0.004f; /* ~40 ms fade in */
            v->lfo_fade = v->lfo_fade > 1.0f ? 1.0f : v->lfo_fade;
        }
    }
    float lfo = synth_lfo_shape(p->lfo.shape, v->lfo_phase, v->lfo_held) * v->lfo_fade;
    v->elapsed_steps++;

    /* Pitch: glide, pitch envelope, vibrato, drift. */
    v->pitch += (in->note - v->pitch) * v->glide_coef;
    float semis = v->pitch;
    if (p->pitch_env_semis != 0.0f) {
        float t_ms = (float)v->elapsed_steps * (float)SYNTH_CTRL * 1000.0f / SYNTH_RATE;
        semis += p->pitch_env_semis * synth_exp2(-1.442695f * t_ms / (p->pitch_env_ms > 1.0f ? p->pitch_env_ms : 1.0f));
    }
    semis += (p->vibrato_cents * lfo) * 0.01f;
    if (p->drift_cents > 0.0f) {
        if ((float)(synth_rand(&v->rng) >> 8) * (1.0f / 16777216.0f) < DRIFT_CHANCE * 8.0f) {
            v->drift_target = synth_rand_bipolar(&v->rng) * p->drift_cents * 0.01f;
        }
        v->drift += (v->drift_target - v->drift) * DRIFT_SMOOTH * 8.0f;
        semis += v->drift;
    }
    float hz = hz_of(semis);
    float f = hz / SYNTH_RATE;
    for (uint32_t i = 0; i < SYNTH_UNISON_MAX; i++) {
        float fi = f * v->ratio[i];
        c->inc[i] = (uint32_t)(synth_clamp(fi, 0.0f, 0.49f) * 4294967296.0f);
    }

    /* Wavetable position. */
    float wave = p->wave + p->wave_pressure * v->pressure_s + p->wave_slide * v->slide_s + p->wave_env * mod +
                 p->wave_lfo * lfo;
    wave = synth_clamp(wave, 0.0f, 1.0f) * (float)(TILES_WT_FRAMES - 1u);
    c->wave_from = v->wave_pos;
    c->wave_step = (wave - v->wave_pos) / (float)SYNTH_CTRL;
    v->wave_pos = wave;

    /* Filter */
    float oct = p->env_oct * mod + p->velocity_oct * in->velocity + p->pressure_oct * v->pressure_s +
                p->slide_oct * v->slide_s + p->lfo_oct * lfo + p->key_track * (semis - 60.0f) / 12.0f;
    float cutoff = synth_clamp(p->cutoff_hz * synth_exp2(synth_clamp(oct, -12.0f, 12.0f)), 20.0f, 20000.0f);
    if (p->filter == SYNTH_FILTER_LADDER24) {
        synth_ladder_set(&v->ladder[0], cutoff, p->resonance);
        float s[4];
        memcpy(s, v->ladder[1].s, sizeof(s));
        v->ladder[1] = v->ladder[0];
        memcpy(v->ladder[1].s, s, sizeof(s));
    } else {
        synth_svf_set(&v->svf[0], cutoff, p->resonance);
        float ic1 = v->svf[1].ic1, ic2 = v->svf[1].ic2;
        v->svf[1] = v->svf[0];
        v->svf[1].ic1 = ic1;
        v->svf[1].ic2 = ic2;
    }

    /* Level: envelope, velocity, pressure. */
    float vel = 1.0f - p->velocity_level + p->velocity_level * in->velocity * in->velocity;
    float target = amp * p->level * vel * (1.0f + p->pressure_level * v->pressure_s);
    c->level_from = v->level;
    c->level_step = (target - v->level) / (float)SYNTH_CTRL;
    v->level = target;
    c->drive = 1.0f + p->drive;
}

void TILES_AUDIO_HOT(synth_voice_render)(synth_voice_t *v, const synth_context_t *ctx, const synth_input_t *in,
                                         float *l, float *r, uint32_t n) {
    const synth_patch_t *p = v->patch;
    const tiles_wt_bank_t *bank = ctx->banks[p->bank < TILES_WT_BANK_COUNT ? p->bank : 0u];
    if (bank == NULL) {
        return;
    }
    const uint32_t copies = p->unison < 1u ? 1u : p->unison > SYNTH_UNISON_MAX ? SYNTH_UNISON_MAX : p->unison;
    const int mode = p->filter == SYNTH_FILTER_BP12 ? SYNTH_SVF_BP : p->filter == SYNTH_FILTER_HP12 ? SYNTH_SVF_HP
                                                                                                    : SYNTH_SVF_LP;
    const bool ladder = p->filter == SYNTH_FILTER_LADDER24;
    const bool shaped = p->drive > 0.0f || p->fold > 0.0f;
    const bool stereo = copies > 1u && p->width > 0.0f;
    const float gain = bank->gain;

    for (uint32_t base = 0; base < n; base += SYNTH_CTRL) {
        ctrl_t c;
        control(v, ctx, in, &c);
        /* The copy to read: the top unison copy decides. */
        float f_max = (float)c.inc[0] * (1.0f / 4294967296.0f);
        for (uint32_t i = 1; i < copies; i++) {
            float fi = (float)c.inc[i] * (1.0f / 4294967296.0f);
            f_max = fi > f_max ? fi : f_max;
        }
        uint32_t level = tiles_wt_level(f_max);
        uint32_t bits = tiles_wt_level_bits(level);
        uint32_t shift = 32u - bits;
        float frac_scale = 1.0f / (float)(1u << shift);
        uint32_t offset = tiles_wt_level_offset(level);
        float wave = c.wave_from;
        float lvl = c.level_from;
        for (uint32_t s = 0; s < SYNTH_CTRL; s++) {
            int32_t fi = (int32_t)wave;
            if (fi > (int32_t)TILES_WT_FRAMES - 2) {
                fi = (int32_t)TILES_WT_FRAMES - 2;
            }
            float ff = wave - (float)fi;
            const int16_t *t0 = &bank->data[fi][offset];
            const int16_t *t1 = &bank->data[fi + 1][offset];
            float sl = 0.0f, sr = 0.0f;
            for (uint32_t u = 0; u < copies; u++) {
                uint32_t ph = v->phase[u];
                uint32_t idx = ph >> shift;
                float fr = (float)(ph & ((1u << shift) - 1u)) * frac_scale;
                float a = (float)t0[idx] + fr * (float)(t0[idx + 1u] - t0[idx]);
                float b = (float)t1[idx] + fr * (float)(t1[idx + 1u] - t1[idx]);
                float x = a + ff * (b - a);
                sl += x * v->gain_l[u];
                sr += x * v->gain_r[u];
                v->phase[u] = ph + c.inc[u];
            }
            sl *= gain;
            if (shaped) {
                sl = synth_fold(synth_sat(sl * c.drive), p->fold);
            }
            float yl = ladder ? synth_ladder_tick(&v->ladder[0], sl) : synth_svf_tick(&v->svf[0], sl, mode);
            float yr = yl;
            if (stereo) {
                sr *= gain;
                if (shaped) {
                    sr = synth_fold(synth_sat(sr * c.drive), p->fold);
                }
                yr = ladder ? synth_ladder_tick(&v->ladder[1], sr) : synth_svf_tick(&v->svf[1], sr, mode);
            }
            l[base + s] += yl * lvl;
            r[base + s] += yr * lvl;
            wave += c.wave_step;
            lvl += c.level_step;
        }
    }
}
