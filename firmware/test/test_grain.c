/* The granular engine on the computer (audio/grain_voice.c): pitch from
 * the wavetables and the tape, the cloud's level and grain count, pressure
 * moving it, grains kept inside the tape, and the tape itself: a drum slot
 * copied into the delay's line through the sampler, given back after. */

#include "audio_engine.h"
#include "fx.h"
#include "fx_memory.h"
#include "grain_voice.h"
#include "midi_channels.h"
#include "presets.h"
#include "sampler.h"
#include "synth_voice.h"
#include "wavetable.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...)                                \
    do {                                                \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            s_failures++;                               \
        }                                               \
    } while (0)

#define FS 48000.0
#define SECOND 48000u
#define TAPE_SAMPLES 24008u /* the delay's line (fx.c) */

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

/* Power at f Hz (Goertzel), over x[0..n). */
static double power_at(const float *x, uint32_t n, double f) {
    double k = 2.0 * cos(2.0 * M_PI * f / FS), s1 = 0.0, s2 = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double s0 = x[i] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}

/* The strongest frequency between lo and hi Hz: the centre of the power
 * within 30 Hz of the loudest 1 Hz bin (a cloud of grains at random phases
 * spreads its line over ~1 / grain length). */
static double peak_hz(const float *x, uint32_t n, double lo, double hi) {
    double best = lo, best_p = -1.0;
    for (double f = lo; f <= hi; f += 1.0) {
        double p = power_at(x, n, f);
        if (p > best_p) {
            best_p = p;
            best = f;
        }
    }
    double sum = 0.0, weighted = 0.0;
    for (double f = best - 30.0; f <= best + 30.0; f += 1.0) {
        double p = power_at(x, n, f);
        sum += p;
        weighted += p * f;
    }
    return sum > 0.0 ? weighted / sum : best;
}

static double rms(const float *x, uint32_t n) {
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        sum += (double)x[i] * x[i];
    }
    return sqrt(sum / n);
}

static bool finite_all(const float *x, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!isfinite(x[i]) || fabsf(x[i]) > 4.0f) {
            return false;
        }
    }
    return true;
}

/* ---- one voice -------------------------------------------------------------- */

static float s_l[2u * SECOND], s_r[2u * SECOND];

static synth_patch_t plain_patch(const tiles_grain_params_t *g, uint8_t bank) {
    synth_patch_t p;
    memset(&p, 0, sizeof(p));
    p.name = "test";
    p.engine = SYNTH_ENGINE_GRAIN;
    p.grain = g;
    p.bank = bank;
    p.filter = SYNTH_FILTER_LP12;
    p.cutoff_hz = 20000.0f;
    p.amp = (synth_adsr_t){1.0f, 100.0f, 1.0f, 100.0f};
    p.mod = (synth_adsr_t){1.0f, 100.0f, 1.0f, 100.0f};
    p.level = 1.0f;
    return p;
}

static tiles_grain_params_t plain_grains(void) {
    tiles_grain_params_t g;
    memset(&g, 0, sizeof(g));
    g.source = TILES_GRAIN_SRC_WAVE;
    g.root = 60u;
    g.size_ms = 100.0f;
    g.density = 40.0f;
    g.jitter = 0.5f;
    return g;
}

/* Renders `frames` of one voice holding `note` (mono sum in s_l); counts
 * the grains sounding at each control step into *mean_grains. */
static void play(const synth_patch_t *p, float note, float pressure, uint32_t frames, double *mean_grains) {
    static tiles_grain_voice_t v;
    synth_input_t in = {note, 0.8f, pressure, 0.0f};
    tiles_grain_voice_start(&v, p, &in, 12345u, false, false);
    memset(s_l, 0, frames * sizeof(float));
    memset(s_r, 0, frames * sizeof(float));
    double grains = 0.0;
    uint32_t steps = 0;
    for (uint32_t i = 0; i < frames; i += 16u) {
        tiles_grain_voice_render(&v, p, &s_ctx, &in, &s_l[i], &s_r[i], 16u);
        grains += tiles_grain_voice_grains(&v);
        steps++;
    }
    for (uint32_t i = 0; i < frames; i++) {
        s_l[i] = 0.5f * (s_l[i] + s_r[i]);
    }
    if (mean_grains != NULL) {
        *mean_grains = grains / steps;
    }
}

