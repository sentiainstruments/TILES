/* The drum sampler on the computer: the slot header (audio/sample_slot.c),
 * loading and deleting slots over a simulated flash that can lose power
 * at any step (audio/sample_store.c), and the voices streaming from it
 * (audio/sampler.c). */

#include "kv_store.h"
#include "sample_slot.h"
#include "sample_store.h"
#include "sampler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
            s_failures++;                                   \
        }                                                   \
    } while (0)

/* ---- simulated flash: erase sets 0xFF, program only clears bits ---- */

#define REGION (TILES_SAMPLE_SLOT_COUNT * TILES_SAMPLE_SLOT_BYTES)
static uint8_t g_flash[REGION];
static int g_ops_until_cut = -1; /* -1: never */
static bool g_dead;
static int g_ops;

static bool consume(void) {
    if (g_dead) {
        return false;
    }
    if (g_ops_until_cut == 0) {
        g_dead = true;
        return false;
    }
    if (g_ops_until_cut > 0) {
        g_ops_until_cut--;
    }
    g_ops++;
    return true;
}

static bool sim_read(uint32_t o, uint8_t *b, uint32_t n) {
    memcpy(b, &g_flash[o], n);
    return true;
}

static bool sim_erase(uint32_t o) {
    if (o % TILES_SAMPLE_SECTOR_BYTES != 0u) {
        printf("FAIL erase not on a sector: %u\n", (unsigned)o);
        s_failures++;
        return false;
    }
    if (!consume()) {
        memset(&g_flash[o], 0xFF, 1000); /* a cut erase: part done */
        return false;
    }
    memset(&g_flash[o], 0xFF, TILES_SAMPLE_SECTOR_BYTES);
    return true;
}

static bool sim_program(uint32_t o, const uint8_t *b) {
    if (!consume()) {
        for (uint32_t i = 0; i < TILES_SAMPLE_PAGE_BYTES / 2u; i++) {
            g_flash[o + i] &= b[i]; /* half a page */
        }
        return false;
    }
    for (uint32_t i = 0; i < TILES_SAMPLE_PAGE_BYTES; i++) {
        g_flash[o + i] &= b[i];
    }
    return true;
}

static const tiles_sample_flash_ops_t OPS = {sim_read, sim_erase, sim_program};

/* The fetch: copies at once; `paused` stands for a flash write. */
static bool g_paused;
static bool fetch_busy(void) {
    return false;
}
static bool fetch_start(uint32_t off, uint32_t *dst, uint32_t words) {
    if (g_paused) {
        return false;
    }
    memcpy(dst, &g_flash[off], words * 4u);
    return true;
}
static const tiles_sampler_fetch_t FETCH = {fetch_busy, fetch_start};

static void power_on(void) {
    g_ops_until_cut = -1;
    g_dead = false;
    g_paused = false;
    tiles_sampler_init(&FETCH);
    tiles_sample_store_init(&OPS, 0u);
}

/* A test sample: frame f, channel c. */
static int16_t value(uint32_t f, uint32_t c, uint32_t seed) {
    return (int16_t)((int32_t)((f * 7919u + c * 104729u + seed * 31u) % 60000u) - 30000);
}

static tiles_sample_result_t load(uint8_t slot, uint8_t channels, uint32_t frames, uint32_t seed, const char *name,
                                  uint32_t crc_xor) {
    static uint8_t audio[TILES_SAMPLE_MAX_FRAMES_MONO * 2u];
    uint32_t bytes = frames * channels * 2u;
    for (uint32_t f = 0; f < frames; f++) {
        for (uint32_t c = 0; c < channels; c++) {
            int16_t v = value(f, c, seed);
            uint32_t at = (f * channels + c) * 2u;
            audio[at] = (uint8_t)v;
            audio[at + 1u] = (uint8_t)((uint16_t)v >> 8);
        }
    }
    tiles_sample_info_t info;
    memset(&info, 0, sizeof(info));
    info.channels = channels;
    info.frames = frames;
    info.audio_crc = tiles_kv_crc32(0u, audio, bytes) ^ crc_xor;
    strncpy(info.name, name, TILES_SAMPLE_NAME_MAX);
    tiles_sample_result_t r = tiles_sample_store_begin(slot, &info);
    for (uint32_t done = 0; r == TILES_SAMPLE_OK && done < bytes;) {
        uint32_t n = bytes - done < 777u ? bytes - done : 777u; /* odd chunks, like USB reads */
        r = tiles_sample_store_write(&audio[done], n);
        done += n;
    }
    return r == TILES_SAMPLE_OK ? tiles_sample_store_finish() : r;
}

