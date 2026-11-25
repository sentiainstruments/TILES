#include "fx.h"

#include "audio_hot.h"
#include "fx_memory.h"
#include "synth_dsp.h"

#include <string.h>

/* Our own code after published designs (docs/architecture/synth-engines.md,
 * "Effects"): J. Dattorro's plate (1997), the BBD ensembles' three
 * 120-degree taps, first-order antiderivative anti-aliasing (Parker,
 * Zavalishin, Le Bivic 2016) on the overdrive. */

#define INLINE static inline __attribute__((always_inline))
#define FS SYNTH_RATE
/* The plate and the delay run at half the rate: half their memory and
 * cycles; the delay's loop filter tops out at 12 kHz anyway and the
 * plate's damping rolls its highs off. Sends go down and returns come up
 * through a 7-tap half-band (-1 0 9 16 9 0 -1)/32. */
#define FS2 (SYNTH_RATE * 0.5f)

/* Delay memory holds 16-bit samples of the signal x 8192: +-4 of headroom,
 * a -78 dB floor (Mutable Instruments' reverbs store 12 bits). */
#define Q 8192.0f
#define INV_Q (1.0f / 8192.0f)

/* ---- Rings: every line of one effect in one circular buffer, one write
 * pointer moving down it; a line is a region at a fixed offset from it. */

typedef struct {
    int16_t *buf;
    uint32_t size, wp;
} ring_t;

INLINE uint32_t wrapi(uint32_t i, uint32_t size) {
    return i >= size ? i - size : i;
}

/* The plate and the delay work in their rings' own units (x Q): a read
 * is a load and a conversion, a write saturates with SSAT. */
