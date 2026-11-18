/* The wavetable engine's DSP measured on the computer: band-limiting,
 * filter responses, envelope times, and every factory sound played
 * through the engine (audio/synth_voice.c, wavetable.c, audio_engine.c). */

#include "audio_engine.h"
#include "synth_voice.h"
#include "wavetable.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
            s_failures++;                                    \
        }                                                    \
    } while (0)

#define FS 48000.0
static tiles_wt_bank_t s_banks[TILES_WT_BANK_COUNT];
static tiles_wt_spectrum_t s_spectrum;
static float s_scratch[TILES_WT_SCRATCH_FLOATS];
static synth_context_t s_ctx;

static void design(void *ctx, uint32_t frame, tiles_wt_spectrum_t *spectrum) {
    tiles_wt_design(*(const uint32_t *)ctx, frame, spectrum, s_scratch);
}

static void build_banks(void) {
    for (uint32_t b = 0; b < TILES_WT_BANK_COUNT; b++) {
        tiles_wt_build(&s_banks[b], design, &b, &s_spectrum, s_scratch);
        s_ctx.banks[b] = &s_banks[b];
    }
    s_ctx.beats_per_second = 2.0f;
}

/* Magnitude of bin k of x[0..n) (a plain DFT). */
static double bin(const float *x, uint32_t n, uint32_t k) {
    double re = 0.0, im = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double a = 2.0 * M_PI * (double)k * (double)i / (double)n;
        re += x[i] * cos(a);
        im -= x[i] * sin(a);
    }
    return sqrt(re * re + im * im);
}

static double db(double ratio) {
    return 20.0 * log10(ratio > 1e-12 ? ratio : 1e-12);
}

/* ---- wavetables ---------------------------------------------------------- */

static void test_levels(void) {
    /* Each level holds only its harmonics: nothing above them. */
    const tiles_wt_bank_t *classic = &s_banks[TILES_WT_BANK_CLASSIC];
    for (uint32_t level = 0; level < TILES_WT_LEVELS; level += 2u) {
        uint32_t len = 1u << tiles_wt_level_bits(level), harmonics = TILES_WT_TOP_HARMONICS >> level;
        static float x[1024];
        for (uint32_t i = 0; i < len; i++) {
            x[i] = (float)tiles_wt_sample(classic, 3u, tiles_wt_level_offset(level) + i);
        }
        double fundamental = bin(x, len, 1u), above = 0.0;
        for (uint32_t k = harmonics + 1u; k < len / 2u; k++) {
            double m = bin(x, len, k);
            above = m > above ? m : above;
        }
        CHECK(db(above / fundamental) < -80.0, "level %u has content above harmonic %u (%.1f dB)", level, harmonics,
              db(above / fundamental));
        CHECK(tiles_wt_sample(classic, 3u, tiles_wt_level_offset(level) + len) ==
                  tiles_wt_sample(classic, 3u, tiles_wt_level_offset(level)),
              "level %u guard point",
              level);
    }
    /* Every frame comes out equally loud. */
    for (uint32_t b = 0; b < TILES_WT_BANK_COUNT; b++) {
        double lo = 1e9, hi = 0.0;
        for (uint32_t f = 0; f < TILES_WT_FRAMES; f++) {
            double sum = 0.0;
            for (uint32_t i = 0; i < 1024u; i++) {
                double v = tiles_wt_sample(&s_banks[b], f, i) * (double)s_banks[b].gain;
                sum += v * v;
            }
            double r = sqrt(sum / 1024.0);
            lo = r < lo ? r : lo;
            hi = r > hi ? r : hi;
        }
        CHECK(db(hi / lo) < 0.5, "bank %u frames differ in loudness by %.2f dB", b, db(hi / lo));
    }
    CHECK(tiles_wt_level(20.0f / 48000.0f) == 0u, "a low note isn't on the top level");
    CHECK(tiles_wt_level(0.4f) == TILES_WT_LEVELS - 1u, "a very high note isn't on the sine");
}

