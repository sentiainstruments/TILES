#include "wavetable.h"

#include <math.h>
#include <string.h>

/* The factory wavetables, as spectra (docs/architecture/synth-engines.md,
 * "Wavetables"). Frames run dark to bright, so pressure sweeping up the
 * frames opens the sound. */

#define PI_F 3.14159265358979f
#define H TILES_WT_TOP_HARMONICS

static void clear(tiles_wt_spectrum_t *s) {
    memset(s, 0, sizeof(*s));
}

/* Fixed spread phases (golden ratio steps): no tall peaks, smooth morphs. */
static float spread_phase(uint32_t h) {
    float x = (float)h * 0.6180339887f * 7.0f;
    return (x - floorf(x)) * 2.0f * PI_F;
}

/* Saw, harmonics falling as 1/h^tilt (1 = the true saw, more = warmer). */
static void saw(tiles_wt_spectrum_t *s, float tilt) {
    for (uint32_t h = 1; h <= H; h++) {
        s->amp[h - 1] = powf((float)h, -tilt);
    }
}

/* Pulse of duty d: (2 / pi h) sin(pi h d) on cosines; d = 0.5 is the square. */
static void pulse(tiles_wt_spectrum_t *s, float d) {
    for (uint32_t h = 1; h <= H; h++) {
        s->amp[h - 1] = 2.0f / (PI_F * (float)h) * sinf(PI_F * (float)h * d);
        s->phase[h - 1] = 0.5f * PI_F;
    }
}

static void triangle(tiles_wt_spectrum_t *s) {
    for (uint32_t h = 1; h <= H; h += 2) {
        s->amp[h - 1] = ((h / 2u) % 2u == 0u ? 1.0f : -1.0f) / ((float)h * (float)h);
    }
}

static void classic(uint32_t frame, tiles_wt_spectrum_t *s) {
    switch (frame) {
    case 0:
        s->amp[0] = 1.0f;
        break;
    case 1:
        triangle(s);
        break;
    case 2:
        saw(s, 1.5f); /* soft saw */
        break;
    case 3:
        saw(s, 1.0f);
        break;
    case 4:
        pulse(s, 0.5f);
        break;
    case 5:
        pulse(s, 0.3f);
        break;
    case 6:
        pulse(s, 0.15f);
        break;
    default:
        pulse(s, 0.07f);
        break;
    }
}

/* Harmonic partials (number, level) on spread phases. */
typedef struct {
    uint8_t h;
    float a;
} partial_t;

static void partials(tiles_wt_spectrum_t *s, const partial_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        s->amp[p[i].h - 1u] = p[i].a;
        s->phase[p[i].h - 1u] = spread_phase(p[i].h);
    }
}

static void glass(uint32_t frame, tiles_wt_spectrum_t *s) {
    static const partial_t sine[] = {{1, 1.0f}};
    static const partial_t octave[] = {{1, 1.0f}, {2, 0.3f}};
    static const partial_t flute[] = {{1, 1.0f}, {2, 0.22f}, {3, 0.12f}, {4, 0.05f}};
    static const partial_t organ[] = {{1, 1.0f}, {2, 0.5f}, {4, 0.35f}, {8, 0.2f}};
    static const partial_t bright[] = {{1, 1.0f}, {2, 0.4f}, {3, 0.25f}, {4, 0.4f}, {6, 0.2f}, {8, 0.3f}, {12, 0.15f}, {16, 0.2f}};
    static const partial_t hollow[] = {{1, 1.0f}, {3, 0.6f}, {5, 0.2f}, {7, 0.4f}, {9, 0.3f}, {11, 0.12f}};
    static const partial_t shimmer[] = {{1, 0.6f}, {2, 0.3f}, {12, 0.22f}, {13, 0.2f}, {14, 0.22f}, {15, 0.18f}, {16, 0.25f}};
    static const partial_t crystal[] = {{1, 0.5f}, {4, 0.2f}, {16, 0.25f}, {18, 0.2f}, {20, 0.22f}, {22, 0.15f}, {24, 0.2f}};
    static const struct {
        const partial_t *p;
        uint32_t n;
    } frames[TILES_WT_FRAMES] = {
        {sine, 1}, {octave, 2}, {flute, 4}, {organ, 4}, {bright, 8}, {hollow, 6}, {shimmer, 7}, {crystal, 7},
    };
    partials(s, frames[frame].p, frames[frame].n);
}

/* Harmonics under three formant peaks (Hz, widths in Hz) for a note at f0:
 * the peaks move with the note. `floor` keeps the low harmonics. */
static void formants(tiles_wt_spectrum_t *s, float f0, const float f[3], const float width[3], float floor) {
    static const float level[3] = {1.0f, 0.6f, 0.3f};
    for (uint32_t h = 1; h <= H; h++) {
        float hz = (float)h * f0, a = 0.0f;
        for (uint32_t i = 0; i < 3u; i++) {
            float d = (hz - f[i]) / width[i];
            a += level[i] * expf(-0.5f * d * d);
        }
        s->amp[h - 1] = (floor + a) / (float)h;
        s->phase[h - 1] = spread_phase(h);
    }
}

