/* The presets (audio/presets.c): every factory sound through the record and
 * back, bad records refused, and the store over a simulated flash: a record
 * written, read after a restart, cleared back to the factory sound. */

#include "audio_engine.h"
#include "presets.h"
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

/* 12 groups x 2 sectors of simulated NOR flash. */
static uint8_t s_flash[TILES_PRESET_GROUPS][2][TILES_KV_SECTOR_SIZE];
static uint8_t s_group;
#define OPS(g)                                                                                          \
    static bool r##g(uint8_t sl, uint32_t o, uint8_t *b, uint32_t n) {                                 \
        memcpy(b, &s_flash[g][sl][o], n);                                                               \
        return true;                                                                                    \
    }                                                                                                   \
    static bool e##g(uint8_t sl) {                                                                      \
        memset(s_flash[g][sl], 0xFF, TILES_KV_SECTOR_SIZE);                                             \
        return true;                                                                                    \
    }                                                                                                   \
    static bool p##g(uint8_t sl, uint32_t o, const uint8_t *b, uint32_t n) {                           \
        for (uint32_t i = 0; i < n; i++) s_flash[g][sl][o + i] &= b[i];                                \
        return true;                                                                                    \
    }                                                                                                   \
    static const tiles_kv_ops_t o##g = {r##g, e##g, p##g};
OPS(0) OPS(1) OPS(2) OPS(3) OPS(4) OPS(5) OPS(6) OPS(7) OPS(8) OPS(9) OPS(10) OPS(11)
static const tiles_kv_ops_t *const k_ops[TILES_PRESET_GROUPS] = {&o0, &o1, &o2, &o3, &o4, &o5,
                                                                 &o6, &o7, &o8, &o9, &o10, &o11};

static uint8_t s_staging[4096];
static uint8_t s_rec[TILES_PRESET_RECORD_BYTES], s_rec2[TILES_PRESET_RECORD_BYTES];

static void test_round_trip(void) {
    static tiles_sound_t a, b;
    uint32_t n = 0;
    for (uint8_t slot = 0; slot < TILES_PRESET_FACTORY_SLOTS; slot++) {
        if (!tiles_preset_get(slot, &a)) {
            continue;
        }
        n++;
        tiles_preset_encode(&a, s_rec);
        CHECK(tiles_preset_decode(s_rec, &b), "%s doesn't decode", a.name);
        tiles_preset_encode(&b, s_rec2);
        CHECK(memcmp(s_rec, s_rec2, sizeof(s_rec)) == 0, "%s changed through the record", a.name);
        CHECK(strcmp(a.name, b.name) == 0 && b.patch.name == b.name, "%s's name", a.name);
        CHECK((b.patch.engine == SYNTH_ENGINE_FM) == (b.patch.fm == &b.fm), "%s's FM pointer", a.name);
    }
    CHECK(n == 95u, "%u factory sounds, want 95", n);
    tiles_preset_get(0u, &a);
    tiles_preset_encode(&a, s_rec);
    s_rec[2] = 9u;
    CHECK(!tiles_preset_decode(s_rec, NULL), "a future version decoded");
    tiles_preset_encode(&a, s_rec);
    float nan = NAN;
    memcpy(&s_rec[20], &nan, 4);
    CHECK(!tiles_preset_decode(s_rec, NULL), "a NaN decoded");
    tiles_preset_encode(&a, s_rec);
    float loud = 1000.0f;
    memcpy(&s_rec[20 + 4 * 31], &loud, 4); /* level */
    CHECK(tiles_preset_decode(s_rec, &b) && b.patch.level <= 4.0f, "level not clamped: %f", (double)b.patch.level);
    printf("  record: %u factory sounds round-trip, bad records refused, values clamped\n", n);
}