static void test_wave_pitch(void) {
    tiles_grain_params_t g = plain_grains();
    synth_patch_t p = plain_patch(&g, TILES_WT_BANK_CLASSIC); /* frame 0: a sine */
    play(&p, 69.0f, 0.0f, SECOND, NULL);
    double hz = peak_hz(&s_l[SECOND / 4u], SECOND / 2u, 300.0, 1000.0);
    CHECK(fabs(hz - 440.0) <= 2.0, "wave grains at A4 peak at %.0f Hz", hz);
    CHECK(finite_all(s_l, SECOND), "wave grains out of range");

    g.octave_up = 1.0f;
    play(&p, 69.0f, 0.0f, SECOND, NULL);
    hz = peak_hz(&s_l[SECOND / 4u], SECOND / 2u, 300.0, 1000.0);
    CHECK(fabs(hz - 880.0) <= 3.0, "octave-up grains peak at %.0f Hz", hz);

    g.octave_up = 0.0f;
    g.reverse = 1.0f;
    play(&p, 69.0f, 0.0f, SECOND, NULL);
    hz = peak_hz(&s_l[SECOND / 4u], SECOND / 2u, 300.0, 1000.0);
    CHECK(fabs(hz - 440.0) <= 2.0, "backward grains peak at %.0f Hz", hz);
    printf("  wave grains: A4 at the pitch, octaves up, backwards\n");
}

static void test_cloud(void) {
    /* The overlap follows density x size, up to the cloud's 8; the level
     * stays within a few dB from one grain to eight. */
    tiles_grain_params_t g = plain_grains();
    g.jitter = 0.0f;
    synth_patch_t p = plain_patch(&g, TILES_WT_BANK_CLASSIC);
    p.wave = 3.0f / 7.0f; /* a saw */
    double sparse_grains, dense_grains;
    g.density = 10.0f;
    g.size_ms = 100.0f;
    play(&p, 57.0f, 0.0f, SECOND, &sparse_grains);
    double sparse = rms(&s_l[SECOND / 4u], SECOND / 2u);
    g.density = 120.0f;
    g.size_ms = 120.0f;
    play(&p, 57.0f, 0.0f, SECOND, &dense_grains);
    double dense = rms(&s_l[SECOND / 4u], SECOND / 2u);
    CHECK(sparse_grains > 0.8 && sparse_grains < 1.4, "10 grains/s of 100 ms: %.2f at once", sparse_grains);
    CHECK(dense_grains > 7.0 && dense_grains <= 8.0, "a full cloud: %.2f at once", dense_grains);
    double ratio_db = 20.0 * log10(dense / sparse);
    CHECK(fabs(ratio_db) < 6.0, "the cloud's level moved %.1f dB from 1 grain to 8", ratio_db);

    /* Pressure: denser and shorter grains, by the sound's settings. */
    g.density = 20.0f;
    g.size_ms = 100.0f;
    g.density_pressure = 2.0f;
    double soft_grains, hard_grains;
    play(&p, 57.0f, 0.0f, SECOND, &soft_grains);
    play(&p, 57.0f, 1.0f, SECOND, &hard_grains);
    CHECK(hard_grains > 2.5 * soft_grains, "pressure: %.2f -> %.2f grains", soft_grains, hard_grains);
    printf("  cloud: %.1f and %.1f grains at once, %.1f dB apart; pressure %.1f -> %.1f\n", sparse_grains,
           dense_grains, ratio_db, soft_grains, hard_grains);
}

