/* The DX7 bank store (audio/fm_bank.c) over a fake flash: a bank streams
 * in, survives a reload, and every bad load leaves no bank. */

#include "fm_bank.h"

#include <stdio.h>
#include <stdlib.h>
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

/* Two 4 KB sectors; programming can only clear bits, as on NOR flash. */
static uint8_t s_flash[2][4096];

static bool f_read(uint8_t slot, uint32_t offset, uint8_t *buf, uint32_t len) {
    memcpy(buf, &s_flash[slot][offset], len);
    return true;
}
static bool f_erase(uint8_t slot) {
    memset(s_flash[slot], 0xFF, sizeof(s_flash[slot]));
    return true;
}
static bool f_program(uint8_t slot, uint32_t offset, const uint8_t *buf, uint32_t len) {
    if (offset % 256u != 0u || len % 256u != 0u || offset + len > 4096u) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        s_flash[slot][offset + i] &= buf[i];
    }
    return true;
}
static const tiles_kv_ops_t k_ops = {f_read, f_erase, f_program};

static uint8_t s_sysex[TILES_DX7_BANK_SYSEX_BYTES];
static uint8_t s_voices[TILES_DX7_BANK_VOICES][TILES_DX7_VOICE_BYTES];

/* A bank of the factory FM voices and variations of them, named VOICE n. */
static void make_bank(void) {
    static const uint8_t head[6] = {0xF0, 0x43, 0x05, 0x09, 0x20, 0x00}; /* MIDI channel 6 */
    memcpy(s_sysex, head, 6);
    uint32_t sum = 0;
    for (uint32_t n = 0; n < TILES_DX7_BANK_VOICES; n++) {
        uint8_t *v = s_voices[n];
        memcpy(v, g_fm_factory[n % TILES_FM_FACTORY_COUNT].dx7, TILES_DX7_VOICE_BYTES);
        v[TILES_DX7_ALGORITHM] = (uint8_t)(n % 32u);
        v[5 * TILES_DX7_OP_BYTES + 16] = (uint8_t)(60u + n); /* operator 1's output level */
        char name[11];
        snprintf(name, sizeof(name), "VOICE %-4u", (unsigned)n + 1u);
        memcpy(&v[TILES_DX7_NAME], name, 10);
        tiles_dx7_pack(v, &s_sysex[6u + n * TILES_DX7_PACKED_BYTES]);
    }
    for (uint32_t i = 0; i < 4096u; i++) {
        sum += s_sysex[6u + i];
    }
    s_sysex[4102] = (uint8_t)((128u - (sum & 127u)) & 127u);
    s_sysex[4103] = 0xF7;
}

/* Streams `bytes` of `sysex` in uneven chunks, as USB delivers them. */
static tiles_fm_bank_result_t load(const uint8_t *sysex, uint32_t bytes, uint32_t crc) {
    tiles_fm_bank_result_t r = tiles_fm_bank_begin(crc);
    if (r != TILES_FM_BANK_OK) {
        return r;
    }
    uint32_t at = 0;
    while (at < bytes) {
        uint32_t n = 1u + (uint32_t)(rand() % 300);
        n = at + n > bytes ? bytes - at : n;
        (void)tiles_fm_bank_write(&sysex[at], n);
        at += n;
    }
    return tiles_fm_bank_finish();
}

static void test_good_bank(void) {
    uint32_t crc = tiles_kv_crc32(0u, s_sysex, sizeof(s_sysex));
    tiles_fm_bank_result_t r = load(s_sysex, sizeof(s_sysex), crc);
    CHECK(r == TILES_FM_BANK_OK, "a good bank: %s", tiles_fm_bank_result_name(r));
    CHECK(tiles_fm_bank_present(), "no bank after a good load");
    char name[11];
    CHECK(tiles_fm_bank_name(0, name) && strcmp(name, "VOICE 1") == 0, "voice 1 is '%s'", name);
    CHECK(tiles_fm_bank_name(31, name) && strcmp(name, "VOICE 32") == 0, "voice 32 is '%s'", name);
    for (uint8_t n = 0; n < TILES_DX7_BANK_VOICES; n++) {
        uint8_t v[TILES_DX7_VOICE_BYTES];
        CHECK(tiles_fm_bank_voice(n, v) && memcmp(v, s_voices[n], sizeof(v)) == 0, "voice %u changed on the way",
              (unsigned)n + 1u);
    }
    /* After a restart it reads it back from flash. */
    tiles_fm_bank_init(&k_ops);
    CHECK(tiles_fm_bank_present() && tiles_fm_bank_name(4, name) && strcmp(name, "VOICE 5") == 0,
          "the bank isn't there after a restart");
}

static void test_bad_banks(void) {
    uint8_t bad[TILES_DX7_BANK_SYSEX_BYTES];
    uint32_t crc;

    memcpy(bad, s_sysex, sizeof(bad));
    bad[3] = 0x00; /* a single-voice dump's header */
    crc = tiles_kv_crc32(0u, bad, sizeof(bad));
    CHECK(load(bad, sizeof(bad), crc) == TILES_FM_BANK_ERR_NOT_DX7 && !tiles_fm_bank_present(), "took a non-bank");

    memcpy(bad, s_sysex, sizeof(bad));
    bad[1000] ^= 0x01; /* a voice byte changed, the checksum not */
    crc = tiles_kv_crc32(0u, bad, sizeof(bad));
    CHECK(load(bad, sizeof(bad), crc) == TILES_FM_BANK_ERR_CHECKSUM && !tiles_fm_bank_present(),
          "took a bad DX7 checksum");

    crc = tiles_kv_crc32(0u, s_sysex, sizeof(s_sysex)) ^ 1u;
    CHECK(load(s_sysex, sizeof(s_sysex), crc) == TILES_FM_BANK_ERR_CRC && !tiles_fm_bank_present(),
          "took bytes whose CRC-32 doesn't match");

    crc = tiles_kv_crc32(0u, s_sysex, sizeof(s_sysex));
    CHECK(load(s_sysex, 2000u, crc) == TILES_FM_BANK_ERR_NOT_DX7 && !tiles_fm_bank_present(), "took half a bank");
    tiles_fm_bank_init(&k_ops);
    CHECK(!tiles_fm_bank_present(), "half a bank came back after a restart");

    CHECK(tiles_fm_bank_begin(0u) == TILES_FM_BANK_OK && tiles_fm_bank_begin(0u) == TILES_FM_BANK_ERR_STATE,
          "a second load started over the first");
    (void)tiles_fm_bank_finish();
}

int main(void) {
    memset(s_flash, 0xFF, sizeof(s_flash));
    srand(3);
    make_bank();
    tiles_fm_bank_init(&k_ops);
    CHECK(!tiles_fm_bank_present(), "a bank in blank flash");
    test_good_bank();
    test_bad_banks();
    tiles_fm_bank_init(NULL);
    CHECK(tiles_fm_bank_begin(0u) == TILES_FM_BANK_ERR_NO_STORAGE, "loaded without storage");
    if (s_failures == 0) {
        printf("test_fm_bank: ok\n");
        return 0;
    }
    printf("test_fm_bank: %d failure(s)\n", s_failures);
    return 1;
}