/* A saw from the voice at `hz` (bin-aligned over 0.1 s): the strongest
 * component below 18 kHz that isn't a harmonic, against the fundamental. */
static double alias_db(float note, double hz) {
    static const synth_patch_t saw = {
        .name = "t", .bank = TILES_WT_BANK_CLASSIC, .wave = 3.0f / 7.0f, .unison = 1, .phase_reset = true,
        .filter = SYNTH_FILTER_LP12, .cutoff_hz = 20000.0f, .amp = {0.3f, 10.0f, 1.0f, 10.0f},
        .mod = {1.0f, 10.0f, 1.0f, 10.0f}, .level = 1.0f,
    };
    static synth_voice_t v;
    static float l[4800 + 2400], r[4800 + 2400];
    memset(&v, 0, sizeof(v));
    memset(l, 0, sizeof(l));
    memset(r, 0, sizeof(r));
    synth_input_t in = {note, 1.0f, 0.0f, 0.0f};
    synth_voice_start(&v, &saw, &in, 1u, false);
    synth_voice_render(&v, &s_ctx, &in, l, r, 7200u);
    const float *x = l + 2400; /* past the attack and the filter settling */
    double f0 = bin(x, 4800u, (uint32_t)lround(hz / 10.0)), worst = 0.0;
    for (uint32_t k = 2; k < 1800u; k++) {
        double f = k * 10.0, h = f / hz;
        if (fabs(h - round(h)) * hz < 15.0) {
            continue; /* a harmonic */
        }
        double m = bin(x, 4800u, k);
        worst = m > worst ? m : worst;
    }
    return db(worst / f0);
}

static void test_aliasing(void) {
    double a7 = alias_db(105.0f, 3520.0);  /* A7 */
    double a5 = alias_db(81.0f, 880.0);    /* A5 */
    double a3 = alias_db(57.0f, 220.0);    /* A3 */
    printf("  saw aliasing below 18 kHz: A3 %.1f dB, A5 %.1f dB, A7 %.1f dB\n", a3, a5, a7);
    CHECK(a3 < -60.0 && a5 < -60.0 && a7 < -60.0, "saw aliases too loud (A3 %.1f, A5 %.1f, A7 %.1f dB)", a3, a5,
          a7);
}

/* ---- filters ---------------------------------------------------------------- */

/* Steady-state gain of a sine at `hz` through a filter. */
static double svf_gain(float cutoff, float res, double hz) {
    synth_svf_t f;
    memset(&f, 0, sizeof(f));
    synth_svf_set(&f, cutoff, res);
    double peak = 0.0;
    for (uint32_t i = 0; i < 48000u; i++) {
        float y = synth_svf_tick(&f, (float)sin(2.0 * M_PI * hz * i / FS), SYNTH_SVF_LP);
        if (i > 24000u && fabs(y) > peak) {
            peak = fabs(y);
        }
    }
    return db(peak);
}

static double ladder_gain(float cutoff, float res, double hz, double amp) {
    synth_ladder_t f;
    memset(&f, 0, sizeof(f));
    synth_ladder_set(&f, cutoff, res);
    double peak = 0.0;
    for (uint32_t i = 0; i < 48000u; i++) {
        float y = synth_ladder_tick(&f, (float)(amp * sin(2.0 * M_PI * hz * i / FS)));
        if (i > 24000u && fabs(y) > peak) {
            peak = fabs(y);
        }
    }
    return db(peak / amp);
}