static void test_store(void) {
    memset(s_flash, 0xFF, sizeof(s_flash));
    (void)s_group;
    tiles_presets_init(k_ops);
    static tiles_sound_t s;
    CHECK(!tiles_preset_stored(60u) && tiles_preset_get(60u, &s), "slot 61 isn't its factory sound");
    tiles_preset_get(6u, &s); /* pluck, into slot 61 as "mine" */
    strcpy(s.name, "mine");
    s.patch.cutoff_hz = 777.0f;
    tiles_preset_encode(&s, s_rec);
    CHECK(tiles_preset_put(60u, s_rec, s_staging) == TILES_PRESET_OK, "put failed");
    tiles_preset_encode(&s, s_rec);
    strcpy(s.name, "acid2");
    tiles_preset_encode(&s, s_rec2);
    CHECK(tiles_preset_put(0u, s_rec2, s_staging) == TILES_PRESET_OK, "put over a factory slot failed");
    tiles_presets_init(k_ops); /* a restart */
    char name[TILES_PRESET_NAME_MAX + 1u];
    tiles_preset_name(60u, name);
    CHECK(strcmp(name, "mine") == 0 && tiles_preset_get(60u, &s) && s.patch.cutoff_hz == 777.0f,
          "slot 61 after a restart: '%s'", name);
    tiles_preset_name(0u, name);
    CHECK(strcmp(name, "acid2") == 0, "slot 1 after a restart: '%s'", name);
    tiles_preset_name(6u, name);
    CHECK(strcmp(name, "pluck") == 0, "a neighbour changed: '%s'", name);
    CHECK(tiles_preset_clear(0u, s_staging) == TILES_PRESET_OK, "clear failed");
    tiles_presets_init(k_ops);
    tiles_preset_name(0u, name);
    CHECK(strcmp(name, "acid") == 0 && tiles_preset_stored(60u), "after clear: '%s'", name);
    memset(s_rec, 0, sizeof(s_rec));
    CHECK(tiles_preset_put(40u, s_rec, s_staging) == TILES_PRESET_ERR_RECORD, "a blank record stored");
    /* Through the engine: track 2 plays slot 61. */
    tiles_audio_engine_init(48u, 230u, NULL);
    tiles_audio_engine_set_patch(1u, 60u);
    CHECK(tiles_audio_engine_patch(1u) == 60u && strcmp(tiles_audio_engine_track_name(1u), "mine") == 0,
          "track 2 is '%s'", tiles_audio_engine_track_name(1u));
    printf("  store: put, restart, a factory slot overridden and cleared, the engine plays a stored one\n");
}

/* Track t is lane t's channel (16 down to 13). */
static void note(uint8_t track, bool on) {
    tiles_audio_engine_midi((uint8_t)((on ? 0x90u : 0x80u) | (15u - track)), 48u, on ? 100u : 0u);
}

static float loudness(uint32_t frames) {
    static int32_t buf[2u * 480u];
    double sum = 0.0;
    for (uint32_t done = 0; done < frames; done += 480u) {
        tiles_audio_engine_render(buf, 480u);
        for (uint32_t i = 0; i < 2u * 480u; i++) {
            double v = buf[i] / 2147483648.0;
            sum += v * v;
        }
    }
    return (float)sqrt(sum / (2.0 * frames));
}

static bool slots_hold(const char *name) {
    char slots[64];
    tiles_audio_engine_wt_slots(slots, sizeof(slots));
    return strstr(slots, name) != NULL;
}

static uint32_t s_cycle;
static uint32_t fake_cycles(void) {
    return s_cycle += 100u;
}

static void service_until_ready(uint8_t track) {
    for (uint32_t i = 0; i < 10000u; i++) {
        if (tiles_audio_engine_service(UINT32_MAX) & (1u << track)) {
            tiles_audio_engine_sound_use(track);
            return;
        }
    }
}

/* Banks built while the engine runs: a sound waits for its bank, the old
 * one playing on; one with no room is refused; one whose only room is its
 * track's old bank switches at once, and that bank goes only once its
 * voices have faded. Slots 71-76 hold pluck on each new bank. */