/* ---- the tape ---------------------------------------------------------------- */

#define TAPE_ROOM 48008u
static int16_t s_tape_mem[TAPE_ROOM + 4096u];

static void fill_sine(int16_t *t, uint32_t frames, double hz) {
    for (uint32_t i = 0; i < frames; i++) {
        t[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * hz * i / FS));
    }
}

static void test_tape_voice(void) {
    fill_sine(s_tape_mem, SECOND, 440.0);
    s_ctx.tape = s_tape_mem;
    s_ctx.tape_frames = SECOND;
    s_ctx.tape_gain = 1.0f;
    tiles_grain_params_t g = plain_grains();
    g.source = TILES_GRAIN_SRC_TAPE;
    g.root = 69u;
    g.spray = 0.5f;
    synth_patch_t p = plain_patch(&g, 0u);
    p.wave = 0.5f;
    play(&p, 69.0f, 0.0f, SECOND, NULL);
    double hz = peak_hz(&s_l[SECOND / 4u], SECOND / 2u, 300.0, 1000.0);
    CHECK(fabs(hz - 440.0) <= 2.0, "tape at its root peaks at %.0f Hz", hz);
    play(&p, 81.0f, 0.0f, SECOND, NULL);
    hz = peak_hz(&s_l[SECOND / 4u], SECOND / 2u, 300.0, 1200.0);
    CHECK(fabs(hz - 880.0) <= 3.0, "tape an octave up peaks at %.0f Hz", hz);

    /* Grains never read past what's loaded: the rest of the memory holds a
     * loud DC that would show in the output. Scan, spray, both directions,
     * long grains, and the pitch bent up three octaves while they play. */
    const uint32_t loaded = 20000u;
    fill_sine(s_tape_mem, loaded, 440.0);
    for (uint32_t i = loaded; i < TAPE_ROOM + 4096u; i++) {
        s_tape_mem[i] = 30000;
    }
    s_ctx.tape_frames = loaded;
    g.scan = 2.0f;
    g.spray = 1.0f;
    g.reverse = 0.5f;
    g.size_ms = 400.0f;
    g.density = 60.0f;
    static tiles_grain_voice_t v;
    synth_input_t in = {69.0f, 0.8f, 0.0f, 0.0f};
    tiles_grain_voice_start(&v, &p, &in, 777u, false, false);
    double sum = 0.0;
    bool ok = true;
    for (uint32_t i = 0; i < 2u * SECOND; i += 16u) {
        in.note = 69.0f + 36.0f * (float)(0.5 + 0.5 * sin(2.0 * M_PI * i / (0.7 * FS)));
        float l[16] = {0}, r[16] = {0};
        tiles_grain_voice_render(&v, &p, &s_ctx, &in, l, r, 16u);
        ok = ok && finite_all(l, 16u) && finite_all(r, 16u);
        for (uint32_t k = 0; k < 16u; k++) {
            sum += l[k] + r[k];
        }
    }
    double mean = sum / (4.0 * SECOND);
    CHECK(ok, "tape grains out of range");
    CHECK(fabs(mean) < 0.01, "tape grains read past the loaded part (mean %.4f)", mean);

    /* The tape gone (its memory lent): grains stop at once. */
    s_ctx.tape_frames = 0u;
    float l[16] = {0}, r[16] = {0};
    tiles_grain_voice_render(&v, &p, &s_ctx, &in, l, r, 16u);
    CHECK(tiles_grain_voice_grains(&v) == 0u, "grains kept playing a tape that went");
    s_ctx.tape = NULL;
    printf("  tape grains: root and octave pitch, inside the loaded part (mean %.5f)\n", mean);
}

/* ---- the tape through the engine --------------------------------------------- */