static void test_filters(void) {
    const float butterworth = (2.0f - 1.41421356f) / 1.96f;
    double at_fc = svf_gain(1000.0f, butterworth, 1000.0);
    double oct2 = svf_gain(1000.0f, butterworth, 4000.0);
    double high = svf_gain(12000.0f, butterworth, 12000.0);
    CHECK(fabs(at_fc + 3.01) < 0.2, "SVF at cutoff %.2f dB, want -3", at_fc);
    CHECK(oct2 < -22.0 && oct2 > -26.0, "SVF two octaves up %.1f dB, want ~-24", oct2);
    CHECK(fabs(high + 3.01) < 0.3, "SVF at 12 kHz cutoff %.2f dB: the warp isn't exact", high);

    double pass = ladder_gain(1000.0f, 0.0f, 100.0, 0.1);
    double stop = ladder_gain(1000.0f, 0.0f, 4000.0, 0.1);
    CHECK(fabs(pass) < 0.5, "ladder passband %.2f dB", pass);
    CHECK(stop < -44.0, "ladder two octaves up %.1f dB, want ~-48", stop);

    /* Full resonance rings on its own and stays bounded. */
    synth_ladder_t f;
    memset(&f, 0, sizeof(f));
    synth_ladder_set(&f, 800.0f, 1.0f);
    float peak = 0.0f;
    bool finite = true;
    for (uint32_t i = 0; i < 96000u; i++) {
        float y = synth_ladder_tick(&f, i == 0u ? 0.5f : 0.0f);
        finite = finite && isfinite(y);
        if (i > 48000u && fabsf(y) > peak) {
            peak = fabsf(y);
        }
    }
    CHECK(finite && peak > 0.05f && peak < 2.0f, "self-oscillation peak %.3f", peak);
    /* Driven hard at full resonance: limited, not exploding. */
    double loud = ladder_gain(300.0f, 1.0f, 300.0, 4.0);
    CHECK(loud < 6.0, "ladder driven at resonance %.1f dB", loud);
}

/* ---- envelope ---------------------------------------------------------------- */

static void test_envelope(void) {
    synth_adsr_t p = {10.0f, 100.0f, 0.0f, 200.0f};
    synth_env_t e;
    memset(&e, 0, sizeof(e));
    synth_env_start(&e, &p, true);
    uint32_t steps = 0;
    while (e.stage == SYNTH_ENV_ATTACK && steps < 10000u) {
        synth_env_step(&e);
        steps++;
    }
    double attack_ms = steps * (double)SYNTH_CTRL * 1000.0 / FS;
    CHECK(fabs(attack_ms - 10.0) < 0.5, "attack took %.2f ms, want 10", attack_ms);
    steps = 0;
    while (e.value > 0.001f && steps < 100000u) {
        synth_env_step(&e);
        steps++;
    }
    double decay_ms = steps * (double)SYNTH_CTRL * 1000.0 / FS;
    CHECK(fabs(decay_ms - 100.0) < 5.0, "decay to -60 dB took %.1f ms, want 100", decay_ms);
}

/* ---- the factory sounds through the engine -------------------------------- */

static int32_t s_out[2u * 48000u];

static double out_peak(uint32_t frames, bool *finite) {
    double peak = 0.0;
    for (uint32_t i = 0; i < 2u * frames; i++) {
        double v = fabs((double)s_out[i] / 2147483648.0);
        peak = v > peak ? v : peak;
    }
    *finite = peak <= 1.0;
    return peak;
}

static double out_rms(uint32_t frames) {
    double sum = 0.0;
    for (uint32_t i = 0; i < 2u * frames; i++) {
        double v = (double)s_out[i] / 2147483648.0;
        sum += v * v;
    }
    return sqrt(sum / (2.0 * frames));
}

/* High-band share: energy of the first difference against the signal's. */
static double brightness(uint32_t frames) {
    double e = 0.0, d = 0.0;
    for (uint32_t i = 1; i < frames; i++) {
        double a = (double)s_out[2u * i], b = (double)s_out[2u * (i - 1u)];
        e += a * a;
        d += (a - b) * (a - b);
    }
    return e > 0.0 ? d / e : 0.0;
}