/* A vowel as sung at C3 (the peaks move with the note, which reads as a
 * choir rather than one singer). */
static void vowel(tiles_wt_spectrum_t *s, float f1, float f2, float f3) {
    static const float width[3] = {90.0f, 110.0f, 170.0f};
    const float f[3] = {f1, f2, f3};
    formants(s, 130.8f, f, width, 0.15f);
}

/* One cycle drawn in time, `sub` points averaged per sample so a drawn
 * edge doesn't alias into the spectrum, then analysed. */
typedef float (*shape_fn)(float t, const float *k);

static void drawn(tiles_wt_spectrum_t *s, shape_fn fn, const float *k, uint32_t sub, float *scratch) {
    const float n = (float)TILES_WT_FFT_MAX, step = 1.0f / (float)sub;
    for (uint32_t i = 0; i < TILES_WT_FFT_MAX; i++) {
        float acc = 0.0f;
        for (uint32_t j = 0; j < sub; j++) {
            acc += fn(((float)i + ((float)j + 0.5f) * step) / n, k);
        }
        scratch[i] = acc * step;
    }
    tiles_wt_analyze(scratch, s);
}

/* sin(2 pi t), a polynomial (under 4e-6 off): drawing calls it thousands
 * of times a frame, while the sound plays. */
static float sin_turns(float t) {
    t -= floorf(t + 0.5f);
    t = t > 0.25f ? 0.5f - t : t < -0.25f ? -0.5f - t : t;
    float x = 2.0f * PI_F * t, x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}

/* A saw hard-synced at k[0] x the cycle. */
static float sync_saw_at(float t, const float *k) {
    float p = t * k[0];
    return 2.0f * (p - floorf(p)) - 1.0f;
}

static void voice(uint32_t frame, tiles_wt_spectrum_t *s, float *scratch) {
    static const float sync[4] = {1.5f, 2.3f, 3.4f, 5.0f};
    switch (frame) {
    case 0:
        vowel(s, 300.0f, 870.0f, 2240.0f); /* oo */
        break;
    case 1:
        vowel(s, 570.0f, 840.0f, 2410.0f); /* oh */
        break;
    case 2:
        vowel(s, 730.0f, 1090.0f, 2440.0f); /* ah */
        break;
    case 3:
        vowel(s, 530.0f, 1840.0f, 2480.0f); /* eh */
        break;
    default:
        drawn(s, sync_saw_at, &sync[frame - 4u], 16u, scratch);
        break;
    }
}

/* Eight vowels in the order a mouth opens and spreads (oo oh aw ah ae eh
 * ih ee), as spoken at C2 so they talk on bass notes; narrow peaks. */
static void talk(uint32_t frame, tiles_wt_spectrum_t *s) {
    static const float vowels[TILES_WT_FRAMES][3] = {
        {300.0f, 870.0f, 2240.0f}, {570.0f, 840.0f, 2410.0f}, {590.0f, 880.0f, 2540.0f},
        {730.0f, 1090.0f, 2440.0f}, {660.0f, 1720.0f, 2410.0f}, {530.0f, 1840.0f, 2480.0f},
        {390.0f, 1990.0f, 2550.0f}, {270.0f, 2290.0f, 3010.0f},
    };
    static const float width[3] = {60.0f, 80.0f, 120.0f};
    formants(s, 65.4f, vowels[frame], width, 0.25f);
}

/* Phase modulation: sin(2 pi t + i1 sin(2 pi r1 t) + i2 sin(2 pi r2 t)). */
static float pm(float t, const float *k) {
    return sin_turns(t + (k[1] * sin_turns(k[0] * t) + k[3] * sin_turns(k[2] * t)) * (0.5f / PI_F));
}

/* Ratio, index, ratio, index; in order of brightness. */
static void growl(uint32_t frame, tiles_wt_spectrum_t *s, float *scratch) {
    static const float k[TILES_WT_FRAMES][4] = {
        {1.0f, 0.6f, 1.0f, 0.0f}, {2.0f, 1.2f, 1.0f, 0.0f}, {1.0f, 1.4f, 1.0f, 0.0f}, {1.0f, 2.6f, 3.0f, 0.4f},
        {3.0f, 2.0f, 1.0f, 1.5f}, {2.0f, 2.4f, 3.0f, 0.8f}, {2.0f, 3.6f, 5.0f, 0.8f}, {3.0f, 4.0f, 2.0f, 2.0f},
    };
    drawn(s, pm, k[frame], 1u, scratch);
}

/* A sine at gain k[0] plus k[1], folded back each time it passes +-1. */
static float folded(float t, const float *k) {
    float v = k[0] * sin_turns(t) + k[1] + 1.0f;
    float m = v - 4.0f * floorf(v * 0.25f); /* 0..4 */
    return m < 2.0f ? m - 1.0f : 3.0f - m;
}