#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
INLINE int16_t q16(float x) {
    return (int16_t)__ssat((int32_t)x, 16);
}
#else
INLINE int16_t q16(float x) {
    int32_t v = (int32_t)x;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
#endif

/* The plate's power-of-two rings. */
INLINE float rm(const int16_t *buf, uint32_t mask, uint32_t wp, uint32_t off) {
    return (float)buf[(wp + off) & mask];
}

INLINE void wm(int16_t *buf, uint32_t mask, uint32_t wp, uint32_t off, float x) {
    buf[(wp + off) & mask] = q16(x);
}

INLINE float rm_frac(const int16_t *buf, uint32_t mask, uint32_t wp, float off) {
    uint32_t i = (uint32_t)off;
    float f = off - (float)i;
    float a = rm(buf, mask, wp, i), b = rm(buf, mask, wp, i + 1u);
    return a + f * (b - a);
}

/* Schroeder allpass, (z^-d - g) / (1 - g z^-d): writes at w, reads at r = w + d. */
INLINE float apm(int16_t *buf, uint32_t mask, uint32_t wp, uint32_t w, uint32_t r, float g, float x) {
    float z = rm(buf, mask, wp, r);
    float v = x + g * z;
    wm(buf, mask, wp, w, v);
    return z - g * v;
}

/* ---- Plate reverb ---------------------------------------------------------
 * Dattorro's figure-eight tank at 24 kHz: four input diffusers,
 * then two halves (a modulated allpass, a delay, damping, an allpass, a
 * delay) feeding each other; seven taps a side for the stereo outputs.
 * Two power-of-two rings, so an access wraps with one AND (Mutable
 * Instruments' FxEngine does the same): the front (predelay, diffusers,
 * modulated allpasses) and the tank. */

#define REVERB_FRONT 4096u
#define REVERB_TANK 16384u
#define REVERB_PRE_MAX 1200u /* 50 ms */
#define REVERB_EXC_MAX 13u   /* his 16 samples of modulation, at 24 kHz */
#define REVERB_GAIN 0.6f

/* His lengths at 29,761 Hz, at 24 kHz: constants, so every access is an
 * add of an immediate and a mask. */
#define DAT(n) ((uint32_t)((double)(n) * (24000.0 / 29761.0) + 0.5))
/* The front ring: predelay, the four diffusers, the modulated allpasses. */
static const uint32_t PRE = 0u;
static const uint32_t IN0 = REVERB_PRE_MAX + 2u;
static const uint32_t IN1 = IN0 + DAT(142) + 2u;
static const uint32_t IN2 = IN1 + DAT(107) + 2u;
static const uint32_t IN3 = IN2 + DAT(379) + 2u;
static const uint32_t AP1L = IN3 + DAT(277) + 2u;
static const uint32_t AP1R = AP1L + DAT(672) + 2u * REVERB_EXC_MAX + 2u;
static const uint32_t FRONT_END = AP1R + DAT(908) + 2u * REVERB_EXC_MAX + 2u;
/* The tank ring: each half's delay, allpass, delay. */
static const uint32_t DEL1L = 0u;
static const uint32_t AP2L = DEL1L + DAT(4453) + 2u;
static const uint32_t DEL2L = AP2L + DAT(1800) + 2u;
static const uint32_t DEL1R = DEL2L + DAT(3720) + 2u;
static const uint32_t AP2R = DEL1R + DAT(4217) + 2u;
static const uint32_t DEL2R = AP2R + DAT(2656) + 2u;
static const uint32_t TANK_END = DEL2R + DAT(3163) + 2u;

typedef struct {
    int16_t *front, *tank;
    uint32_t wp;    /* both rings' write point, running down */
    uint32_t pre_d; /* predelay, samples (0: none) */
    float bw, damp[2], end[2];
    float decay, diff2, damp_a, exc, lfo_phase;
    uint32_t quiet; /* blocks with no input and no output */
} reverb_t;

/* ---- Ping-pong delay ------------------------------------------------------
 * The left line hears the input and the right line's echo, the right line
 * the left's: echoes alternate sides, each `feedback` down, through a band
 * filter and a soft clip in the loop; a slow wobble of a sample or two
 * keeps repeats from ringing metallic. 16-bit at 48 kHz, 0.5 s a side. */

#define DELAY_MAX 12000u /* 0.5 s at 24 kHz */
#define DELAY_RING (2u * (DELAY_MAX + 4u))
#define DELAY_WOW 0.75f /* samples */

typedef struct {
    ring_t ring;
    float time; /* samples, gliding to the tempo's */
    float lp[2], lo[2]; /* loop filter: low-pass, and the lows a high-pass takes out */
    float wow_phase;
    uint32_t quiet;
} delay_t;

/* ---- Ensemble -------------------------------------------------------------
 * Three taps on one line, each moved by a slow and a fast LFO 120 degrees
 * apart, as the string ensembles' bucket-brigade lines were. */

#define CHORUS_LEN 1024u /* 21 ms */
#define CHORUS_BASE 360.0f /* 7.5 ms */

typedef struct {
    float *buf;
    uint32_t wp;
    float slow, fast;   /* LFO phases, turns */
    float d[3];         /* each tap's delay at the end of the last block */
    uint32_t quiet;
} chorus_t;

/* A bus asleep after this many blocks of silence in and out (~0.5 s). */
#define QUIET_BLOCKS 1500u
#define QUIET_LEVEL 1e-5f

static tiles_fx_bus_t s_bus;
static reverb_t s_rv;
static delay_t s_dl;
static chorus_t s_ch;
static bool s_bypass = true;
static bool s_ready;
static bool s_delay_lent; /* the granular tape has the delay's line */
static uint32_t s_generation;
/* The half-band's history: the two sends going down, the two returns up. */
static float s_down_delay[6], s_down_reverb[6], s_up_l[3], s_up_r[3];

/* The pump: seconds since the last duck. */
static float s_pump_t = 100.0f;
static float s_dip_from, s_dip_to;
static float s_beat_s = 0.5f;

const tiles_fx_bus_t g_tiles_fx_bus_default = {
    .reverb_decay = 0.62f,
    .reverb_damping = 0.35f,
    .reverb_predelay_ms = 12.0f,
    .reverb_mod = 0.6f,
    .delay_sixteenths = 3.0f,
    .delay_feedback = 0.38f,
    .delay_tone = 0.55f,
    .delay_to_reverb = 0.25f,
    .chorus_rate = 0.5f,
    .chorus_depth = 0.6f,
    .pump_release = 0.7f,
};

TILES_AUDIO_COLD static void apply_bus(void) {
    s_rv.pre_d = (uint32_t)(synth_clamp(s_bus.reverb_predelay_ms, 0.0f, 50.0f) * (FS2 / 1000.0f));
    s_rv.pre_d = s_rv.pre_d > REVERB_PRE_MAX ? REVERB_PRE_MAX : s_rv.pre_d;
    s_rv.decay = 0.25f + 0.7f * synth_clamp(s_bus.reverb_decay, 0.0f, 1.0f);
    s_rv.diff2 = synth_clamp(s_rv.decay + 0.15f, 0.25f, 0.5f);
    /* The one-pole at 48 kHz (1 - 0.7 x damping), as one step at 24 kHz. */
    float d = 0.7f * synth_clamp(s_bus.reverb_damping, 0.0f, 1.0f);
    s_rv.damp_a = 1.0f - d * d;
    s_rv.exc = (float)REVERB_EXC_MAX * synth_clamp(s_bus.reverb_mod, 0.0f, 1.0f);
}

TILES_AUDIO_COLD void tiles_fx_init(void *memory) {
    if (!s_ready) {
        s_bus = g_tiles_fx_bus_default;
        s_ready = true;
    }
    memset(memory, 0, TILES_FX_MEMORY_BYTES);
    int16_t *m16 = (int16_t *)memory;
    memset(&s_rv, 0, sizeof(s_rv));
    s_rv.tank = m16;
    s_rv.front = m16 + REVERB_TANK;
    memset(&s_dl, 0, sizeof(s_dl));
    s_dl.ring = (ring_t){m16 + REVERB_TANK + REVERB_FRONT, DELAY_RING, 0u};
    memset(&s_ch, 0, sizeof(s_ch));
    s_ch.buf = (float *)(m16 + REVERB_TANK + REVERB_FRONT + DELAY_RING);
    for (uint32_t i = 0; i < 3u; i++) {
        s_ch.d[i] = CHORUS_BASE;
    }
    apply_bus();
    s_dl.time = -1.0f; /* takes the tempo's at once */
    memset(s_down_delay, 0, sizeof(s_down_delay));
    memset(s_down_reverb, 0, sizeof(s_down_reverb));
    memset(s_up_l, 0, sizeof(s_up_l));
    memset(s_up_r, 0, sizeof(s_up_r));
    s_bypass = false;
    s_delay_lent = false;
    s_generation++;
}

_Static_assert((REVERB_TANK + REVERB_FRONT + DELAY_RING) * 2u + CHORUS_LEN * 4u <= TILES_FX_MEMORY_BYTES, "fx memory");
_Static_assert(DAT(908) == 732u && DAT(3163) == 2551u, "the plate's lengths");
/* FRONT_END and TANK_END fit their rings: test_fx.c. */
uint32_t tiles_fx_reverb_extent(bool tank) {
    return tank ? TANK_END : FRONT_END;
}

TILES_AUDIO_COLD void tiles_fx_set_bus(const tiles_fx_bus_t *bus) {
    s_bus = *bus;
    s_ready = true;
    apply_bus();
}

const tiles_fx_bus_t *tiles_fx_bus(void) {
    if (!s_ready) {
        s_bus = g_tiles_fx_bus_default;
        s_ready = true;
    }
    return &s_bus;
}

void tiles_fx_set_bypass(bool bypass) {
    s_bypass = bypass;
}

uint32_t tiles_fx_generation(void) {
    return s_generation;
}

TILES_AUDIO_COLD int16_t *tiles_fx_delay_lend(uint32_t *samples) {
    if (s_bypass || s_dl.ring.buf == NULL) {
        return NULL;
    }
    s_delay_lent = true;
    *samples = s_dl.ring.size;
    return s_dl.ring.buf;
}

TILES_AUDIO_COLD void tiles_fx_delay_return(void) {
    if (!s_delay_lent || s_bypass) {
        return;
    }
    int16_t *buf = s_dl.ring.buf;
    uint32_t size = s_dl.ring.size;
    memset(buf, 0, size * sizeof(int16_t));
    memset(&s_dl, 0, sizeof(s_dl));
    s_dl.ring = (ring_t){buf, size, 0u};
    s_dl.time = -1.0f;
    s_delay_lent = false;
}

bool tiles_fx_delay_lent(void) {
    return s_delay_lent;
}

/* ---- The pump ---------------------------------------------------------- */

#define PUMP_ATTACK_S 0.0015f

void tiles_fx_pump_kick(void) {
    s_pump_t = 0.0f;
}

void tiles_fx_pump_beat(void) {
    if (s_pump_t > 0.5f * s_beat_s) {
        s_pump_t = 0.0f;
    }
}

/* Down within 1.5 ms (no click), back up along a smooth S over the release. */
static float pump_dip(float t, float release) {
    if (t < PUMP_ATTACK_S) {
        return t * (1.0f / PUMP_ATTACK_S);
    }
    float x = (t - PUMP_ATTACK_S) / release;
    if (x >= 1.0f) {
        return 0.0f;
    }
    float u = 1.0f - x;
    return u * u * (1.0f + 2.0f * x);
}

void tiles_fx_block_begin(uint32_t n, float beats_per_second) {
    s_beat_s = beats_per_second > 0.1f ? 1.0f / beats_per_second : 0.5f;
    float release = synth_clamp(s_bus.pump_release * s_beat_s, 0.08f, 0.6f);
    s_dip_from = s_dip_to;
    s_pump_t = s_pump_t < 100.0f ? s_pump_t + (float)n * (1.0f / FS) : s_pump_t;
    s_dip_to = pump_dip(s_pump_t, release);
}

/* ---- Inserts ------------------------------------------------------------- */

/* synth_sat's antiderivative: the polynomial's below the knee at 15/8,
 * then a straight line (synth_sat is flat at +-1 there). */
#define SAT_KNEE 1.875f
#define SAT_B (-0.18962963f)
#define SAT_D 0.016181728f
INLINE float sat_poly_F(float x2) {
    return x2 * (0.5f + x2 * (SAT_B * 0.25f + x2 * (SAT_D * (1.0f / 6.0f))));
}

INLINE float sat_F(float x) {
    float a = __builtin_fabsf(x);
    if (a >= SAT_KNEE) {
        return sat_poly_F(SAT_KNEE * SAT_KNEE) + (a - SAT_KNEE);
    }
    return sat_poly_F(x * x);
}

/* First-order ADAA: the mean of the shaper over the step from the last
 * input, (F(x) - F(x0)) / (x - x0); its midpoint value when the step is
 * too small to divide by. Both past the knee on one side: exactly +-1. */
INLINE float drive_sample(tiles_fx_insert_t *s, uint32_t ch, float x) {
    float x0 = s->x_prev[ch], f = sat_F(x), d = x - x0, y;
    if (__builtin_fabsf(x) >= SAT_KNEE && __builtin_fabsf(x0) >= SAT_KNEE && (x > 0.0f) == (x0 > 0.0f)) {
        y = x > 0.0f ? 1.0f : -1.0f;
    } else if (__builtin_fabsf(d) > 1e-3f) {
        y = (f - s->f_prev[ch]) / d;
    } else {
        y = synth_sat(0.5f * (x + x0));
    }
    s->x_prev[ch] = x;
    s->f_prev[ch] = f;
    return y;
}

void tiles_fx_insert_reset(tiles_fx_insert_t *s) {
    memset(s, 0, sizeof(*s));
}

void tiles_fx_insert(tiles_fx_insert_t *s, const tiles_fx_amounts_t *fx, float *l, float *r, uint32_t n) {
    if (fx->drive > 0.0f) {
        float drive = synth_clamp(fx->drive, 0.0f, 1.0f);
        float g = 1.0f + 24.0f * drive * drive;               /* up to +28 dB */
        float bias = 0.15f * drive;                           /* asymmetry: even harmonics */
        float makeup = 1.0f / __builtin_sqrtf(g);
        float fc = 18000.0f * synth_exp2(-3.6f * synth_clamp(fx->dark, 0.0f, 1.0f)); /* 18 kHz .. 1.5 kHz */
        float a = 1.0f - synth_exp2(-fc * (2.0f * SYNTH_PI / FS) * 1.442695f);
        float tl = s->tone[0], tr = s->tone[1];
        float dxl = s->dc_x[0], dyl = s->dc_y[0], dxr = s->dc_x[1], dyr = s->dc_y[1];
        for (uint32_t i = 0; i < n; i++) {
            float yl = drive_sample(s, 0u, l[i] * g + bias);
            float yr = drive_sample(s, 1u, r[i] * g + bias);
            float ol = yl - dxl + 0.9987f * dyl; /* DC block, ~10 Hz: the bias out */
            float or_ = yr - dxr + 0.9987f * dyr;
            dxl = yl;
            dyl = ol;
            dxr = yr;
            dyr = or_;
            tl += a * (ol - tl);
            tr += a * (or_ - tr);
            l[i] = tl * makeup;
            r[i] = tr * makeup;
        }
        s->tone[0] = tl;
        s->tone[1] = tr;
        s->dc_x[0] = dxl;
        s->dc_y[0] = dyl;
        s->dc_x[1] = dxr;
        s->dc_y[1] = dyr;
    }
    if (fx->crush > 0.0f) {
        float c = synth_clamp(fx->crush, 0.0f, 1.0f);
        float rate = synth_exp2(-4.5f * c);             /* 48 kHz .. 2.1 kHz */
        float q = synth_exp2(15.0f - 11.0f * c), inv_q = 1.0f / q; /* 16 bits .. 5 */
        for (uint32_t i = 0; i < n; i++) {
            s->crush_phase += rate;
            if (s->crush_phase >= 1.0f) {
                s->crush_phase -= 1.0f;
                s->hold[0] = __builtin_rintf(l[i] * q) * inv_q;
                s->hold[1] = __builtin_rintf(r[i] * q) * inv_q;
            }
            l[i] = s->hold[0];
            r[i] = s->hold[1];
        }
    }
    if (fx->pump > 0.0f) {
        float depth = synth_clamp(fx->pump, 0.0f, 1.0f);
        float g = 1.0f - depth * s_dip_from;
        float step = depth * (s_dip_from - s_dip_to) / (float)n;
        for (uint32_t i = 0; i < n; i++) {
            l[i] *= g;
            r[i] *= g;
            g += step;
        }
    }
}

/* ---- The shared effects ---------------------------------------------------- */

static bool silent(const float *x, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] != 0.0f) {
            return false;
        }
    }
    return true;
}

