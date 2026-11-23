/* The presets (audio/presets.c): every factory sound through the record and
 * back, bad records refused, and the store over a simulated flash: a record
 * written, read after a restart, cleared back to the factory sound. */

#include "audio_engine.h"
#include "presets.h"

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
    CHECK(n == 20u, "%u factory sounds, want 20", n);
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
    CHECK(!tiles_preset_stored(30u) && !tiles_preset_get(30u, &s), "slot 31 not empty");
    tiles_preset_get(6u, &s); /* pluck, into slot 31 as "mine" */
    strcpy(s.name, "mine");
    s.patch.cutoff_hz = 777.0f;
    tiles_preset_encode(&s, s_rec);
    CHECK(tiles_preset_put(30u, s_rec, s_staging) == TILES_PRESET_OK, "put failed");
    tiles_preset_encode(&s, s_rec);
    strcpy(s.name, "acid2");
    tiles_preset_encode(&s, s_rec2);
    CHECK(tiles_preset_put(0u, s_rec2, s_staging) == TILES_PRESET_OK, "put over a factory slot failed");
    tiles_presets_init(k_ops); /* a restart */
    char name[TILES_PRESET_NAME_MAX + 1u];
    tiles_preset_name(30u, name);
    CHECK(strcmp(name, "mine") == 0 && tiles_preset_get(30u, &s) && s.patch.cutoff_hz == 777.0f,
          "slot 31 after a restart: '%s'", name);
    tiles_preset_name(0u, name);
    CHECK(strcmp(name, "acid2") == 0, "slot 1 after a restart: '%s'", name);
    tiles_preset_name(6u, name);
    CHECK(strcmp(name, "pluck") == 0, "a neighbour changed: '%s'", name);
    CHECK(tiles_preset_clear(0u, s_staging) == TILES_PRESET_OK, "clear failed");
    tiles_presets_init(k_ops);
    tiles_preset_name(0u, name);
    CHECK(strcmp(name, "acid") == 0 && tiles_preset_stored(30u), "after clear: '%s'", name);
    memset(s_rec, 0, sizeof(s_rec));
    CHECK(tiles_preset_put(40u, s_rec, s_staging) == TILES_PRESET_ERR_RECORD, "a blank record stored");
    /* Through the engine: track 2 plays slot 31. */
    tiles_audio_engine_init(48u, 230u, NULL);
    tiles_audio_engine_set_patch(1u, 30u);
    CHECK(tiles_audio_engine_patch(1u) == 30u && strcmp(tiles_audio_engine_track_name(1u), "mine") == 0,
          "track 2 is '%s'", tiles_audio_engine_track_name(1u));
    printf("  store: put, restart, a factory slot overridden and cleared, the engine plays a stored one\n");
}

int main(void) {
    test_round_trip();
    test_store();
    if (s_failures != 0) {
        printf("test_presets: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("test_presets: ok\n");
    return 0;
}
