#include "fm_voice.h"

#include "audio_hot.h"
#include "synth_dsp.h"

#include <stddef.h>
#include <string.h>

/* Our own code after the DX7's published behaviour (docs/architecture/
 * synth-engines.md, "FM"): its 32 algorithms, the 0.75 dB level steps,
 * envelope rates and curves, keyboard scaling, velocity, feedback, LFO and
 * pitch envelope tables, as measured from the instrument. */

#define INLINE static inline __attribute__((always_inline))
#define FS 48000.0f
#define Q24 16777216
#define BLOCK_MAX 16u
/* An operator below this gain (Q24, ~-84 dB) isn't computed. */
#define LEVEL_THRESHOLD 1120
/* The envelope tables are for 44.1 kHz. */
#define RATE_44K1 (44100.0f / FS)
/* A carrier at full output gives +-2: the mix sees +-0.5. */
#define OUT_SCALE (0.25f / (float)Q24)
#define INPUT_SMOOTH 0.12f

/* ---- The algorithms ----------------------------------------------------
 * Operator 6 first: which bus it reads (IN1/IN2, none), writes (OUT1/OUT2,
 * else the output), whether it adds there, and FB for the operator with
 * feedback. Algorithms 4 and 6 loop through three operators on the DX7;
 * here operator 6 feeds back on itself, as in other emulations. */
enum { OUT1 = 0x01, OUT2 = 0x02, ADD = 0x04, IN1 = 0x10, IN2 = 0x20, FB = 0xC0 };

static const uint8_t k_alg[32][6] = {
    {FB | OUT1, IN1 | OUT1, IN1 | OUT1, IN1 | ADD, OUT1, IN1 | ADD},               /* 1 */
    {OUT1, IN1 | OUT1, IN1 | OUT1, IN1 | ADD, FB | OUT1, IN1 | ADD},               /* 2 */
    {FB | OUT1, IN1 | OUT1, IN1 | ADD, OUT1, IN1 | OUT1, IN1 | ADD},               /* 3 */
    {FB | OUT1, IN1 | OUT1, IN1 | ADD, OUT1, IN1 | OUT1, IN1 | ADD},               /* 4 */
    {FB | OUT1, IN1 | ADD, OUT1, IN1 | ADD, OUT1, IN1 | ADD},                      /* 5 */
    {FB | OUT1, IN1 | ADD, OUT1, IN1 | ADD, OUT1, IN1 | ADD},                      /* 6 */
    {FB | OUT1, IN1 | OUT1, OUT1 | ADD, IN1 | ADD, OUT1, IN1 | ADD},               /* 7 */
    {OUT1, IN1 | OUT1, FB | OUT1 | ADD, IN1 | ADD, OUT1, IN1 | ADD},               /* 8 */
    {OUT1, IN1 | OUT1, OUT1 | ADD, IN1 | ADD, FB | OUT1, IN1 | ADD},               /* 9 */
    {OUT1, OUT1 | ADD, IN1 | ADD, FB | OUT1, IN1 | OUT1, IN1 | ADD},               /* 10 */
    {FB | OUT1, OUT1 | ADD, IN1 | ADD, OUT1, IN1 | OUT1, IN1 | ADD},               /* 11 */
    {OUT1, OUT1 | ADD, OUT1 | ADD, IN1 | ADD, FB | OUT1, IN1 | ADD},               /* 12 */
    {FB | OUT1, OUT1 | ADD, OUT1 | ADD, IN1 | ADD, OUT1, IN1 | ADD},               /* 13 */
    {FB | OUT1, OUT1 | ADD, IN1 | OUT1, IN1 | ADD, OUT1, IN1 | ADD},               /* 14 */
    {OUT1, OUT1 | ADD, IN1 | OUT1, IN1 | ADD, FB | OUT1, IN1 | ADD},               /* 15 */
    {FB | OUT1, IN1 | OUT1, OUT2, IN2 | OUT1 | ADD, OUT1 | ADD, IN1 | ADD},        /* 16 */
    {OUT1, IN1 | OUT1, OUT2, IN2 | OUT1 | ADD, FB | OUT1 | ADD, IN1 | ADD},        /* 17 */
    {OUT1, IN1 | OUT1, IN1 | OUT1, FB | OUT1 | ADD, OUT1 | ADD, IN1 | ADD},        /* 18 */
    {FB | OUT1, IN1 | ADD, IN1 | ADD, OUT1, IN1 | OUT1, IN1 | ADD},                /* 19 */
    {OUT1, OUT1 | ADD, IN1 | ADD, FB | OUT1, IN1 | ADD, IN1 | ADD},                /* 20 */
    {OUT1, IN1 | ADD, IN1 | ADD, FB | OUT1, IN1 | ADD, IN1 | ADD},                 /* 21 */
    {FB | OUT1, IN1 | ADD, IN1 | ADD, IN1 | ADD, OUT1, IN1 | ADD},                 /* 22 */
    {FB | OUT1, IN1 | ADD, IN1 | ADD, OUT1, IN1 | ADD, ADD},                       /* 23 */
    {FB | OUT1, IN1 | ADD, IN1 | ADD, IN1 | ADD, ADD, ADD},                        /* 24 */
    {FB | OUT1, IN1 | ADD, IN1 | ADD, ADD, ADD, ADD},                              /* 25 */
    {FB | OUT1, OUT1 | ADD, IN1 | ADD, OUT1, IN1 | ADD, ADD},                      /* 26 */
    {OUT1, OUT1 | ADD, IN1 | ADD, FB | OUT1, IN1 | ADD, ADD},                      /* 27 */
    {ADD, FB | OUT1, IN1 | OUT1, IN1 | ADD, OUT1, IN1 | ADD},                      /* 28 */
    {FB | OUT1, IN1 | ADD, OUT1, IN1 | ADD, ADD, ADD},                             /* 29 */
    {ADD, FB | OUT1, IN1 | OUT1, IN1 | ADD, ADD, ADD},                             /* 30 */
    {FB | OUT1, IN1 | ADD, ADD, ADD, ADD, ADD},                                    /* 31 */
    {FB | ADD, ADD, ADD, ADD, ADD, ADD},                                           /* 32 */
};