static void test_patches(void) {
    for (uint8_t p = 0; p < tiles_audio_engine_patch_count(); p++) {
        tiles_audio_engine_init(48u, 150u, NULL);
        tiles_audio_engine_set_patch(1u, p);
        /* A chord on three MPE channels (track 2), pressure and slide rising. */
        for (uint8_t i = 0; i < 3u; i++) {
            tiles_audio_engine_midi((uint8_t)(0x91u + i), (uint8_t)(48u + 4u * i), 100);
        }
        double peak = 0.0, level = 0.0;
        bool ok = true;
        for (uint32_t step = 0; step < 15u; step++) {
            for (uint8_t i = 0; i < 3u; i++) {
                tiles_audio_engine_midi((uint8_t)(0xD1u + i), (uint8_t)(step * 8u), 0);
                tiles_audio_engine_midi((uint8_t)(0xB1u + i), 74u, (uint8_t)(step * 8u));
            }
            tiles_audio_engine_render(s_out, 4800u);
            bool finite;
            double pk = out_peak(4800u, &finite);
            ok = ok && finite;
            peak = pk > peak ? pk : peak;
            double rr = out_rms(4800u);
            level = rr > level ? rr : level;
        }
        for (uint8_t i = 0; i < 3u; i++) {
            tiles_audio_engine_midi((uint8_t)(0x81u + i), (uint8_t)(48u + 4u * i), 0);
        }
        for (uint32_t step = 0; step < 6u; step++) {
            tiles_audio_engine_render(s_out, 48000u);
        }
        const char *name = tiles_audio_engine_patch_name(p);
        CHECK(ok, "%s: output out of range", name);
        CHECK(level > 0.01, "%s: too quiet (%.4f RMS)", name, level);
        CHECK(peak < 0.99, "%s: hits the ceiling (peak %.3f)", name, peak);
        CHECK(tiles_audio_engine_active_voices() == 0u && out_rms(48000u) == 0.0, "%s: still sounding after release",
              name);
        printf("  %-7s peak %.2f, loudest 0.1 s %.3f RMS\n", name, peak, level);
    }
}

static uint8_t patch_named(const char *name) {
    for (uint8_t p = 0; p < tiles_audio_engine_patch_count(); p++) {
        if (strcmp(tiles_audio_engine_patch_name(p), name) == 0) {
            return p;
        }
    }
    return 0u;
}

static void test_expression(void) {
    /* Pressure opens the sound. */
    tiles_audio_engine_init(48u, 150u, NULL);
    tiles_audio_engine_set_patch(1u, patch_named("haze"));
    tiles_audio_engine_midi(0x91u, 60u, 100u);
    tiles_audio_engine_render(s_out, 48000u);
    tiles_audio_engine_render(s_out, 9600u);
    double soft = brightness(9600u);
    tiles_audio_engine_midi(0xD1u, 127u, 0u);
    tiles_audio_engine_render(s_out, 9600u);
    tiles_audio_engine_render(s_out, 9600u);
    double hard = brightness(9600u);
    CHECK(hard > soft * 1.5, "pressure didn't brighten haze (%.4f -> %.4f)", soft, hard);

    /* Mono legato: one voice, back to the held note, then off. */
    tiles_audio_engine_init(48u, 150u, NULL);
    tiles_audio_engine_set_patch(0u, patch_named("acid"));
    tiles_audio_engine_midi(0x9Fu, 36u, 100u); /* lane 1 */
    tiles_audio_engine_render(s_out, 4800u);
    tiles_audio_engine_midi(0x9Fu, 43u, 100u);
    tiles_audio_engine_render(s_out, 4800u);
    CHECK(tiles_audio_engine_track_voices(0u) == 1u, "acid played %u voices, want 1",
          tiles_audio_engine_track_voices(0u));
    tiles_audio_engine_midi(0x8Fu, 43u, 0u);
    tiles_audio_engine_render(s_out, 4800u);
    CHECK(tiles_audio_engine_track_voices(0u) == 1u, "acid stopped with a note still held");
    tiles_audio_engine_midi(0x8Fu, 36u, 0u);
    tiles_audio_engine_render(s_out, 48000u);
    CHECK(tiles_audio_engine_track_voices(0u) == 0u, "acid kept sounding");
}

int main(void) {
    build_banks();
    test_levels();
    test_aliasing();
    test_filters();
    test_envelope();
    test_patches();
    test_expression();
    if (s_failures != 0) {
        printf("test_synth: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_synth: ok\n");
    return 0;
}