static void fold(uint32_t frame, tiles_wt_spectrum_t *s, float *scratch) {
    static const float k[TILES_WT_FRAMES][2] = {
        {1.0f, 0.0f}, {1.5f, 0.0f}, {2.1f, 0.0f}, {2.8f, 0.0f}, {3.6f, 0.2f}, {4.6f, 0.3f}, {5.8f, 0.4f}, {7.2f, 0.5f},
    };
    drawn(s, folded, k[frame], 1u, scratch); /* corners fall as 1/h^2: nothing to alias */
}

/* k[0] steps a cycle, their values in k[1..]. */
static float stepped(float t, const float *k) {
    uint32_t i = (uint32_t)(t * k[0]);
    return k[1u + (i < (uint32_t)k[0] ? i : 0u)];
}

/* A sine held at fewer steps a cycle and rounded to fewer levels. */
static void bits(uint32_t frame, tiles_wt_spectrum_t *s, float *scratch) {
    static const uint8_t steps[TILES_WT_FRAMES] = {64, 48, 32, 24, 16, 12, 8, 6};
    static const uint8_t levels[TILES_WT_FRAMES] = {24, 12, 8, 6, 5, 4, 3, 2};
    float k[1u + 64u];
    float half = 0.5f * (float)levels[frame];
    k[0] = (float)steps[frame];
    for (uint32_t i = 0; i < steps[frame]; i++) {
        k[1u + i] = roundf(sin_turns(((float)i + 0.5f) / (float)steps[frame]) * half) / half;
    }
    drawn(s, stepped, k, 8u, scratch);
}

/* A saw through a 4-pole low-pass with a resonant peak at harmonic k. */
static void sweep(uint32_t frame, tiles_wt_spectrum_t *s) {
    static const float cutoff[TILES_WT_FRAMES] = {2.0f, 3.5f, 6.0f, 10.0f, 16.0f, 26.0f, 42.0f, 70.0f};
    const float k = cutoff[frame];
    for (uint32_t h = 1; h <= H; h++) {
        float x = (float)h / k, x4 = x * x * x * x;
        float d = log2f(x) / 0.25f;
        s->amp[h - 1] = (1.0f + 4.0f * expf(-0.5f * d * d)) / ((float)h * sqrtf(1.0f + x4 * x4));
    }
}

/* Drawbars at 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' (harmonics 1 2 3 4 5
 * 6 8), each 0-8 (3 dB a step), on spread phases. */
static void organ(uint32_t frame, tiles_wt_spectrum_t *s) {
    static const uint8_t harmonic[7] = {1, 2, 3, 4, 5, 6, 8};
    static const uint8_t bars[TILES_WT_FRAMES][7] = {
        {8, 0, 0, 0, 0, 0, 0}, {8, 6, 0, 0, 0, 0, 0}, {8, 8, 0, 6, 0, 0, 0}, {8, 8, 6, 0, 0, 0, 4},
        {8, 8, 8, 8, 0, 0, 0}, {8, 8, 8, 8, 0, 0, 8}, {8, 8, 8, 8, 6, 6, 8}, {8, 8, 8, 8, 8, 8, 8},
    };
    for (uint32_t i = 0; i < 7u; i++) {
        uint8_t bar = bars[frame][i], h = harmonic[i];
        if (bar > 0u) {
            s->amp[h - 1u] = powf(10.0f, -0.15f * (float)(8u - bar));
            s->phase[h - 1u] = spread_phase(h);
        }
    }
}

static const char *const k_names[TILES_WT_BANK_COUNT] = {
    "classic", "glass", "voice", "talk", "growl", "fold", "bits", "sweep", "organ",
};

const char *tiles_wt_bank_name(uint32_t bank) {
    return bank < TILES_WT_BANK_COUNT ? k_names[bank] : "";
}

void tiles_wt_design(uint32_t bank, uint32_t frame, tiles_wt_spectrum_t *spectrum, float *scratch) {
    clear(spectrum);
    switch (bank) {
    case TILES_WT_BANK_GLASS:
        glass(frame, spectrum);
        break;
    case TILES_WT_BANK_VOICE:
        voice(frame, spectrum, scratch);
        break;
    case TILES_WT_BANK_TALK:
        talk(frame, spectrum);
        break;
    case TILES_WT_BANK_GROWL:
        growl(frame, spectrum, scratch);
        break;
    case TILES_WT_BANK_FOLD:
        fold(frame, spectrum, scratch);
        break;
    case TILES_WT_BANK_BITS:
        bits(frame, spectrum, scratch);
        break;
    case TILES_WT_BANK_SWEEP:
        sweep(frame, spectrum);
        break;
    case TILES_WT_BANK_ORGAN:
        organ(frame, spectrum);
        break;
    default:
        classic(frame, spectrum);
        break;
    }
}