INLINE bool is_carrier(uint8_t flags) {
    return (flags & 3u) == 0u;
}

/* ---- The DX7's tables -------------------------------------------------- */

/* Output and envelope levels below 20 (above: 28 + level). */
static const uint8_t k_level_low[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
/* Velocity's curve, every other velocity. */
static const uint8_t k_velocity[64] = {
    0,   70,  86,  97,  106, 114, 121, 126, 132, 138, 142, 148, 152, 156, 160, 163, 166, 170, 173, 174, 178, 181,
    184, 186, 189, 190, 194, 196, 198, 200, 202, 205, 206, 209, 211, 214, 216, 218, 220, 222, 224, 225, 227, 229,
    230, 232, 233, 235, 237, 238, 240, 241, 242, 243, 244, 246, 246, 248, 249, 250, 251, 252, 253, 254};
/* Keyboard level scaling's exponential curve, by groups of three notes. */
static const uint8_t k_exp_scale[33] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,   11,  14,  16,  19,  23,  27, 33,
                                        39, 47, 56, 66, 80, 94, 110, 126, 142, 158, 174, 190, 206, 222, 238, 250};
static const uint8_t k_pitch_mod_sens[8] = {0, 10, 20, 33, 55, 92, 153, 255};
/* Amp mod sensitivity 0-3, Q24 fractions. */
static const float k_amp_mod_sens[4] = {0.0f, 0.2588f, 0.4274f, 1.0f};
/* LFO speeds 0-99, Hz. */
static const float k_lfo_hz[100] = {
    0.062541f, 0.125031f, 0.312393f, 0.437120f, 0.624610f, 0.750694f, 0.936330f, 1.125302f, 1.249609f, 1.436782f,
    1.560915f, 1.752081f, 1.875117f, 2.062494f, 2.247191f, 2.374451f, 2.560492f, 2.686728f, 2.873976f, 2.998950f,
    3.188013f, 3.369840f, 3.500175f, 3.682224f, 3.812065f, 4.000800f, 4.186202f, 4.310716f, 4.501260f, 4.623209f,
    4.814636f, 4.930480f, 5.121901f, 5.315191f, 5.434783f, 5.617346f, 5.750431f, 5.946717f, 6.062811f, 6.248438f,
    6.431695f, 6.564264f, 6.749460f, 6.868132f, 7.052186f, 7.250580f, 7.375719f, 7.556294f, 7.687577f, 7.877738f,
    7.993605f, 8.181967f, 8.372405f, 8.504848f, 8.685079f, 8.810573f, 8.986341f, 9.122423f, 9.300595f, 9.500285f,
    9.607994f, 9.798158f, 9.950249f, 10.117361f, 11.251125f, 11.384335f, 12.562814f, 13.676149f, 13.904338f,
    15.092062f, 16.366612f, 16.638935f, 17.869907f, 19.193858f, 19.425019f, 20.833333f, 21.034918f, 22.502250f,
    24.003841f, 24.260068f, 25.746653f, 27.173913f, 27.578599f, 29.052876f, 30.693677f, 31.191516f, 32.658393f,
    34.317090f, 34.674064f, 36.416606f, 38.197097f, 38.550501f, 40.387722f, 40.749796f, 42.625746f, 44.326241f,
    44.883303f, 46.772685f, 48.590865f, 49.261084f};
/* Pitch envelope: rates 0-99 (steps of 1/21.3 octave a second) and levels
 * 0-99 (128 = four octaves; 50 = none). */
static const uint8_t k_peg_rate[100] = {
    1,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13, 14,
    14, 15, 16, 16, 17, 18, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 31, 33, 34, 36, 37, 38, 39,
    41, 42, 44, 46, 47, 49, 51, 53, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 79, 82, 85, 88, 91,
    94, 98, 102, 106, 110, 115, 120, 125, 130, 135, 141, 147, 153, 159, 165, 171, 178, 185, 193, 202, 211, 232, 243, 254, 255};