static void test_header(void) {
    tiles_sample_info_t info = {.channels = 2u, .frames = TILES_SAMPLE_MAX_FRAMES_STEREO, .audio_crc = 0x1234u};
    strcpy(info.name, "kick-808");
    uint8_t page[TILES_SAMPLE_HEADER_BYTES];
    tiles_sample_info_t got;
    CHECK(tiles_sample_header_encode(&info, page) && tiles_sample_header_decode(page, &got), "round trip");
    CHECK(got.channels == 2u && got.frames == info.frames && got.audio_crc == 0x1234u && !strcmp(got.name, "kick-808"),
          "decoded differently");
    page[13] ^= 1u;
    CHECK(!tiles_sample_header_decode(page, &got), "a damaged header was read");
    memset(page, 0xFF, sizeof(page));
    CHECK(!tiles_sample_header_decode(page, &got), "an erased page read as a sample");
    info.frames = TILES_SAMPLE_MAX_FRAMES_STEREO + 1u;
    CHECK(!tiles_sample_info_valid(&info), "stereo past 1 s accepted");
    info.channels = 1u;
    CHECK(tiles_sample_info_valid(&info), "mono up to 2 s refused");
    info.frames = TILES_SAMPLE_MAX_FRAMES_MONO + 1u;
    CHECK(!tiles_sample_info_valid(&info), "mono past 2 s accepted");
    info.frames = 10u;
    strcpy(info.name, "has space");
    CHECK(!tiles_sample_info_valid(&info), "a name with a space accepted");
    info.channels = 3u;
    strcpy(info.name, "x");
    CHECK(!tiles_sample_info_valid(&info), "3 channels accepted");
}

static float s_l[TILES_SAMPLE_MAX_FRAMES_MONO + 64u], s_r[TILES_SAMPLE_MAX_FRAMES_MONO + 64u];

/* Plays everything in 16-frame blocks; frames rendered. */
static uint32_t play_out(uint32_t max_frames) {
    memset(s_l, 0, sizeof(s_l));
    memset(s_r, 0, sizeof(s_r));
    uint32_t f = 0;
    while (tiles_sampler_voices() > 0u && f < max_frames) {
        tiles_sampler_render(&s_l[f], &s_r[f], 16u);
        f += 16u;
    }
    return f;
}

static void test_store_and_stream(void) {
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    tiles_sample_info_t info;
    CHECK(!tiles_sample_store_info(0u, &info), "blank flash has a sample");

    /* A full stereo slot streams back exactly, frame for frame. */
    CHECK(load(2u, 2u, TILES_SAMPLE_MAX_FRAMES_STEREO, 1u, "crash", 0u) == TILES_SAMPLE_OK, "stereo load");
    CHECK(load(5u, 1u, 3000u, 2u, "rim", 0u) == TILES_SAMPLE_OK, "mono load");
    power_on(); /* found again after a restart */
    CHECK(tiles_sample_store_info(2u, &info) && info.channels == 2u && !strcmp(info.name, "crash"), "slot 3 lost");
    CHECK(tiles_sample_store_info(5u, &info) && info.frames == 3000u, "slot 6 lost");
    tiles_sampler_hit(2u, 127u);
    uint32_t frames = play_out(TILES_SAMPLE_MAX_FRAMES_STEREO + 64u);
    CHECK(frames >= TILES_SAMPLE_MAX_FRAMES_STEREO && frames < TILES_SAMPLE_MAX_FRAMES_STEREO + 16u,
          "stereo played %u frames", frames);
    uint32_t wrong = 0;
    for (uint32_t f = 0; f < TILES_SAMPLE_MAX_FRAMES_STEREO; f++) {
        wrong += fabsf(s_l[f] * 32768.0f - value(f, 0u, 1u)) > 0.5f || fabsf(s_r[f] * 32768.0f - value(f, 1u, 1u)) > 0.5f;
    }
    CHECK(wrong == 0u, "%u stereo frames differ from what was loaded", wrong);
    CHECK(tiles_sampler_underruns() == 0u, "a lone voice ran dry %u times", tiles_sampler_underruns());

    /* Mono: centred, equal power, both sides. */
    tiles_sampler_hit(5u, 127u);
    play_out(4000u);
    CHECK(fabsf(s_l[100] * 32768.0f - value(100u, 0u, 2u) * 0.70710678f) < 0.5f && s_l[100] == s_r[100],
          "mono not centred");

    /* Velocity: a softer hit is quieter. */
    tiles_sampler_hit(5u, 64u);
    play_out(4000u);
    CHECK(fabsf(s_l[100] * 32768.0f - value(100u, 0u, 2u) * 0.70710678f * (64.0f / 127.0f) * (64.0f / 127.0f)) < 0.5f,
          "velocity curve");

    /* Five voices: a pad restarts its own; a sixth takes the oldest. All
     * five streaming at once never run dry. */
    for (uint8_t s = 0; s < TILES_SAMPLE_SLOT_COUNT; s++) {
        if (!tiles_sample_store_info(s, &info)) {
            CHECK(load(s, 2u, 20000u, s, "hit", 0u) == TILES_SAMPLE_OK, "load slot %u", s + 1u);
        }
    }
    tiles_sampler_hit(0u, 100u);
    tiles_sampler_hit(0u, 100u);
    CHECK(tiles_sampler_voices() == 1u, "a pad's second hit took a second voice");
    for (uint8_t s = 1; s < 6u; s++) {
        tiles_sampler_hit(s, 100u);
    }
    CHECK(tiles_sampler_voices() == TILES_SAMPLER_VOICES, "voices %u", tiles_sampler_voices());
    uint32_t before = tiles_sampler_underruns();
    play_out(48000u);
    CHECK(tiles_sampler_underruns() == before, "five voices ran dry %u times", tiles_sampler_underruns() - before);

    /* A paused stream (flash being written) waits, then goes on. */
    tiles_sampler_hit(2u, 127u);
    g_paused = true;
    memset(s_l, 0, sizeof(s_l));
    memset(s_r, 0, sizeof(s_r));
    for (uint32_t f = 0; f < 1600u; f += 16u) {
        tiles_sampler_render(&s_l[f], &s_r[f], 16u);
    }
    CHECK(tiles_sampler_underruns() > before, "a paused stream didn't count as running dry");
    g_paused = false;
    CHECK(tiles_sampler_voices() == 1u, "a paused voice was dropped");

    /* Deleting a slot stops its voice; the slot reads empty after restart. */
    play_out(TILES_SAMPLE_MAX_FRAMES_STEREO);
    tiles_sampler_hit(5u, 127u);
    CHECK(tiles_sample_store_delete(5u) == TILES_SAMPLE_OK, "delete");
    float l[16] = {0}, r[16] = {0};
    tiles_sampler_render(l, r, 16u);
    tiles_sampler_hit(5u, 127u);
    CHECK(l[0] == 0.0f && tiles_sampler_voices() == 0u, "a deleted slot still sounded");
    power_on();
    CHECK(!tiles_sample_store_info(5u, &info), "a deleted slot came back");
}

