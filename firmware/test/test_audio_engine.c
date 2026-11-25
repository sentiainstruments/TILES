/* The engine's tracks, voices and guard (audio/audio_engine.c), played
 * with the plain "pure" sound and no effects, and the cross-core ring
 * (audio/audio_ring.h), on the computer. The synth itself: test_synth.c;
 * the effects: test_fx.c. */

#include "audio_engine.h"
#include "audio_ring.h"
#include "fm_voice.h"
#include "presets.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* The smoothed load passes the ceiling on the 5th slow block; 3 more
 * over it end a voice. */
#define GUARD_TEST_BLOCKS 7u
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

/* 141 MHz: the voices' budget is 85 % of 2,937 cycles minus the 1,050
 * reserve, 1,447 (the scenarios below: 12 voices of 120 fit, 13 don't). */
static void pure_everywhere(void) {
    for (uint8_t p = 0; p < TILES_PRESET_SLOTS; p++) {
        char name[TILES_PRESET_NAME_MAX + 1u];
        tiles_preset_name(p, name);
        if (strcmp(name, "pure") == 0) {
            for (uint8_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
                tiles_audio_engine_set_patch(t, p);
                for (uint8_t f = 0; f < TILES_FX_TRACK_PARAMS; f++) {
                    tiles_audio_engine_set_fx(t, f, 0.0f); /* the voices alone: test_fx.c has the effects */
                }
            }
        }
    }
}

static void reset(void) {
    tiles_audio_engine_init(12u, 141u, NULL);
    pure_everywhere();
}