static const int8_t k_peg_level[100] = {
    -128, -116, -104, -95, -85, -76, -68, -61, -56, -52, -49, -46, -43, -41, -39, -37, -35, -33, -32, -31,
    -30,  -29,  -28,  -27, -26, -25, -24, -23, -22, -21, -20, -19, -18, -17, -16, -15, -14, -13, -12, -11,
    -10,  -9,   -8,   -7,  -6,  -5,  -4,  -3,  -2,  -1,  0,   1,   2,   3,   4,   5,   6,   7,   8,   9,
    10,   11,   12,   13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
    30,   31,   32,   33,  34,  35,  38,  40,  43,  46,  49,  53,  58,  65,  73,  82,  92,  103, 115, 127};

/* The sine, 12 bits of phase, 16-bit amplitude: the DX7's own resolution. */
#define SINE_BITS 12u
static int16_t s_sine[1u << SINE_BITS];
/* 2^(i/64), Q30: the gains from the envelopes' log levels. */
static uint32_t s_exp2_q30[65];

TILES_AUDIO_COLD void tiles_fm_init(void) {
    for (uint32_t i = 0; i < (1u << SINE_BITS); i++) {
        s_sine[i] = (int16_t)(32767.0f * sinf(2.0f * SYNTH_PI * ((float)i + 0.5f) / (float)(1u << SINE_BITS)));
    }
    for (uint32_t i = 0; i <= 64u; i++) {
        s_exp2_q30[i] = (uint32_t)(synth_exp2((float)i / 64.0f) * 1073741824.0f + 0.5f);
    }
}

/* A frequency in Hz as a phase step (above 2^32, clamped: it aliases
 * anyway). */
static inline uint32_t to_inc(float hz) {
    float f = hz * (4294967296.0f / FS);
    return f < 4294967040.0f ? (uint32_t)f : 4294967040u;
}

/* 2^(level - 14 octaves) in Q24 (level: Q24 octaves; below 1 octave, 0):
 * the table, linearly interpolated (error ~1e-5). */
INLINE int32_t gain_of(int32_t level) {
    if (level < Q24) {
        return 0;
    }
    level = level < (20 << 24) ? level : (20 << 24) - 1;
    uint32_t f = (uint32_t)level & 0xFFFFFFu;
    uint32_t i = f >> 18, t = (f >> 10) & 0xFFu;
    uint32_t a = s_exp2_q30[i];
    uint32_t m = a + (((s_exp2_q30[i + 1u] - a) * t) >> 8);
    return (int32_t)(m >> (20 - (level >> 24)));
}

/* ---- Voice data ------------------------------------------------------- */

TILES_AUDIO_COLD void tiles_dx7_unpack(const uint8_t p[TILES_DX7_PACKED_BYTES], uint8_t v[TILES_DX7_VOICE_BYTES]) {
    for (uint32_t op = 0; op < 6u; op++) {
        const uint8_t *s = &p[op * 17u];
        uint8_t *d = &v[op * TILES_DX7_OP_BYTES];
        memcpy(d, s, 11u);              /* rates, levels, break point, depths */
        d[11] = s[11] & 3u;             /* left curve */
        d[12] = (s[11] >> 2) & 3u;      /* right curve */
        d[13] = s[12] & 7u;             /* rate scaling */
        d[14] = s[13] & 3u;             /* amp mod sensitivity */
        d[15] = (s[13] >> 2) & 7u;      /* velocity sensitivity */
        d[16] = s[14];                  /* output level */
        d[17] = s[15] & 1u;             /* mode */
        d[18] = (s[15] >> 1) & 31u;     /* coarse */
        d[19] = s[16];                  /* fine */
        d[20] = (s[12] >> 3) & 15u;     /* detune */
    }
    memcpy(&v[TILES_DX7_PEG_RATE], &p[102], 8u);
    v[TILES_DX7_ALGORITHM] = p[110] & 31u;
    v[TILES_DX7_FEEDBACK] = p[111] & 7u;
    v[TILES_DX7_OSC_SYNC] = (p[111] >> 3) & 1u;
    v[TILES_DX7_LFO_SPEED] = p[112];
    v[TILES_DX7_LFO_DELAY] = p[113];
    v[TILES_DX7_LFO_PMD] = p[114];
    v[TILES_DX7_LFO_AMD] = p[115];
    v[TILES_DX7_LFO_SYNC] = p[116] & 1u;
    v[TILES_DX7_LFO_WAVE] = (p[116] >> 1) & 7u;
    v[TILES_DX7_PITCH_MOD_SENS] = (p[116] >> 4) & 7u;
    v[TILES_DX7_TRANSPOSE] = p[117];
    memcpy(&v[TILES_DX7_NAME], &p[118], 10u);
}