static void test_bad_loads(void) {
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    CHECK(load(0u, 1u, 1000u, 3u, "kick", 0u) == TILES_SAMPLE_OK, "first load");
    CHECK(load(0u, 1u, 1000u, 4u, "kick2", 0xBADu) == TILES_SAMPLE_ERR_CRC, "a bad CRC was accepted");
    tiles_sample_info_t info;
    CHECK(!tiles_sample_store_info(0u, &info), "a failed load left a sample");
    power_on();
    CHECK(!tiles_sample_store_info(0u, &info), "a failed load left a sample after restart");

    info = (tiles_sample_info_t){.channels = 1u, .frames = 100u};
    strcpy(info.name, "x");
    uint8_t buf[300] = {0};
    CHECK(tiles_sample_store_begin(1u, &info) == TILES_SAMPLE_OK, "begin");
    CHECK(tiles_sample_store_begin(2u, &info) == TILES_SAMPLE_ERR_BUSY, "a second load while one runs");
    CHECK(tiles_sample_store_write(buf, 201u) == TILES_SAMPLE_ERR_SIZE, "more bytes than announced");
    CHECK(tiles_sample_store_remaining() == 0u, "an oversized load still under way");
    CHECK(tiles_sample_store_begin(1u, &info) == TILES_SAMPLE_OK, "begin again");
    CHECK(tiles_sample_store_write(buf, 199u) == TILES_SAMPLE_OK && tiles_sample_store_remaining() == 1u, "remaining");
    CHECK(tiles_sample_store_finish() == TILES_SAMPLE_ERR_SIZE, "a short load finished");
    CHECK(tiles_sample_store_begin(9u, &info) == TILES_SAMPLE_ERR_SLOT, "slot 10");
}

/* A power cut at every flash step of a load: the slot is empty or holds
 * the whole new sample, never anything else, and its neighbours never
 * change. */
static void test_power_cuts(void) {
    memset(g_flash, 0xFF, sizeof(g_flash));
    power_on();
    CHECK(load(3u, 2u, 2000u, 5u, "before", 0u) == TILES_SAMPLE_OK, "neighbour");
    CHECK(load(4u, 1u, 900u, 6u, "old", 0u) == TILES_SAMPLE_OK, "old sample");
    g_ops = 0;
    CHECK(load(4u, 1u, 5000u, 7u, "new", 0u) == TILES_SAMPLE_OK, "new sample");
    int steps = g_ops;
    for (int cut = 0; cut <= steps; cut++) {
        power_on();
        g_ops_until_cut = cut;
        (void)load(4u, 1u, 5000u, 8u, "next", 0u);
        power_on();
        tiles_sample_info_t info;
        bool have = tiles_sample_store_info(4u, &info);
        /* Only once the audio checked out does the header go on (a cut
         * halfway through that page still leaves the whole header). */
        CHECK(!have || (cut >= steps - 1 && info.frames == 5000u && !strcmp(info.name, "next")),
              "cut at step %d of %d left %s", cut, steps, have ? info.name : "?");
        CHECK(tiles_sample_store_info(3u, &info) && !strcmp(info.name, "before"), "cut at %d hurt slot 4", cut);
    }
}

int main(void) {
    test_header();
    test_store_and_stream();
    test_bad_loads();
    test_power_cuts();
    if (s_failures) {
        printf("test_sampler: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_sampler: ok\n");
    return 0;
}