static void costs(uint32_t cycles) {
    for (uint8_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        tiles_audio_engine_set_track_cost(t, cycles);
    }
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

/* Status bytes by part (channel nibbles from services/midi_channels.h). */
#define ON(nib) ((uint8_t)(0x90u | (nib)))
#define OFF(nib) ((uint8_t)(0x80u | (nib)))
#define MELODIC 1u /* the MPE pool: track 2 */
#define DRUMS 9u
#define PHYSICS 10u /* track 4 */
#define CHORDS 11u  /* track 3 */
#define LANE4 12u
#define LANE3 13u
#define LANE2 14u
#define LANE1 15u

static void test_tracks(void) {
    bool lane;
    CHECK(tiles_audio_engine_track(LANE1, &lane) == 0u && lane, "lane 1 not on track 1");
    CHECK(tiles_audio_engine_track(LANE2, &lane) == 1u && lane, "lane 2 not on track 2");
    CHECK(tiles_audio_engine_track(LANE3, &lane) == 2u && lane, "lane 3 not on track 3");
    CHECK(tiles_audio_engine_track(LANE4, &lane) == 3u && lane, "lane 4 not on track 4");
    CHECK(tiles_audio_engine_track(MELODIC, &lane) == 1u && !lane, "melodic not on track 2");
    CHECK(tiles_audio_engine_track(0u, &lane) == 1u && !lane, "MPE master not on track 2");
    CHECK(tiles_audio_engine_track(CHORDS, &lane) == 2u && !lane, "chords not on track 3");
    CHECK(tiles_audio_engine_track(PHYSICS, &lane) == 3u && !lane, "physics not on track 4");
    CHECK(tiles_audio_engine_track(DRUMS, &lane) == TILES_AUDIO_TRACK_DRUMS, "drums not on the drum track");
}

static void test_voices(void) {
    /* A track holds 8 at most. */
    reset();
    for (uint8_t i = 0; i < 10u; i++) {
        tiles_audio_engine_midi(ON(MELODIC), (uint8_t)(48u + i), 100);
    }
    CHECK(tiles_audio_engine_track_voices(1u) == TILES_AUDIO_TRACK_VOICES, "track 2 holds %u",
          tiles_audio_engine_track_voices(1u));

    /* Melodic playing takes lane 2's voice, never its own first. */
    reset();
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_audio_engine_midi(ON(LANE2), (uint8_t)(60u + i), 100);
        tiles_audio_engine_midi(ON(MELODIC), (uint8_t)(70u + i), 100);
    }
    tiles_audio_engine_midi(ON(MELODIC), 80, 100);
    CHECK(tiles_audio_engine_track_voices(1u) == 8u, "track 2 holds %u", tiles_audio_engine_track_voices(1u));
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_audio_engine_midi(OFF(MELODIC), (uint8_t)(70u + i), 0);
    }
    tiles_audio_engine_midi(OFF(MELODIC), 80, 0);
    render(SECOND / 2u);
    CHECK(tiles_audio_engine_track_voices(1u) == 3u, "lane 2 kept %u notes, want 3",
          tiles_audio_engine_track_voices(1u));

    /* A lane note never takes a played one: skipped. */
    reset();
    for (uint8_t i = 0; i < 8u; i++) {
        tiles_audio_engine_midi(ON(MELODIC), (uint8_t)(60u + i), 100);
    }
    tiles_audio_engine_midi(ON(LANE2), 40, 100);
    for (uint8_t i = 0; i < 8u; i++) {
        tiles_audio_engine_midi(OFF(MELODIC), (uint8_t)(60u + i), 0);
    }
    render(SECOND / 2u);
    CHECK(tiles_audio_engine_active_voices() == 0u, "the lane note took a played voice");

    /* A full budget (12 voices at 120 cycles) gives a tail first, then a
     * sequencer note on any track, before anyone's played notes. */
    reset();
    costs(120u);
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_audio_engine_midi(ON(LANE1), (uint8_t)(40u + i), 100);
        tiles_audio_engine_midi(ON(CHORDS), (uint8_t)(60u + i), 100);
        tiles_audio_engine_midi(ON(PHYSICS), (uint8_t)(80u + i), 100);
    }
    CHECK(tiles_audio_engine_active_voices() == 12u, "the budget holds %u", tiles_audio_engine_active_voices());
    tiles_audio_engine_midi(OFF(CHORDS), 61, 0);
    tiles_audio_engine_midi(ON(MELODIC), 70, 100);
    CHECK(tiles_audio_engine_track_voices(2u) == 3u && tiles_audio_engine_track_voices(0u) == 4u,
          "a full pool didn't give the tail first");
    tiles_audio_engine_midi(ON(MELODIC), 71, 100);
    CHECK(tiles_audio_engine_track_voices(0u) == 3u && tiles_audio_engine_track_voices(2u) == 3u &&
              tiles_audio_engine_track_voices(3u) == 4u && tiles_audio_engine_track_voices(1u) == 2u,
          "a full pool didn't give lane 1's note (tracks %u %u %u %u)", tiles_audio_engine_track_voices(0u),
          tiles_audio_engine_track_voices(1u), tiles_audio_engine_track_voices(2u),
          tiles_audio_engine_track_voices(3u));
    /* With no sequencer notes left, the busiest track gives its oldest. */
    reset();
    costs(120u);
    for (uint8_t i = 0; i < 6u; i++) {
        tiles_audio_engine_midi(ON(PHYSICS), (uint8_t)(80u + i), 100);
    }
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_audio_engine_midi(ON(CHORDS), (uint8_t)(60u + i), 100);
    }
    tiles_audio_engine_midi(ON(LANE1), 40, 100);
    tiles_audio_engine_midi(ON(LANE1), 41, 100);
    tiles_audio_engine_midi(ON(MELODIC), 70, 100);
    tiles_audio_engine_midi(ON(MELODIC), 71, 100);
    CHECK(tiles_audio_engine_track_voices(0u) == 0u, "lane 1 kept %u notes", tiles_audio_engine_track_voices(0u));
    tiles_audio_engine_midi(ON(MELODIC), 72, 100);
    CHECK(tiles_audio_engine_track_voices(3u) == 5u && tiles_audio_engine_track_voices(2u) == 4u &&
              tiles_audio_engine_track_voices(1u) == 3u,
          "the busiest track didn't give its oldest");
    tiles_audio_engine_midi(ON(LANE2), 50, 100);
    CHECK(tiles_audio_engine_track_voices(1u) == 3u, "a lane note took a played voice in a full pool");
    render(SECOND / 10u);
    double level = rms(SECOND / 10u);
    CHECK(level > 0.0 && level < 1.0, "full polyphony level %.3f", level);

    /* A heavy note takes as many light ones as it needs. */
    reset();
    costs(100u);
    tiles_audio_engine_set_track_cost(1u, 300u);
    for (uint8_t i = 0; i < 5u; i++) {
        tiles_audio_engine_midi(ON(LANE1), (uint8_t)(40u + i), 100);
    }
    for (uint8_t i = 0; i < 3u; i++) {
        tiles_audio_engine_midi(ON(MELODIC), (uint8_t)(60u + i), 100);
    }
    tiles_audio_engine_midi(ON(MELODIC), 63, 100); /* 1,700 > 1,447: three light voices give way */
    CHECK(tiles_audio_engine_track_voices(0u) == 2u && tiles_audio_engine_track_voices(1u) == 4u,
          "a heavy note took %u light voices", 5u - tiles_audio_engine_track_voices(0u));

    /* A note that can't fit is skipped and takes nothing. */
    reset();
    costs(280u);
    for (uint8_t i = 0; i < 5u; i++) {
        tiles_audio_engine_midi(ON(MELODIC), (uint8_t)(60u + i), 100);
    }
    tiles_audio_engine_midi(ON(LANE1), 40, 100);
    CHECK(tiles_audio_engine_track_voices(0u) == 0u && tiles_audio_engine_track_voices(1u) == 5u,
          "a lane note that couldn't fit took a voice");

    /* Drum hits wait for the drum engine. */
    reset();
    tiles_audio_engine_midi(ON(DRUMS), 36, 100);
    CHECK(tiles_audio_engine_active_voices() == 0u, "a drum hit took a voice");

    reset();
    tiles_audio_engine_midi(0x91, 60, 100);
    tiles_audio_engine_midi(0x91, 60, 100);
    CHECK(tiles_audio_engine_active_voices() == 1u, "same note twice took two voices");
}