TILES_AUDIO_COLD void tiles_dx7_pack(const uint8_t v[TILES_DX7_VOICE_BYTES], uint8_t p[TILES_DX7_PACKED_BYTES]) {
    for (uint32_t op = 0; op < 6u; op++) {
        const uint8_t *s = &v[op * TILES_DX7_OP_BYTES];
        uint8_t *d = &p[op * 17u];
        memcpy(d, s, 11u);
        d[11] = (uint8_t)((s[11] & 3u) | (s[12] & 3u) << 2);
        d[12] = (uint8_t)((s[13] & 7u) | (s[20] & 15u) << 3);
        d[13] = (uint8_t)((s[14] & 3u) | (s[15] & 7u) << 2);
        d[14] = s[16];
        d[15] = (uint8_t)((s[17] & 1u) | (s[18] & 31u) << 1);
        d[16] = s[19];
    }
    memcpy(&p[102], &v[TILES_DX7_PEG_RATE], 8u);
    p[110] = v[TILES_DX7_ALGORITHM] & 31u;
    p[111] = (uint8_t)((v[TILES_DX7_FEEDBACK] & 7u) | (v[TILES_DX7_OSC_SYNC] & 1u) << 3);
    p[112] = v[TILES_DX7_LFO_SPEED];
    p[113] = v[TILES_DX7_LFO_DELAY];
    p[114] = v[TILES_DX7_LFO_PMD];
    p[115] = v[TILES_DX7_LFO_AMD];
    p[116] = (uint8_t)((v[TILES_DX7_LFO_SYNC] & 1u) | (v[TILES_DX7_LFO_WAVE] & 7u) << 1 |
                       (v[TILES_DX7_PITCH_MOD_SENS] & 7u) << 4);
    p[117] = v[TILES_DX7_TRANSPOSE];
    memcpy(&p[118], &v[TILES_DX7_NAME], 10u);
}

TILES_AUDIO_COLD uint8_t tiles_fm_carriers(const uint8_t v[TILES_DX7_VOICE_BYTES]) {
    const uint8_t *alg = k_alg[v[TILES_DX7_ALGORITHM] & 31u];
    uint8_t n = 0;
    for (uint32_t k = 0; k < 6u; k++) {
        n += is_carrier(alg[k]) ? 1u : 0u;
    }
    return n;
}

TILES_AUDIO_COLD const uint8_t *tiles_dx7_bank_voices(const uint8_t *s, uint32_t bytes) {
    if (bytes < TILES_DX7_BANK_SYSEX_BYTES || s[0] != 0xF0u || s[1] != 0x43u || (s[2] & 0xF0u) != 0u ||
        s[3] != 0x09u || s[4] != 0x20u || s[5] != 0x00u || s[TILES_DX7_BANK_SYSEX_BYTES - 1u] != 0xF7u) {
        return NULL;
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < TILES_DX7_BANK_VOICES * TILES_DX7_PACKED_BYTES; i++) {
        sum += s[6u + i];
    }
    return ((128u - (sum & 127u)) & 127u) == s[6u + 4096u] ? &s[6] : NULL;
}

TILES_AUDIO_COLD void tiles_dx7_name(const uint8_t v[TILES_DX7_VOICE_BYTES], char name[11]) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 10u; i++) {
        char c = (char)v[TILES_DX7_NAME + i];
        name[i] = c >= 32 && c < 127 ? c : ' ';
        n = name[i] != ' ' ? i + 1u : n;
    }
    name[n] = '\0';
}

/* ---- Envelopes --------------------------------------------------------- */

static int32_t scale_level(int32_t level) {
    level = level < 0 ? 0 : level > 99 ? 99 : level;
    return level >= 20 ? 28 + level : k_level_low[level];
}

/* Stage ix's target and step (Q24 log2 units a sample at 44.1 kHz). */
static void env_stage(tiles_fm_env_t *e, const uint8_t *op, int32_t outlevel, int32_t rate_scaling, uint8_t ix) {
    e->ix = ix;
    if (ix >= 4u) {
        return;
    }
    int32_t actual = ((scale_level(op[4 + ix]) >> 1) << 6) + outlevel - 4256;
    actual = actual < 16 ? 16 : actual;
    e->target = actual << 16;
    e->rising = e->target > e->level;
    int32_t q = ((op[ix] * 41) >> 6) + rate_scaling;
    q = q > 63 ? 63 : q;
    e->inc = (int32_t)((float)((4 + (q & 3)) << (2 + (q >> 2))) * (RATE_44K1 * (float)BLOCK_MAX));
}

/* One block of n samples. Attack: up from at least -40 dB, faster the
 * quieter; decay: steady in dB. Stage 3 waits for the key to lift. */