static int16_t s_flash[2u * SECOND * 2u];
static bool fetch_busy(void) {
    return false;
}
static bool fetch_start(uint32_t off, uint32_t *dst, uint32_t words) {
    memcpy(dst, (const uint8_t *)s_flash + off, words * 4u);
    return true;
}
static const tiles_sampler_fetch_t FETCH = {fetch_busy, fetch_start};

static int32_t s_out[2u * 4800u];

static uint8_t patch_named(const char *name) {
    for (uint8_t p = 0; p < TILES_PRESET_SLOTS; p++) {
        char n[TILES_PRESET_NAME_MAX + 1u];
        tiles_preset_name(p, n);
        if (strcmp(n, name) == 0) {
            return p;
        }
    }
    return 0xFFu;
}

/* Renders `frames` (a multiple of 4800) and keeps the last 4800 as mono. */
static void engine_run(uint32_t frames) {
    for (uint32_t done = 0; done < frames; done += 4800u) {
        tiles_audio_engine_render(s_out, 4800u);
    }
    for (uint32_t i = 0; i < 4800u; i++) {
        s_l[i] = (float)(0.5 * ((double)s_out[2u * i] + (double)s_out[2u * i + 1u]) / 2147483648.0);
    }
}

/* A slot of `frames` at `hz` (stereo: the same in both) at flash offset `at`. */
static void put_slot(uint8_t slot, uint8_t channels, uint32_t frames, double hz, uint32_t at) {
    int16_t *d = (int16_t *)((uint8_t *)s_flash + at);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t x = (int16_t)(20000.0 * sin(2.0 * M_PI * hz * i / FS));
        for (uint32_t c = 0; c < channels; c++) {
            d[i * channels + c] = x;
        }
    }
    tiles_sampler_slot_set(slot, channels, frames, at, (const uint8_t *)d);
}

static void test_engine_tape(void) {
    tiles_sampler_init(&FETCH);
    tiles_audio_engine_init(48u, 150u, NULL);
    uint8_t scatter = patch_named("scatter");
    CHECK(scatter != 0xFFu, "no scatter sound");
    if (scatter == 0xFFu) {
        return;
    }
    /* Slot 8 empty: the last loaded slot (here 3, stereo) instead. */
    put_slot(2u, 2u, SECOND, 261.63, 0u);
    tiles_audio_engine_set_patch(1u, scatter);
    engine_run(4800u * 10u);
    uint32_t frames = tiles_audio_engine_tape_frames();
    /* The tape is the delay's line: 0.5 s (24,008 samples), a stereo slot
     * a chunk's room short. */
    CHECK(frames >= TAPE_SAMPLES - 512u && frames <= TAPE_SAMPLES, "a stereo slot on the tape: %u samples", frames);
    CHECK(tiles_fx_delay_lent(), "the delay kept its line");
    tiles_audio_engine_midi(0x91u, 60u, 100u); /* the root: its own speed */
    engine_run(4800u * 5u);
    double hz = peak_hz(s_l, 4800u, 150.0, 600.0);
    CHECK(fabs(hz - 261.63) <= 12.0, "scatter on its root peaks at %.0f Hz", hz);
    CHECK(rms(s_l, 4800u) > 0.005, "scatter silent (%.4f RMS)", rms(s_l, 4800u));
    double first = rms(s_l, 4800u);

    /* Slot 8 loaded (mono): the tape follows it. */
    put_slot(7u, 1u, SECOND, 440.0, 4u * SECOND);
    engine_run(4800u * 10u);
    hz = peak_hz(s_l, 4800u, 150.0, 600.0);
    CHECK(fabs(hz - 440.0) <= 12.0, "scatter on slot 8 peaks at %.0f Hz", hz);
    CHECK(tiles_audio_engine_tape_frames() == TAPE_SAMPLES, "a mono slot on the tape: %u",
          tiles_audio_engine_tape_frames());

    /* The effects' memory lent to the pad capture: no tape, no sound from
     * it, no crash; back, the slot is copied in again. */
    CHECK(tiles_fx_memory_borrow() != NULL, "borrow failed");
    memset(tiles_fx_memory(), 0x55, TILES_FX_MEMORY_BYTES);
    engine_run(4800u);
    CHECK(tiles_audio_engine_tape_frames() == 0u, "the tape outlived its memory");
    tiles_fx_memory_return();
    engine_run(4800u * 10u);
    CHECK(tiles_audio_engine_tape_frames() == TAPE_SAMPLES, "the tape didn't come back: %u",
          tiles_audio_engine_tape_frames());

    /* Another sound: the delay gets its line back, cleared. */
    tiles_audio_engine_midi(0x81u, 60u, 0u);
    tiles_audio_engine_set_patch(1u, patch_named("pluck"));
    engine_run(4800u);
    CHECK(!tiles_fx_delay_lent(), "the delay didn't get its line back");
    CHECK(tiles_audio_engine_tape_frames() == 0u, "a tape with no tape sound");
    printf("  tape: slot fallback, stereo folded, %.3f RMS, slot 8 followed, capture lend, given back\n", first);
}