static void test_swapping(void) {
    static const uint8_t banks[6] = {TILES_WT_BANK_TALK, TILES_WT_BANK_GROWL, TILES_WT_BANK_FOLD,
                                     TILES_WT_BANK_BITS, TILES_WT_BANK_SWEEP, TILES_WT_BANK_ORGAN};
    static tiles_sound_t s;
    for (uint32_t i = 0; i < 6u; i++) {
        tiles_preset_get(6u, &s);
        s.patch.bank = banks[i];
        strcpy(s.name, tiles_wt_bank_name(banks[i]));
        tiles_preset_encode(&s, s_rec);
        CHECK(tiles_preset_put((uint8_t)(70u + i), s_rec, s_staging) == TILES_PRESET_OK, "put %u", i);
    }
    enum { TALK = 70, GROWL, FOLD, BITS, SWEEP, ORGAN };
    tiles_audio_engine_init(48u, 230u, NULL); /* acid, pluck, haze (classic), glass */
    CHECK(slots_hold("classic") && slots_hold("glass"), "the tracks' banks after init");

    tiles_audio_engine_set_patch(0u, GROWL);
    tiles_audio_engine_set_patch(1u, FOLD);
    CHECK(slots_hold("growl") && slots_hold("fold") && slots_hold("glass"), "growl, fold and glass in RAM");
    CHECK(tiles_audio_engine_sound_load(2u, BITS) == TILES_SOUND_REFUSED, "a fourth bank with every slot held");

    tiles_audio_engine_set_patch(3u, 0u); /* acid: glass is free */
    CHECK(tiles_audio_engine_sound_load(2u, BITS) == TILES_SOUND_BUILDING, "bits doesn't wait for its bank");
    CHECK(strcmp(tiles_audio_engine_track_name(2u), "haze") == 0, "the old sound plays on while it builds");
    service_until_ready(2u);
    CHECK(strcmp(tiles_audio_engine_track_name(2u), "bits") == 0 && slots_hold("bits") && !slots_hold("glass"),
          "track 3 on bits, glass gone");

    /* Track 1 (growl) asks for sweep: only its own bank can go. */
    note(0u, true);
    CHECK(loudness(4800u) > 1e-3f, "growl is silent");
    CHECK(tiles_audio_engine_sound_load(0u, SWEEP) == TILES_SOUND_READY, "sweep didn't switch at once");
    tiles_audio_engine_sound_use(0u);
    for (uint32_t i = 0; i < 50u; i++) {
        (void)tiles_audio_engine_service(UINT32_MAX);
    }
    CHECK(slots_hold("growl"), "growl went while its voice still faded");
    note(0u, false);
    (void)loudness(960u); /* the fade */
    for (uint32_t i = 0; i < 10000u && !slots_hold("sweep"); i++) {
        (void)tiles_audio_engine_service(UINT32_MAX);
    }
    for (uint32_t i = 0; i < 10000u && slots_hold("+"); i++) {
        (void)tiles_audio_engine_service(UINT32_MAX);
    }
    CHECK(slots_hold("sweep") && !slots_hold("growl"), "sweep built in growl's place");
    note(0u, true);
    CHECK(loudness(4800u) > 1e-3f, "sweep is silent");
    note(0u, false);

    /* The bench builds its bank when there is room, and says 0 when not. */
    CHECK(tiles_audio_engine_bench(ORGAN, 480u, fake_cycles) == 0u, "the bench found room that isn't there");
    tiles_audio_engine_set_patch(1u, 6u); /* pluck: fold is free */
    CHECK(tiles_audio_engine_bench(ORGAN, 480u, fake_cycles) > 0u && slots_hold("organ"), "the bench on organ");
    char slots[64];
    tiles_audio_engine_wt_slots(slots, sizeof(slots));
    printf("  swapping: built while playing, refused when full, old bank kept until its voices end (%s)\n", slots);
}

int main(void) {
    test_round_trip();
    test_store();
    test_swapping();
    if (s_failures != 0) {
        printf("test_presets: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_presets: ok\n");
    return 0;
}