INLINE void env_step(tiles_fm_env_t *e, const uint8_t *op, int32_t outlevel, int32_t rate_scaling, uint32_t n) {
    if (!(e->ix < 3u || (e->ix < 4u && !e->down))) {
        return;
    }
    int32_t inc = n == BLOCK_MAX ? e->inc : (e->inc * (int32_t)n) / (int32_t)BLOCK_MAX;
    if (e->rising) {
        const int32_t jump = 1716 << 16;
        e->level = e->level < jump ? jump : e->level;
        e->level += (((17 << 24) - e->level) >> 24) * inc;
        if (e->level >= e->target) {
            e->level = e->target;
            env_stage(e, op, outlevel, rate_scaling, (uint8_t)(e->ix + 1u));
        }
    } else {
        e->level -= inc;
        if (e->level <= e->target) {
            e->level = e->target;
            env_stage(e, op, outlevel, rate_scaling, (uint8_t)(e->ix + 1u));
        }
    }
}

static void peg_stage(tiles_fm_voice_t *v, uint8_t ix) {
    const uint8_t *d = v->patch->dx7;
    v->peg_ix = ix;
    if (ix >= 4u) {
        return;
    }
    v->peg_target = (int32_t)k_peg_level[d[TILES_DX7_PEG_LEVEL + ix] % 100u] << 19;
    v->peg_rising = v->peg_target > v->peg_level;
    v->peg_inc = (int32_t)((float)k_peg_rate[d[TILES_DX7_PEG_RATE + ix] % 100u] * ((float)Q24 / (21.3f * FS)));
}

static void peg_step(tiles_fm_voice_t *v, uint32_t n) {
    if (!(v->peg_ix < 3u || (v->peg_ix < 4u && !v->peg_down))) {
        return;
    }
    int32_t inc = v->peg_inc * (int32_t)n;
    if (v->peg_rising) {
        v->peg_level += inc;
        if (v->peg_level >= v->peg_target) {
            v->peg_level = v->peg_target;
            peg_stage(v, (uint8_t)(v->peg_ix + 1u));
        }
    } else {
        v->peg_level -= inc;
        if (v->peg_level <= v->peg_target) {
            v->peg_level = v->peg_target;
            peg_stage(v, (uint8_t)(v->peg_ix + 1u));
        }
    }
}

/* ---- Notes ------------------------------------------------------------- */

/* Keyboard level scaling, in output-level steps (0.75 dB). */
static int32_t scale_curve(int32_t group, int32_t depth, int32_t curve) {
    int32_t s;
    if (curve == 0 || curve == 3) {
        s = (group * depth * 329) >> 12; /* linear */
    } else {
        s = (k_exp_scale[group > 32 ? 32 : group] * depth * 329) >> 15;
    }
    return curve < 2 ? -s : s;
}

static int32_t key_level_scaling(int32_t note, const uint8_t *op) {
    int32_t offset = note - op[8] - 17;
    if (offset >= 0) {
        return scale_curve((offset + 1) / 3, op[10], op[12]);
    }
    return scale_curve(-(offset - 1) / 3, op[9], op[11]);
}

/* An operator's pitch apart from the note's, in octaves (its ratio, fine
 * and detune); a fixed operator's whole pitch, log2 Hz. */
static float op_pitch(const uint8_t *op, float note_oct) {
    int32_t coarse = op[18] & 31, fine = op[19] > 99 ? 99 : op[19], detune = op[20] > 14 ? 14 : op[20];
    if (op[17] != 0u) {
        float oct = 3.321928f * ((float)(coarse & 3) + (float)fine * 0.01f);
        return oct + (detune > 7 ? (float)(13457 * (detune - 7)) / (float)Q24 : 0.0f);
    }
    float oct = coarse == 0 ? -1.0f : __builtin_log2f((float)coarse);
    oct += __builtin_log2f(1.0f + 0.01f * (float)fine);
    /* Detune: a fraction of the log pitch, smaller higher up (measured on
     * the instrument). */
    oct += 0.0209f * synth_exp2(-0.396f * 1.442695f * note_oct) / 7.0f * note_oct * (float)(detune - 7);
    return oct;
}