static void test_factory(void) {
    /* Every granular sound at once on the four tracks, pressure and slide
     * sweeping: bounded, then silent after release. */
    tiles_sampler_init(&FETCH);
    tiles_audio_engine_init(48u, 150u, NULL);
    put_slot(7u, 1u, SECOND, 330.0, 0u);
    const char *names[4] = {"cloud", "dust", "shimmer", "scatter"};
    for (uint8_t t = 0; t < 4u; t++) {
        tiles_audio_engine_set_patch(t, patch_named(names[t]));
    }
    /* Each track's sequencer lane channel. */
    const uint8_t ch[4] = {TILES_MIDI_CH_SEQ_LANE_0, TILES_MIDI_CH_SEQ_LANE_1, TILES_MIDI_CH_SEQ_LANE_2,
                           TILES_MIDI_CH_SEQ_LANE_3};
    double peak = 0.0;
    for (uint8_t t = 0; t < 4u; t++) {
        tiles_audio_engine_midi((uint8_t)(0x90u | ch[t]), (uint8_t)(48u + 5u * t), 100u);
    }
    for (uint32_t step = 0; step < 20u; step++) {
        for (uint8_t t = 0; t < 4u; t++) {
            tiles_audio_engine_midi((uint8_t)(0xD0u | ch[t]), (uint8_t)(step * 6u), 0u);
            tiles_audio_engine_midi((uint8_t)(0xB0u | ch[t]), 74u, (uint8_t)(127u - step * 6u));
        }
        tiles_audio_engine_render(s_out, 4800u);
        for (uint32_t i = 0; i < 2u * 4800u; i++) {
            double v = fabs((double)s_out[i] / 2147483648.0);
            peak = v > peak ? v : peak;
        }
    }
    CHECK(peak > 0.02 && peak < 0.99, "four granular tracks peak %.3f", peak);
    tiles_audio_master_t m = tiles_audio_engine_master();
    CHECK(m.peak > 0.02f && m.clips == 0u, "master: peak %.3f before the clip, %u clipped", (double)m.peak,
          (unsigned)m.clips);
    for (uint8_t t = 0; t < 4u; t++) {
        tiles_audio_engine_midi((uint8_t)(0x80u | ch[t]), (uint8_t)(48u + 5u * t), 0u);
    }
    for (uint32_t step = 0; step < 50u; step++) {
        tiles_audio_engine_render(s_out, 4800u);
    }
    CHECK(tiles_audio_engine_active_voices() == 0u, "granular voices still sounding after release");
    printf("  factory: four granular tracks together, peak %.2f, silent after\n", peak);
}

int main(void) {
    build_banks();
    test_wave_pitch();
    test_cloud();
    test_tape_voice();
    test_engine_tape();
    test_factory();
    if (s_failures != 0) {
        printf("test_grain: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_grain: ok\n");
    return 0;
}
