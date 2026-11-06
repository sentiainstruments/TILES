/* The test voice (audio/audio_engine.c) and the cross-core ring
 * (audio/audio_ring.h), on the computer. */

#include "audio_engine.h"
#include "audio_ring.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int s_failures;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            s_failures++;                        \
        }                                        \
    } while (0)

#define SECOND TILES_AUDIO_RATE_HZ
static int32_t s_buf[2u * SECOND];

static void render(uint32_t frames) {
    tiles_audio_engine_render(s_buf, frames);
}

/* Upward zero crossings of the left channel = cycles of the fundamental. */
static uint32_t cycles(uint32_t frames) {
    uint32_t count = 0;
    for (uint32_t i = 1; i < frames; i++) {
        if (s_buf[2u * (i - 1u)] < 0 && s_buf[2u * i] >= 0) {
            count++;
        }
    }
    return count;
}

static double rms(uint32_t frames) {
    double sum = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double s = (double)s_buf[2u * i] / 2147483648.0;
        sum += s * s;
    }
    return sqrt(sum / frames);
}

static void reset(void) {
    tiles_audio_engine_init(12u);
}

static void test_silence_and_format(void) {
    reset();
    render(SECOND / 10u);
    CHECK(rms(SECOND / 10u) == 0.0, "no notes, not silent");
    tiles_audio_engine_midi(0x91, 60, 100);
    render(SECOND / 10u);
    bool low_bits_clear = true;
    for (uint32_t i = 0; i < 2u * (SECOND / 10u); i++) {
        low_bits_clear = low_bits_clear && (s_buf[i] & 0xFF) == 0;
    }
    CHECK(low_bits_clear, "24-bit samples not in the top of the subslot");
    CHECK(s_buf[100] == s_buf[101], "left and right differ");
}

static void test_pitch_and_bend(void) {
    reset();
    tiles_audio_engine_midi(0x92, 69, 100); /* A4 */
    render(SECOND / 10u);                   /* past the attack */
    render(SECOND);
    uint32_t a4 = cycles(SECOND);
    CHECK(a4 >= 437u && a4 <= 443u, "A4 played %u Hz", a4);
    tiles_audio_engine_midi(0xE2, 0x7F, 0x7F); /* full bend up = +12 */
    render(SECOND / 10u);
    render(SECOND);
    uint32_t bent = cycles(SECOND);
    CHECK(bent >= 874u && bent <= 886u, "full bend played %u Hz, want ~880", bent);
    tiles_audio_engine_midi(0xE3, 0x7F, 0x7F); /* another channel's bend leaves it alone */
    render(SECOND);
    CHECK(cycles(SECOND) >= 874u, "bend on another channel moved this note");
}

static void test_release_and_sustain(void) {
    reset();
    tiles_audio_engine_midi(0x91, 60, 100);
    render(SECOND / 10u);
    tiles_audio_engine_midi(0x81, 60, 0);
    render(SECOND / 2u);
    CHECK(tiles_audio_engine_active_voices() == 0u, "note still sounding 0.5 s after release");
    render(SECOND / 10u);
    CHECK(rms(SECOND / 10u) == 0.0, "not silent after release");

    tiles_audio_engine_midi(0xB1, 64, 127); /* sustain down */
    tiles_audio_engine_midi(0x91, 64, 100);
    render(SECOND / 10u);
    tiles_audio_engine_midi(0x91, 64, 0); /* note on velocity 0 = off */
    render(SECOND / 2u);
    CHECK(tiles_audio_engine_active_voices() == 1u, "sustain didn't hold the note");
    tiles_audio_engine_midi(0xB1, 64, 0); /* sustain up */
    render(SECOND / 2u);
    CHECK(tiles_audio_engine_active_voices() == 0u, "sustain up didn't release");

    tiles_audio_engine_midi(0x91, 67, 100);
    tiles_audio_engine_midi(0xB1, 123, 0); /* all notes off */
    render(SECOND / 2u);
    CHECK(tiles_audio_engine_active_voices() == 0u, "all notes off left a voice");
}

static void test_pressure_and_velocity(void) {
    reset();
    tiles_audio_engine_midi(0x91, 60, 100);
    render(SECOND / 10u);
    render(SECOND / 10u);
    double light = rms(SECOND / 10u);
    tiles_audio_engine_midi(0xD1, 127, 0);
    render(SECOND / 10u);
    render(SECOND / 10u);
    double hard = rms(SECOND / 10u);
    CHECK(hard > light * 1.4, "pressure didn't raise the level (%.4f -> %.4f)", light, hard);

    reset();
    tiles_audio_engine_midi(0x91, 60, 20);
    render(SECOND / 5u);
    double soft = rms(SECOND / 10u);
    reset();
    tiles_audio_engine_midi(0x91, 60, 127);
    render(SECOND / 5u);
    double loud = rms(SECOND / 10u);
    CHECK(loud > soft * 1.5, "velocity didn't set the level (%.4f vs %.4f)", soft, loud);
}

static void test_voices(void) {
    reset();
    for (uint8_t i = 0; i < 20u; i++) {
        tiles_audio_engine_midi((uint8_t)(0x90u | (i % 15u + 1u)), (uint8_t)(48u + i), 100);
    }
    render(SECOND / 10u);
    CHECK(tiles_audio_engine_active_voices() == TILES_AUDIO_VOICES, "voice count %u",
          tiles_audio_engine_active_voices());
    double level = rms(SECOND / 10u);
    CHECK(level > 0.0 && level < 1.0, "full polyphony level %.3f", level);

    reset();
    tiles_audio_engine_midi(0x91, 60, 100);
    tiles_audio_engine_midi(0x91, 60, 100);
    CHECK(tiles_audio_engine_active_voices() == 1u, "same note twice took two voices");
}

static void test_ring(void) {
    uint32_t storage[8];
    tiles_spsc_t r;
    tiles_spsc_init(&r, storage, 8u);
    uint32_t in[6] = {1, 2, 3, 4, 5, 6}, out[8];
    CHECK(tiles_spsc_push(&r, in, 6u), "push into an empty ring failed");
    CHECK(!tiles_spsc_push(&r, in, 3u), "push past capacity accepted");
    CHECK(tiles_spsc_pop(&r, out, 4u) == 4u && out[3] == 4u, "pop order");
    CHECK(tiles_spsc_push(&r, in, 6u), "push across the wrap failed");
    CHECK(tiles_spsc_count(&r) == 8u, "count after wrap %u", tiles_spsc_count(&r));
    CHECK(tiles_spsc_pop(&r, out, 8u) == 8u && out[1] == 6u && out[2] == 1u && out[7] == 6u, "wrapped order");
    CHECK(tiles_spsc_pop(&r, out, 1u) == 0u, "pop from empty");
}

int main(void) {
    test_silence_and_format();
    test_pitch_and_bend();
    test_release_and_sustain();
    test_pressure_and_velocity();
    test_voices();
    test_ring();
    if (s_failures) {
        printf("test_audio_engine: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_audio_engine: ok\n");
    return 0;
}