void tiles_fm_voice_start(tiles_fm_voice_t *v, const tiles_fm_patch_t *patch, const tiles_fm_input_t *in,
                          float glide_ms, bool legato) {
    bool was = v->sounding && v->patch == patch;
    legato = legato && was;
    v->patch = patch;
    const uint8_t *d = patch->dx7;
    float note = in->note + (float)((int32_t)d[TILES_DX7_TRANSPOSE] - 24);
    if (!was || glide_ms <= 0.0f) {
        v->pitch = note;
        v->glide_coef = 1.0f;
    } else {
        v->glide_coef = 1.0f - synth_exp2(-1.442695f * 16.0f / (glide_ms * 0.001f * FS));
    }
    if (!was) {
        memset(v->fb, 0, sizeof(v->fb));
        v->level = 0.0f;
        v->pressure_s = in->pressure;
        v->slide_s = in->slide;
    }
    int32_t midi = (int32_t)(note + 0.5f);
    float note_oct = 8.781360f + (note - 69.0f) / 12.0f; /* log2 Hz */
    int32_t velocity = (int32_t)(synth_clamp(in->velocity, 0.0f, 1.0f) * 127.0f + 0.5f);
    for (uint32_t k = 0; k < 6u; k++) {
        const uint8_t *op = &d[k * TILES_DX7_OP_BYTES];
        tiles_fm_op_t *o = &v->op[k];
        int32_t out = scale_level(op[16]) + key_level_scaling(midi, op);
        out = (out > 127 ? 127 : out) << 5;
        int32_t vel = k_velocity[velocity >> 1] - 239;
        out += ((op[15] * vel + 7) >> 3) << 4;
        o->outlevel = out < 0 ? 0 : out;
        int32_t x = midi / 3 - 7;
        x = x < 0 ? 0 : x > 31 ? 31 : x;
        o->rate_scaling = (op[13] * x) >> 3;
        o->fixed = op[17] != 0u;
        o->ratio = synth_exp2(op_pitch(op, o->fixed ? 0.0f : note_oct));
        if (o->fixed) {
            o->inc = to_inc(o->ratio); /* op_pitch gave log2 Hz */
        }
        /* Audible only if a stage reaches 1 octave up from silence (gain_of). */
        o->silent = true;
        for (uint32_t ix = 0; ix < 4u; ix++) {
            o->silent = o->silent && ((scale_level(op[4 + ix]) >> 1) << 6) + o->outlevel - 4256 < 256;
        }
        if (!legato) {
            if (!was) {
                o->env.level = 0;
                o->gain = 0;
            }
            o->env.down = true;
            env_stage(&o->env, op, o->outlevel, o->rate_scaling, 0u);
            if (d[TILES_DX7_OSC_SYNC] != 0u || !was) {
                o->phase = 0u;
            }
        }
    }
    if (!legato) {
        v->peg_level = (int32_t)k_peg_level[d[TILES_DX7_PEG_LEVEL + 3] % 100u] << 19;
        v->peg_down = true;
        peg_stage(v, 0u);
        if (d[TILES_DX7_LFO_SYNC] != 0u || !was) {
            v->lfo_phase = 0u;
        }
        v->lfo_delay = 0u;
    }
    v->lfo_inc = (uint32_t)(k_lfo_hz[d[TILES_DX7_LFO_SPEED] % 100u] * (4294967296.0f / FS));
    /* The LFO's delay: waits, then fades in over as long again. */
    int32_t a = 99 - (d[TILES_DX7_LFO_DELAY] > 99 ? 99 : d[TILES_DX7_LFO_DELAY]);
    v->lfo_delay_inc = a == 99 ? 0xFFFFFFFFu : (uint32_t)(((16 + (a & 15)) << (1 + (a >> 4))) * (25190424.0f / FS));
    v->hz = 0.0f; /* the steps from the new ratios */
    for (uint32_t k = 0; k < 6u; k++) {
        v->op[k].gain_level = -1;
    }
    v->sounding = true;
}

void tiles_fm_voice_release(tiles_fm_voice_t *v) {
    const uint8_t *d = v->patch->dx7;
    for (uint32_t k = 0; k < 6u; k++) {
        tiles_fm_op_t *o = &v->op[k];
        if (o->env.down) {
            o->env.down = false;
            env_stage(&o->env, &d[k * TILES_DX7_OP_BYTES], o->outlevel, o->rate_scaling, 3u);
        }
    }
    if (v->peg_down) {
        v->peg_down = false;
        peg_stage(v, 3u);
    }
}

void tiles_fm_voice_kill(tiles_fm_voice_t *v) {
    for (uint32_t k = 0; k < 6u; k++) {
        tiles_fm_env_t *e = &v->op[k].env;
        e->down = false;
        e->ix = 3u;
        e->target = 16 << 16;
        e->rising = false;
        e->inc = (int32_t)((float)(7 << 17) * (RATE_44K1 * (float)BLOCK_MAX)); /* the fastest: ~5 ms */
    }
}

bool tiles_fm_voice_active(const tiles_fm_voice_t *v) {
    return v->sounding;
}

float tiles_fm_voice_level(const tiles_fm_voice_t *v) {
    if (!v->sounding) {
        return 0.0f;
    }
    const uint8_t *alg = k_alg[v->patch->dx7[TILES_DX7_ALGORITHM] & 31u];
    int32_t g = 0;
    for (uint32_t k = 0; k < 6u; k++) {
        if (is_carrier(alg[k]) && v->op[k].gain > g) {
            g = v->op[k].gain;
        }
    }
    return (float)g / (float)Q24;
}

/* ---- Rendering --------------------------------------------------------- */

/* gain (Q24, up to 2^25) times a Q15 sine, Q24. */
#if defined(__ARM_FEATURE_DSP)
INLINE int32_t mul_gain(int32_t g, int32_t s) {
    int32_t r;
    __asm("smulwb %0, %1, %2" : "=r"(r) : "r"(g), "r"(s));
    return r << 1;
}
#else
INLINE int32_t mul_gain(int32_t g, int32_t s) {
    return (int32_t)(((int64_t)g * s) >> 15);
}
#endif

/* One operator over the block: its gain ramping from g to g + n * dg,
 * phase-modulated by `in` (bus, Q24 cycles: << 8 into the 32-bit phase) or
 * by its own last two outputs; into `out`, added or written. */