/* Asleep after a long silence in and out; any input wakes it. */
static bool awake(uint32_t *quiet, const float *in, uint32_t n) {
    if (!silent(in, n)) {
        *quiet = 0u;
    }
    return *quiet < QUIET_BLOCKS;
}

static void note_output(uint32_t *quiet, float peak) {
    if (peak < QUIET_LEVEL) {
        *quiet = *quiet < QUIET_BLOCKS ? *quiet + 1u : *quiet;
    } else {
        *quiet = 0u;
    }
}

static void chorus_run(const float *in, float *l, float *r, uint32_t n) {
    chorus_t *c = &s_ch;
    if (!awake(&c->quiet, in, n)) {
        return;
    }
    float depth = synth_clamp(s_bus.chorus_depth, 0.0f, 1.0f);
    float rate = synth_clamp(s_bus.chorus_rate, 0.0f, 1.0f);
    c->slow += (0.3f + 0.9f * rate) * (float)n * (1.0f / FS); /* 0.3 .. 1.2 Hz */
    c->fast += (4.5f + 3.0f * rate) * (float)n * (1.0f / FS); /* 4.5 .. 7.5 Hz */
    c->slow -= (float)(int32_t)c->slow;
    c->fast -= (float)(int32_t)c->fast;
    float from[3], step[3];
    for (uint32_t i = 0; i < 3u; i++) {
        float third = (float)i * (1.0f / 3.0f);
        float d = CHORUS_BASE + depth * (72.0f * synth_sin_turns(c->slow + third) + 6.0f * synth_sin_turns(c->fast + third));
        from[i] = c->d[i];
        step[i] = (d - from[i]) / (float)n;
        c->d[i] = d;
    }
    const uint32_t mask = CHORUS_LEN - 1u;
    float peak = 0.0f;
    for (uint32_t s = 0; s < n; s++) {
        c->buf[c->wp] = in[s];
        float t[3];
        for (uint32_t i = 0; i < 3u; i++) {
            float d = from[i];
            from[i] += step[i];
            uint32_t k = (uint32_t)d;
            float f = d - (float)k;
            float a = c->buf[(c->wp + k) & mask], b = c->buf[(c->wp + k + 1u) & mask];
            t[i] = a + f * (b - a);
        }
        float yl = 0.66f * (t[0] + 0.5f * t[1]);
        float yr = 0.66f * (t[2] + 0.5f * t[1]);
        l[s] += yl;
        r[s] += yr;
        peak = synth_clamp(__builtin_fabsf(yl) + __builtin_fabsf(yr), peak, 1e9f);
        c->wp = (c->wp - 1u) & mask;
    }
    note_output(&c->quiet, peak);
}