/* A clock that makes every track's voices look slow: each timing takes
 * 100 us (15,000 cycles a track, ~234 a voice-sample for 4 voices; 60,000
 * a 16-frame block), over the 90 % ceiling but short of a pause. */
static uint32_t s_fake_us;
static uint32_t slow_clock(void) {
    s_fake_us += 100u;
    return s_fake_us;
}

static void test_guard(void) {
    tiles_audio_engine_init(12u, 150u, slow_clock);
    pure_everywhere();
    for (uint8_t i = 0; i < 4u; i++) {
        tiles_audio_engine_midi(ON(MELODIC), (uint8_t)(60u + i), 100);
    }
    uint32_t cost_before = tiles_audio_engine_track_cost(1u);
    render(16u * (GUARD_TEST_BLOCKS - 1u));
    CHECK(tiles_audio_engine_shed_count() == 0u, "the guard acted before %u blocks", GUARD_TEST_BLOCKS);
    render(16u);
    CHECK(tiles_audio_engine_shed_count() == 1u, "the guard ended %u voices after %u slow blocks",
          tiles_audio_engine_shed_count(), GUARD_TEST_BLOCKS);
    CHECK(tiles_audio_engine_track_cost(1u) > cost_before + 10u, "track 2's cost didn't follow the timing (%u)",
          tiles_audio_engine_track_cost(1u));
    render(SECOND / 10u);
    CHECK(tiles_audio_engine_track_voices(1u) < 4u, "an ended voice still sounds");
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

/* A DX7 voice as track 2's sound: the two-step switch, its name, its
 * sound, the next one into the other slot, back to a preset; a preset
 * the same way. */
static void test_bank_voice(void) {
    reset();
    uint8_t v[TILES_DX7_VOICE_BYTES];
    memcpy(v, g_fm_factory[TILES_FM_TINE].dx7, sizeof(v));
    CHECK(tiles_audio_engine_fm_load(1u, v, "MY PIANO"), "track 2 refused a voice");
    CHECK(!tiles_audio_engine_fm_load(1u, v, "AGAIN"), "a second voice taken before the switch");
    CHECK(strcmp(tiles_audio_engine_track_name(1u), "pure") == 0, "the track switched before sound_use");
    tiles_audio_engine_sound_use(1u);
    CHECK(strcmp(tiles_audio_engine_track_name(1u), "MY PIANO") == 0 &&
              tiles_audio_engine_patch(1u) == TILES_AUDIO_PATCH_BANK,
          "track 2 is '%s' (%u)", tiles_audio_engine_track_name(1u), (unsigned)tiles_audio_engine_patch(1u));
    tiles_audio_engine_midi((uint8_t)(0x90u | MELODIC), 69u, 100u);
    render(SECOND / 4u);
    double level = rms(SECOND / 4u);
    CHECK(level > 0.005, "the bank voice is silent (%.4f RMS)", level);
    CHECK(tiles_audio_engine_fm_load(1u, v, "SECOND") , "the next voice refused");
    tiles_audio_engine_sound_use(1u);
    CHECK(strcmp(tiles_audio_engine_track_name(1u), "SECOND") == 0, "the second voice isn't the track's");
    render(SECOND / 10u);
    tiles_audio_engine_set_patch(1u, 0u);
    CHECK(tiles_audio_engine_patch(1u) == 0u, "back to a factory sound failed");
    CHECK(tiles_audio_engine_sound_load(1u, 6u), "pluck refused");
    CHECK(!tiles_audio_engine_sound_load(1u, 7u), "a second sound taken before the switch");
    CHECK(tiles_audio_engine_patch(1u) == 0u, "the track switched before sound_use");
    tiles_audio_engine_sound_use(1u);
    CHECK(tiles_audio_engine_patch(1u) == 6u && strcmp(tiles_audio_engine_track_name(1u), "pluck") == 0,
          "track 2 is '%s'", tiles_audio_engine_track_name(1u));
    CHECK(!tiles_audio_engine_sound_load(1u, 23u), "an empty slot loaded");
}

int main(void) {
    test_bank_voice();
    test_silence_and_format();
    test_pitch_and_bend();
    test_release_and_sustain();
    test_pressure_and_velocity();
    test_tracks();
    test_voices();
    test_guard();
    test_ring();
    if (s_failures) {
        printf("test_audio_engine: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_audio_engine: ok\n");
    return 0;
}