INLINE void op_run(uint32_t *phase_p, uint32_t inc, int32_t g, int32_t dg, const int32_t *in, int32_t *out,
                   const bool has_in, const bool add, const bool feedback, int32_t *fb, uint32_t fb_shift,
                   uint32_t n) {
    uint32_t ph = *phase_p;
    int32_t y0 = feedback ? fb[0] : 0, y1 = feedback ? fb[1] : 0;
    for (uint32_t i = 0; i < n; i++) {
        g += dg;
        uint32_t p = ph;
        if (feedback) {
            p += (uint32_t)((y0 + y1) >> (fb_shift + 1u)) << 8;
        } else if (has_in) {
            p += (uint32_t)in[i] << 8;
        }
        int32_t y = mul_gain(g, s_sine[p >> (32u - SINE_BITS)]);
        if (feedback) {
            y0 = y1;
            y1 = y;
        }
        out[i] = add ? out[i] + y : y;
        ph += inc;
    }
    *phase_p = ph;
    if (feedback) {
        fb[0] = y0;
        fb[1] = y1;
    }
}

/* The same with the flags at run time, any n: a block shorter than 16
 * (the engine renders 16 at a time). */
static __attribute__((noinline)) void op_run_any(uint32_t *phase, uint32_t inc, int32_t g, int32_t dg,
                                                 const int32_t *in, int32_t *out, bool has_in, bool add,
                                                 bool feedback, int32_t *fb, uint32_t fb_shift, uint32_t n) {
    op_run(phase, inc, g, dg, in, out, has_in, add, feedback, fb, fb_shift, n);
}

/* Unipolar LFO 0..1 at phase (32-bit), its six DX7 waves. */
static float lfo_value(tiles_fm_voice_t *v, uint8_t wave, bool wrapped) {
    float t = (float)v->lfo_phase * (1.0f / 4294967296.0f);
    switch (wave) {
    case 0: /* triangle */
        return t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
    case 1: /* saw down */
        return 1.0f - t;
    case 2: /* saw up */
        return t;
    case 3: /* square */
        return t < 0.5f ? 1.0f : 0.0f;
    case 4: /* sine */
        return 0.5f + 0.5f * synth_sin_turns(t);
    default: /* sample and hold */
        if (wrapped) {
            v->lfo_held = (uint8_t)((v->lfo_held * 179u + 17u) & 0xFFu);
        }
        return (float)v->lfo_held * (1.0f / 255.0f);
    }
}