/* The echo time for the tempo, halved until it fits the line, less the
 * half-band's way down and up (9 samples at 48 kHz, measured), so
 * echoes land on the beat. */
#define RESAMPLE_LAG 4.5f
static float delay_target(float beats_per_second) {
    float sixteenths = synth_clamp(s_bus.delay_sixteenths, 0.25f, 16.0f);
    float t = sixteenths * 0.25f / (beats_per_second > 0.1f ? beats_per_second : 2.0f) * FS2;
    while (t > (float)(DELAY_MAX - 4u) - DELAY_WOW) {
        t *= 0.5f;
    }
    return t > 2.0f * RESAMPLE_LAG ? t - RESAMPLE_LAG : t;
}

static void delay_run(const float *in, float *l, float *r, float *to_reverb, uint32_t n, float beats_per_second) {
    delay_t *d = &s_dl;
    if (s_delay_lent || !awake(&d->quiet, in, n)) {
        return;
    }
    float target = delay_target(beats_per_second);
    if (d->time < 0.0f) {
        d->time = target;
    }
    /* Glides to a new tempo's time over ~70 ms, tape-like, never a jump. */
    float t_from = d->time, t_to = d->time + (target - d->time) * 0.2f;
    d->time = t_to;
    d->wow_phase += 0.35f * (float)n * (1.0f / FS2);
    d->wow_phase -= (float)(int32_t)d->wow_phase;
    float wow = DELAY_WOW * (1.0f + synth_sin_turns(d->wow_phase));
    float fb = synth_clamp(s_bus.delay_feedback, 0.0f, 0.95f);
    float lp_a = 1.0f - synth_exp2(-(2500.0f * synth_exp2(2.3f * synth_clamp(s_bus.delay_tone, 0.0f, 1.0f))) *
                                   (2.0f * SYNTH_PI / FS2) * 1.442695f); /* 2.5 .. 12 kHz */
    const float hp_a = 1.0f - 0.98442f * 0.98442f;                       /* ~120 Hz */
    const float send = synth_clamp(s_bus.delay_to_reverb, 0.0f, 1.0f);
    const uint32_t base_r = DELAY_MAX + 4u;
    int16_t *const buf = d->ring.buf;
    const uint32_t size = d->ring.size;
    uint32_t wp = d->ring.wp;
    float lp0 = d->lp[0], lp1 = d->lp[1], lo0 = d->lo[0], lo1 = d->lo[1];
    const float half_send = 0.5f * send;
    float peak = 0.0f;
    const float t_step = (t_to - t_from) / (float)n;
    float t = t_from + wow;
    /* In the ring's units (x Q); SSAT bounds the loop (feedback is at most
     * 0.95 and the filters never gain). */
    for (uint32_t s = 0; s < n; s++, t += t_step) {
        uint32_t i = (uint32_t)t;
        float f = t - (float)i;
        uint32_t a = wp + i, b = a + base_r;
        float la = buf[wrapi(a, size)], lb = buf[wrapi(a + 1u, size)];
        float ra = buf[wrapi(b, size)], rb = buf[wrapi(b + 1u, size)];
        float ol = la + f * (lb - la), or_ = ra + f * (rb - ra);
        lp0 += lp_a * (or_ - lp0); /* the left line hears the right's echo */
        lo0 += hp_a * (lp0 - lo0);
        lp1 += lp_a * (ol - lp1);
        lo1 += hp_a * (lp1 - lo1);
        buf[wp] = q16(in[s] * Q + fb * (lp0 - lo0));
        buf[wrapi(wp + base_r, size)] = q16(fb * (lp1 - lo1));
        ol *= INV_Q;
        or_ *= INV_Q;
        l[s] += ol;
        r[s] += or_;
        to_reverb[s] += half_send * (ol + or_);
        peak = synth_clamp(__builtin_fabsf(ol) + __builtin_fabsf(or_), peak, 1e9f);
        wp = wp == 0u ? size - 1u : wp - 1u;
    }
    d->ring.wp = wp;
    d->lp[0] = lp0;
    d->lp[1] = lp1;
    d->lo[0] = lo0;
    d->lo[1] = lo1;
    note_output(&d->quiet, peak);
}

