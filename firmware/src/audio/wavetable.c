#include "wavetable.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f
/* Every frame at this RMS before the bank is scaled (a sine peaks at 0.5). */
#define FRAME_RMS 0.3536f

uint8_t g_wt_level_bits[TILES_WT_LEVELS];
uint16_t g_wt_level_offset[TILES_WT_LEVELS];
float g_wt_level_max_f[TILES_WT_LEVELS];

/* A copy's length: linear interpolation images each harmonic h at L - h,
 * down by sinc^2(1 - h/L): 8x the harmonics (-34 dB below the top one),
 * 16x for the short copies whose top harmonics are loud (-47 dB), 64 at
 * least. Every saw alias stays under -60 dB.
 * The top harmonic of a note at f may pass Nyquist up to 28 kHz
 * (f * H <= 0.58): what folds back lands above 20 kHz, unheard, and an
 * octave mip-map keeps more of its top. */
void tiles_wt_init_levels(void) {
    uint32_t offset = 0;
    for (uint32_t level = 0; level < TILES_WT_LEVELS; level++) {
        uint32_t h = TILES_WT_TOP_HARMONICS >> level;
        uint32_t len = 8u * h;
        uint32_t dense = 16u * h < 256u ? 16u * h : 256u;
        len = dense > len ? dense : len;
        len = len < 64u ? 64u : len;
        uint32_t bits = 0;
        while ((1u << bits) < len) {
            bits++;
        }
        g_wt_level_bits[level] = (uint8_t)bits;
        g_wt_level_offset[level] = (uint16_t)offset;
        g_wt_level_max_f[level] = 0.58f / (float)h;
        offset += len + 1u; /* + the guard point */
    }
}

/* In-place radix-2 complex FFT, interleaved re/im; inverse = +i. */
static void fft(float *x, uint32_t n, bool inverse) {
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            float tr = x[2 * i], ti = x[2 * i + 1];
            x[2 * i] = x[2 * j];
            x[2 * i + 1] = x[2 * j + 1];
            x[2 * j] = tr;
            x[2 * j + 1] = ti;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        float ang = (inverse ? 2.0f : -2.0f) * PI_F / (float)len;
        float cs = cosf(ang), sn = sinf(ang), wr = 1.0f, wi = 0.0f;
        for (uint32_t k = 0; k < len / 2; k++) {
            /* The twiddle turned a step at a time, exact again every 32. */
            if ((k & 31u) == 0u) {
                wr = cosf(ang * (float)k);
                wi = sinf(ang * (float)k);
            } else {
                float t = wr * cs - wi * sn;
                wi = wr * sn + wi * cs;
                wr = t;
            }
            for (uint32_t i = k; i < n; i += len) {
                uint32_t a = i, b = i + len / 2;
                float br = x[2 * b] * wr - x[2 * b + 1] * wi;
                float bi = x[2 * b] * wi + x[2 * b + 1] * wr;
                x[2 * b] = x[2 * a] - br;
                x[2 * b + 1] = x[2 * a + 1] - bi;
                x[2 * a] += br;
                x[2 * a + 1] += bi;
            }
        }
    }
}

void tiles_wt_analyze(float *scratch, tiles_wt_spectrum_t *spectrum) {
    const uint32_t n = TILES_WT_FFT_MAX;
    for (uint32_t i = n; i-- > 0;) {
        scratch[2 * i] = scratch[i];
        scratch[2 * i + 1] = 0.0f;
    }
    fft(scratch, n, false);
    /* x = sum a sin(2 pi h t + p): bin h = (n a / 2) e^{i (p - pi/2)}. */
    for (uint32_t h = 1; h <= TILES_WT_TOP_HARMONICS; h++) {
        float re = scratch[2 * h], im = scratch[2 * h + 1];
        spectrum->amp[h - 1] = 2.0f * sqrtf(re * re + im * im) / (float)n;
        spectrum->phase[h - 1] = atan2f(im, re) + 0.5f * PI_F;
    }
}