void TILES_AUDIO_HOT(tiles_fm_voice_render)(tiles_fm_voice_t *v, const tiles_fm_input_t *in, float gain,
                                            float *restrict l, float *restrict r, uint32_t n) {
    if (!v->sounding || n == 0u || n > BLOCK_MAX) {
        return;
    }
    const tiles_fm_patch_t *patch = v->patch;
    const uint8_t *d = patch->dx7;
    const uint8_t *alg = k_alg[d[TILES_DX7_ALGORITHM] & 31u];

    /* Control, once a block: inputs, LFO, pitch, envelopes, gains. */
    v->pressure_s += (in->pressure - v->pressure_s) * INPUT_SMOOTH;
    v->slide_s += (in->slide - v->slide_s) * INPUT_SMOOTH;
    float note = in->note + (float)((int32_t)d[TILES_DX7_TRANSPOSE] - 24);
    v->pitch += (note - v->pitch) * v->glide_coef;

    /* The LFO, when the voice uses it. */
    uint32_t before = v->lfo_phase;
    v->lfo_phase += v->lfo_inc * n;
    float pitch_lfo = 0.0f, amd = 0.0f;
    if ((d[TILES_DX7_LFO_PMD] | d[TILES_DX7_LFO_AMD]) != 0u) {
        float lfo = lfo_value(v, d[TILES_DX7_LFO_WAVE] % 6u, v->lfo_phase < before);
        uint32_t dl = v->lfo_delay;
        v->lfo_delay = v->lfo_delay_inc == 0xFFFFFFFFu || dl > 0xFFFFFFFFu - v->lfo_delay_inc * n
                           ? 0xFFFFFFFFu
                           : dl + v->lfo_delay_inc * n;
        float delay =
            v->lfo_delay < 0x80000000u ? 0.0f : (float)(v->lfo_delay - 0x80000000u) * (1.0f / 2147483648.0f);
        float pmd = (float)((d[TILES_DX7_LFO_PMD] * 165) >> 6) * (1.0f / 255.0f);
        float pms = (float)k_pitch_mod_sens[d[TILES_DX7_PITCH_MOD_SENS] & 7u] * (1.0f / 255.0f);
        pitch_lfo = pmd * pms * delay * (2.0f * lfo - 1.0f) * 0.99f;
        amd = (float)((d[TILES_DX7_LFO_AMD] * 165) >> 6) * (1.0f / 255.0f) * delay * (1.0f - lfo);
    }

    peg_step(v, n);
    /* Pitch: the note (glided, bent) plus the pitch envelope and the LFO's
     * up to an octave; one exponential, each operator its ratio of it. */
    float note_oct = 8.781360f + (v->pitch - 69.0f) * (1.0f / 12.0f);
    float note_hz = synth_exp2(note_oct + (float)v->peg_level * (1.0f / (float)Q24) + pitch_lfo);
    bool repitch = note_hz != v->hz; /* a held, unbent note keeps its steps */
    v->hz = note_hz;
    int32_t boost = (int32_t)((patch->pressure_db * v->pressure_s + patch->slide_db * v->slide_s) *
                              ((float)Q24 / 6.0206f));

    int32_t g_from[6], g_step[6];
    bool any = false;
    for (uint32_t k = 0; k < 6u; k++) {
        tiles_fm_op_t *o = &v->op[k];
        any = any || o->env.down;
        if (o->silent) {
            g_from[k] = 0;
            continue;
        }
        const uint8_t *op = &d[k * TILES_DX7_OP_BYTES];
        if (repitch && !o->fixed) {
            o->inc = to_inc(note_hz * o->ratio);
        }
        env_step(&o->env, op, o->outlevel, o->rate_scaling, n);
        int32_t level = o->env.level;
        uint8_t ams = op[14] & 3u;
        if (ams != 0u) {
            /* Amp mod takes a share of the level (measured: an exponential
             * of depth x sensitivity). */
            float s = amd * k_amp_mod_sens[ams] * 64.0f;
            level = (int32_t)((float)level *
                              (1.0f - synth_exp2(1.442695f * (12.2f + 0.07f * s)) * (16.0f / 268435456.0f)));
        }
        bool carrier = is_carrier(alg[k]);
        if (!carrier && level >= Q24) {
            level += boost; /* a silent modulator stays silent (and skipped) */
        }
        int32_t g = level == o->gain_level ? o->gain : gain_of(level); /* held: as it was */
        o->gain_level = level;
        g_from[k] = o->gain;
        g_step[k] = n == BLOCK_MAX ? (g - o->gain) / (int32_t)BLOCK_MAX : (g - o->gain) / (int32_t)n;
        o->gain = g;
        any = any || (carrier && g >= LEVEL_THRESHOLD);
    }

    /* The operators, 6 first, through two buses into the output. */
    int32_t bus[3][BLOCK_MAX];
    bool full[3] = {false, false, false};
    const uint32_t fb_shift = d[TILES_DX7_FEEDBACK] == 0u ? 16u : 8u - (d[TILES_DX7_FEEDBACK] & 7u);
    for (uint32_t k = 0; k < 6u; k++) {
        tiles_fm_op_t *o = &v->op[k];
        uint8_t flags = alg[k];
        uint32_t outb = flags & 3u, inb = (flags >> 4) & 3u;
        bool add = (flags & ADD) != 0u && full[outb];
        int32_t g0 = g_from[k];
        if (g0 < LEVEL_THRESHOLD && o->gain < LEVEL_THRESHOLD) {
            if (!add) {
                full[outb] = false;
            }
            o->phase += o->inc * n;
            continue;
        }
        int32_t *out = bus[outb];
        bool has_in = inb != 0u && full[inb];
        bool fb = (flags & FB) == FB && fb_shift < 16u;
        if (n != BLOCK_MAX) {
            op_run_any(&o->phase, o->inc, g0, g_step[k], bus[inb], out, has_in, add, fb, v->fb, fb_shift, n);
        } else if (fb) {
            if (add) {
                op_run(&o->phase, o->inc, g0, g_step[k], NULL, out, false, true, true, v->fb, fb_shift, BLOCK_MAX);
            } else {
                op_run(&o->phase, o->inc, g0, g_step[k], NULL, out, false, false, true, v->fb, fb_shift, BLOCK_MAX);
            }
        } else if (has_in) {
            if (add) {
                op_run(&o->phase, o->inc, g0, g_step[k], bus[inb], out, true, true, false, NULL, 0u, BLOCK_MAX);
            } else {
                op_run(&o->phase, o->inc, g0, g_step[k], bus[inb], out, true, false, false, NULL, 0u, BLOCK_MAX);
            }
        } else if (add) {
            op_run(&o->phase, o->inc, g0, g_step[k], NULL, out, false, true, false, NULL, 0u, BLOCK_MAX);
        } else {
            op_run(&o->phase, o->inc, g0, g_step[k], NULL, out, false, false, false, NULL, 0u, BLOCK_MAX);
        }
        full[outb] = true;
    }

    if (full[0]) {
        float a = v->level * OUT_SCALE, da = (gain - v->level) * OUT_SCALE / (float)n;
#pragma GCC unroll 2
        for (uint32_t i = 0; i < n; i++) {
            float y = (float)bus[0][i] * a;
            l[i] += y;
            r[i] += y;
            a += da;
        }
    }
    v->level = gain;
    if (!any) {
        v->sounding = false;
    }
}