#define FM (REVERB_FRONT - 1u)
#define TM (REVERB_TANK - 1u)

static void reverb_run(const float *in, float *l, float *r, uint32_t n) {
    reverb_t *rv = &s_rv;
    if (!awake(&rv->quiet, in, n)) {
        return;
    }
    rv->lfo_phase += 0.8f * (float)n * (1.0f / FS2);
    rv->lfo_phase -= (float)(int32_t)rv->lfo_phase;
    int16_t *const fr = rv->front, *const tk = rv->tank;
    uint32_t wp = rv->wp;
    /* The tank's two modulated allpasses, in quadrature, 0 .. 2 x excursion. */
    const float ap1l_r = (float)(AP1L + DAT(672)) + rv->exc * (1.0f + synth_sin_turns(rv->lfo_phase));
    const float ap1r_r = (float)(AP1R + DAT(908)) + rv->exc * (1.0f + synth_sin_turns(rv->lfo_phase + 0.25f));
    const float decay = rv->decay, diff2 = rv->diff2, damp_a = rv->damp_a;
    const float out_gain = REVERB_GAIN * INV_Q;
    const uint32_t pre_r = PRE + rv->pre_d;
    const bool pre = rv->pre_d > 0u;
    float bw = rv->bw, damp0 = rv->damp[0], damp1 = rv->damp[1], end0 = rv->end[0], end1 = rv->end[1];
    float peak = 0.0f;
    for (uint32_t s = 0; s < n; s++) {
        float x = in[s] * Q;
        if (pre) {
            wm(fr, FM, wp, PRE, x);
            x = rm(fr, FM, wp, pre_r);
        }
        bw += 0.9775f * (x - bw); /* input bandwidth: Dattorro's, as one 24 kHz step of 0.85 */
        x = apm(fr, FM, wp, IN0, IN0 + DAT(142), 0.75f, bw);
        x = apm(fr, FM, wp, IN1, IN1 + DAT(107), 0.75f, x);
        x = apm(fr, FM, wp, IN2, IN2 + DAT(379), 0.625f, x);
        x = apm(fr, FM, wp, IN3, IN3 + DAT(277), 0.625f, x);
        float into0 = x + decay * end1, into1 = x + decay * end0;
        /* Each half: decay diffusion 1 (an allpass of -0.70, its length
         * moving), a delay, damping, decay diffusion 2, a delay. */
        float z = rm_frac(fr, FM, wp, ap1l_r);
        float v = into0 - 0.70f * z;
        wm(fr, FM, wp, AP1L, v);
        wm(tk, TM, wp, DEL1L, z + 0.70f * v);
        damp0 += damp_a * (rm(tk, TM, wp, DEL1L + DAT(4453)) - damp0);
        float y = apm(tk, TM, wp, AP2L, AP2L + DAT(1800), diff2, damp0 * decay);
        wm(tk, TM, wp, DEL2L, y);
        end0 = rm(tk, TM, wp, DEL2L + DAT(3720));

        z = rm_frac(fr, FM, wp, ap1r_r);
        v = into1 - 0.70f * z;
        wm(fr, FM, wp, AP1R, v);
        wm(tk, TM, wp, DEL1R, z + 0.70f * v);
        damp1 += damp_a * (rm(tk, TM, wp, DEL1R + DAT(4217)) - damp1);
        y = apm(tk, TM, wp, AP2R, AP2R + DAT(2656), diff2, damp1 * decay);
        wm(tk, TM, wp, DEL2R, y);
        end1 = rm(tk, TM, wp, DEL2R + DAT(3163));

        /* His table 2: seven taps a side, mirrored. */
        float ol = out_gain * (rm(tk, TM, wp, DEL1R + DAT(266)) + rm(tk, TM, wp, DEL1R + DAT(2974)) -
                               rm(tk, TM, wp, AP2R + DAT(1913)) + rm(tk, TM, wp, DEL2R + DAT(1996)) -
                               rm(tk, TM, wp, DEL1L + DAT(1990)) - rm(tk, TM, wp, AP2L + DAT(187)) -
                               rm(tk, TM, wp, DEL2L + DAT(1066)));
        float or_ = out_gain * (rm(tk, TM, wp, DEL1L + DAT(353)) + rm(tk, TM, wp, DEL1L + DAT(3627)) -
                                rm(tk, TM, wp, AP2L + DAT(1228)) + rm(tk, TM, wp, DEL2L + DAT(2673)) -
                                rm(tk, TM, wp, DEL1R + DAT(2111)) - rm(tk, TM, wp, AP2R + DAT(335)) -
                                rm(tk, TM, wp, DEL2R + DAT(121)));
        l[s] += ol;
        r[s] += or_;
        peak = synth_clamp(__builtin_fabsf(ol) + __builtin_fabsf(or_), peak, 1e9f);
        wp--;
    }
    rv->wp = wp;
    rv->bw = bw;
    rv->damp[0] = damp0;
    rv->damp[1] = damp1;
    rv->end[0] = end0;
    rv->end[1] = end1;
    note_output(&rv->quiet, peak);
}

