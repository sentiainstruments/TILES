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

/* A vowel: harmonics under three formant peaks, as sung at C3 (the peaks
 * move with the note, which reads as a choir rather than one singer). */
static void vowel(tiles_wt_spectrum_t *s, float f1, float f2, float f3) {
    const float f0 = 130.8f;
    const float formant[3] = {f1, f2, f3}, width[3] = {90.0f, 110.0f, 170.0f}, level[3] = {1.0f, 0.6f, 0.3f};
    for (uint32_t h = 1; h <= H; h++) {
        float hz = (float)h * f0, a = 0.0f;
        for (uint32_t i = 0; i < 3u; i++) {
            float d = (hz - formant[i]) / width[i];
            a += level[i] * expf(-0.5f * d * d);
        }
        s->amp[h - 1] = (0.15f + a) / (float)h;
        s->phase[h - 1] = spread_phase(h);
    }
}

/* Hard-synced saw (slave at `ratio` x the cycle), each point averaged over
 * 16 sub-points so the drawn edge doesn't alias into the spectrum. */
static void sync_saw(tiles_wt_spectrum_t *s, float ratio, float *scratch) {
    for (uint32_t i = 0; i < TILES_WT_FFT_MAX; i++) {
        float acc = 0.0f;
        for (uint32_t j = 0; j < 16u; j++) {
            float t = ((float)i + ((float)j + 0.5f) / 16.0f) / (float)TILES_WT_FFT_MAX;
            float p = t * ratio;
            acc += 2.0f * (p - floorf(p)) - 1.0f;
        }
        scratch[i] = acc / 16.0f;
    }
    tiles_wt_analyze(scratch, s);
}

static void voice(uint32_t frame, tiles_wt_spectrum_t *s, float *scratch) {
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
    case 4:
        sync_saw(s, 1.5f, scratch);
        break;
    case 5:
        sync_saw(s, 2.3f, scratch);
        break;
    case 6:
        sync_saw(s, 3.4f, scratch);
        break;
    default:
        sync_saw(s, 5.0f, scratch);
        break;
    }
}

void tiles_wt_design(uint32_t bank, uint32_t frame, tiles_wt_spectrum_t *spectrum, float *scratch) {
    clear(spectrum);
    if (bank == TILES_WT_BANK_GLASS) {
        glass(frame, spectrum);
    } else if (bank == TILES_WT_BANK_VOICE) {
        voice(frame, spectrum, scratch);
    } else {
        classic(frame, spectrum);
    }
}