/* One level of one frame: harmonics 1..H by inverse FFT, left in
 * scratch[0..L) (compacted in place: each read is ahead of the write). */
static uint32_t synth_level(const tiles_wt_spectrum_t *s, uint32_t level, float *scratch) {
    uint32_t len = 1u << tiles_wt_level_bits(level);
    uint32_t harmonics = TILES_WT_TOP_HARMONICS >> level;
    memset(scratch, 0, 2u * len * sizeof(float));
    for (uint32_t h = 1; h <= harmonics; h++) {
        /* a sin(w + p) = Im(a e^{ip} e^{iw}) */
        scratch[2 * h] = s->amp[h - 1] * cosf(s->phase[h - 1]);
        scratch[2 * h + 1] = s->amp[h - 1] * sinf(s->phase[h - 1]);
    }
    fft(scratch, len, true);
    for (uint32_t i = 0; i < len; i++) {
        scratch[i] = scratch[2 * i + 1];
    }
    return len;
}

void tiles_wt_build_begin(tiles_wt_builder_t *b, tiles_wt_bank_t *bank, tiles_wt_design_fn design, void *ctx,
                          tiles_wt_spectrum_t *spectrum, float *scratch) {
    tiles_wt_init_levels();
    memset(b, 0, sizeof(*b));
    b->bank = bank;
    b->design = design;
    b->ctx = ctx;
    b->spectrum = spectrum;
    b->scratch = scratch;
    b->peak = 1e-9f;
}

bool tiles_wt_build_step(tiles_wt_builder_t *b) {
    if (b->pass >= 2u) {
        return true;
    }
    const uint32_t f = b->frame;
    tiles_wt_spectrum_t *spectrum = b->spectrum;
    if (b->level == 0u) { /* the frame's design: a step of its own (a drawn one costs as much as a level) */
        b->design(b->ctx, f, spectrum);
        if (b->pass == 0u) {
            float energy = 0.0f;
            for (uint32_t h = 0; h < TILES_WT_TOP_HARMONICS; h++) {
                energy += spectrum->amp[h] * spectrum->amp[h];
            }
            b->frame_gain[f] = energy > 0.0f ? FRAME_RMS / sqrtf(0.5f * energy) : 0.0f;
        }
        for (uint32_t h = 0; h < TILES_WT_TOP_HARMONICS; h++) {
            spectrum->amp[h] *= b->frame_gain[f];
        }
        b->level = 1u;
        return false;
    }
    const uint32_t level = b->level - 1u;
    uint32_t len = synth_level(spectrum, level, b->scratch);
    const float *v = b->scratch;
    if (b->pass == 0u) {
        float peak = b->peak;
        for (uint32_t i = 0; i < len; i++) {
            float a = fabsf(v[i]);
            peak = a > peak ? a : peak;
        }
        b->peak = peak;
    } else {
        const float to_int = 32767.0f / b->peak;
        int16_t *dst = &b->bank->data[tiles_wt_level_offset(level) * TILES_WT_FRAMES + f];
        for (uint32_t i = 0; i < len; i++) {
            dst[i * TILES_WT_FRAMES] = (int16_t)lrintf(v[i] * to_int);
        }
        dst[len * TILES_WT_FRAMES] = dst[0]; /* guard point: interpolation wraps */
    }
    if (++b->level == TILES_WT_LEVELS + 1u) {
        b->level = 0u;
        if (++b->frame == TILES_WT_FRAMES) {
            b->frame = 0u;
            if (++b->pass == 2u) {
                b->bank->gain = b->peak / 32767.0f;
            }
        }
    }
    return b->pass >= 2u;
}

void tiles_wt_build(tiles_wt_bank_t *bank, tiles_wt_design_fn design, void *ctx, tiles_wt_spectrum_t *spectrum,
                    float *scratch) {
    tiles_wt_builder_t b;
    tiles_wt_build_begin(&b, bank, design, ctx, spectrum, scratch);
    while (!tiles_wt_build_step(&b)) {
    }
}