/* 48 kHz to 24: n (even) in, n / 2 out; `h` the last 6 inputs. */
static void down(const float *in, uint32_t n, float *out, float *h) {
    float b[6u + TILES_FX_BLOCK_MAX];
    memcpy(b, h, 6u * sizeof(float));
    memcpy(&b[6], in, n * sizeof(float));
    for (uint32_t m = 0; m < n / 2u; m++) {
        const float *x = &b[2u * m + 1u];
        out[m] = (16.0f * x[3] + 9.0f * (x[2] + x[4]) - (x[0] + x[6])) * (1.0f / 32.0f);
    }
    memcpy(h, &b[n], 6u * sizeof(float));
}

/* 24 kHz to 48: n in, 2n added into out; `h` the last 3 inputs. */
static void up(const float *in, uint32_t n, float *out, float *h) {
    float c[3u + TILES_FX_BLOCK_MAX / 2u];
    memcpy(c, h, 3u * sizeof(float));
    memcpy(&c[3], in, n * sizeof(float));
    for (uint32_t m = 0; m < n; m++) {
        const float *x = &c[m];
        out[2u * m] += x[1];
        out[2u * m + 1u] += (9.0f * (x[1] + x[2]) - (x[0] + x[3])) * (1.0f / 16.0f);
    }
    memcpy(h, &c[n], 3u * sizeof(float));
}

void tiles_fx_busses(const float *chorus_in, const float *delay_in, const float *reverb_in, float *l, float *r,
                     uint32_t n, float beats_per_second) {
    if (s_bypass || n > TILES_FX_BLOCK_MAX || (n & 1u) != 0u) {
        return;
    }
    const uint32_t half = n / 2u;
    float dl[TILES_FX_BLOCK_MAX / 2u], rv[TILES_FX_BLOCK_MAX / 2u];
    float ol[TILES_FX_BLOCK_MAX / 2u] = {0}, or_[TILES_FX_BLOCK_MAX / 2u] = {0};
    down(delay_in, n, dl, s_down_delay);
    down(reverb_in, n, rv, s_down_reverb);
    delay_run(dl, ol, or_, rv, half, beats_per_second);
    reverb_run(rv, ol, or_, half);
    up(ol, half, l, s_up_l);
    up(or_, half, r, s_up_r);
    chorus_run(chorus_in, l, r, n);
}

TILES_AUDIO_COLD uint32_t tiles_fx_bench(uint32_t which, uint32_t frames, uint32_t (*cycles)(void)) {
    if (s_bypass) {
        return 0u; /* the memory is the pad capture's */
    }
    float in[TILES_FX_BLOCK_MAX], l[TILES_FX_BLOCK_MAX], r[TILES_FX_BLOCK_MAX], rev[TILES_FX_BLOCK_MAX];
    tiles_fx_amounts_t a;
    memset(&a, 0, sizeof(a));
    a.drive = which == TILES_FX_BENCH_DRIVE ? 0.5f : 0.0f;
    a.crush = which == TILES_FX_BENCH_CRUSH ? 0.5f : 0.0f;
    a.pump = which == TILES_FX_BENCH_PUMP ? 0.5f : 0.0f;
    tiles_fx_insert_t ins;
    tiles_fx_insert_reset(&ins);
    uint32_t rng = 0x1234567u;
    uint64_t total = 0;
    uint32_t done = 0;
    while (done < frames) {
        for (uint32_t i = 0; i < TILES_FX_BLOCK_MAX; i++) {
            in[i] = 0.1f * synth_rand_bipolar(&rng);
            l[i] = r[i] = in[i];
            rev[i] = 0.0f;
        }
        uint32_t t0 = cycles();
        switch (which) {
        case TILES_FX_BENCH_CHORUS:
            chorus_run(in, l, r, TILES_FX_BLOCK_MAX);
            break;
        case TILES_FX_BENCH_DELAY: /* at its half rate: a block's half */
            delay_run(in, l, r, rev, TILES_FX_BLOCK_MAX / 2u, 2.0f);
            break;
        case TILES_FX_BENCH_REVERB:
            reverb_run(in, l, r, TILES_FX_BLOCK_MAX / 2u);
            break;
        default:
            tiles_fx_block_begin(TILES_FX_BLOCK_MAX, 2.0f);
            tiles_fx_insert(&ins, &a, l, r, TILES_FX_BLOCK_MAX);
            break;
        }
        total += cycles() - t0;
        done += TILES_FX_BLOCK_MAX;
    }
    tiles_fx_init(s_rv.tank); /* its noise out of the rings, or the next stream starts with it */
    return (uint32_t)(total / done);
}
