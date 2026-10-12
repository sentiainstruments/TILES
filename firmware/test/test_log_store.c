/*
 * storage/log_store.c -- the append-only record log: appends without
 * erasing, switches sectors, waits for an erase only when it must, and
 * survives a power cut at every flash step. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "log_store.h"

static uint8_t g_mem[TILES_KV_NUM_SLOTS][TILES_KV_SECTOR_SIZE];
static int g_ops_until_cut = -1; /* -1 = no cut */
static bool g_dead;
static int g_erases, g_programs;

static bool consume(void) {
    if (g_dead) return false;
    if (g_ops_until_cut == 0) { g_dead = true; return false; }
    if (g_ops_until_cut > 0) g_ops_until_cut--;
    return true;
}
static bool sim_read(uint8_t s, uint32_t o, uint8_t *b, uint32_t l) { memcpy(b, &g_mem[s][o], l); return true; }
static bool sim_erase(uint8_t s) {
    if (!consume()) { memset(g_mem[s], 0xFF, 1000); return false; } /* a cut erase: part of it done */
    memset(g_mem[s], 0xFF, TILES_KV_SECTOR_SIZE); g_erases++; return true;
}
static bool sim_program(uint8_t s, uint32_t o, const uint8_t *b, uint32_t l) {
    /* The log only ever programs erased pages. */
    for (uint32_t i = 0; i < l; i++) assert(g_mem[s][o + i] == 0xFF);
    if (!consume()) { for (uint32_t i = 0; i < l / 2; i++) g_mem[s][o + i] &= b[i]; return false; } /* half a page */
    for (uint32_t i = 0; i < l; i++) g_mem[s][o + i] &= b[i];
    g_programs++;
    return true;
}
static const tiles_kv_ops_t OPS = {sim_read, sim_erase, sim_program};
static tiles_log_t g_log;

static void blank(void) { memset(g_mem, 0xFF, sizeof(g_mem)); g_ops_until_cut = -1; g_dead = false; g_erases = g_programs = 0; }
static void reboot(void) { g_ops_until_cut = -1; g_dead = false; tiles_log_init(&g_log, &OPS); }
static void make(uint8_t *p, uint16_t n, uint8_t tag) { for (uint16_t i = 0; i < n; i++) p[i] = (uint8_t)(tag * 37u + i * 11u); }
static bool newest_is(uint16_t n, uint8_t tag) {
    uint8_t out[TILES_LOG_MAX_PAYLOAD], want[TILES_LOG_MAX_PAYLOAD]; uint16_t len, ver;
    if (!tiles_log_read(&g_log, out, sizeof(out), &len, &ver) || len != n || ver != 2) return false;
    make(want, n, tag);
    return memcmp(out, want, n) == 0;
}

int main(void) {
    uint8_t buf[TILES_LOG_MAX_PAYLOAD], out[64];
    uint16_t len;

    /* 1. blank: nothing to read; appends never erase while there's room */
    blank(); reboot();
    assert(!tiles_log_read(&g_log, out, sizeof(out), &len, NULL));
    for (int i = 1; i <= 16; i++) {                               /* 16 one-page records fill sector 0 */
        make(buf, 100, (uint8_t)i);
        assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_OK);
        assert(newest_is(100, (uint8_t)i));
    }
    assert(g_erases == 0 && g_log.newest_slot == 0);
    make(buf, 100, 17);                                           /* sector 1 is blank: still no erase */
    assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_OK && g_log.newest_slot == 1 && g_erases == 0);
    reboot(); assert(newest_is(100, 17));

    /* 2. both used: the switch back waits for an erase, unless allowed */
    for (int i = 18; i <= 32; i++) { make(buf, 100, (uint8_t)i); assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_OK); }
    assert(!tiles_log_fits_without_erase(&g_log, 100));
    make(buf, 100, 33);
    assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_ERR_NEEDS_ERASE);
    assert(newest_is(100, 32));                                   /* nothing changed */
    assert(tiles_log_prepare(&g_log) && g_erases == 1);           /* erases sector 0, never sector 1 */
    assert(newest_is(100, 32) && tiles_log_fits_without_erase(&g_log, 100));
    assert(!tiles_log_prepare(&g_log));                           /* already blank */
    assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_OK && g_log.newest_slot == 0);
    reboot(); assert(newest_is(100, 33));
    /* allow_erase does the erase itself */
    blank(); reboot();
    for (int i = 1; i <= 32; i++) { make(buf, 100, (uint8_t)i); assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_OK); }
    make(buf, 100, 40);
    assert(tiles_log_append(&g_log, buf, 100, 2, true) == TILES_LOG_OK && g_erases == 1);
    reboot(); assert(newest_is(100, 40));

    /* 3. multi-page records, a full-size one, and one too big */
    blank(); reboot();
    make(buf, 600, 5); assert(tiles_log_append(&g_log, buf, 600, 2, false) == TILES_LOG_OK);
    assert(g_log.next_free_page[0] == 3);
    make(buf, TILES_LOG_MAX_PAYLOAD, 6); assert(tiles_log_append(&g_log, buf, TILES_LOG_MAX_PAYLOAD, 2, false) == TILES_LOG_OK);
    reboot(); assert(newest_is(TILES_LOG_MAX_PAYLOAD, 6) && g_log.newest_slot == 1);
    assert(tiles_log_append(&g_log, buf, TILES_LOG_MAX_PAYLOAD + 1, 2, true) == TILES_LOG_ERR_TOO_BIG);

    /* 4. a power cut at every step of an append: the old record or the new
     * one, never neither; and the log keeps working afterwards */
    for (int pages = 1; pages <= 3; pages++) {
        uint16_t n = (uint16_t)(pages * 200);
        for (int cut = 0; cut < pages; cut++) {
            blank(); reboot();
            make(buf, 50, 1); assert(tiles_log_append(&g_log, buf, 50, 2, false) == TILES_LOG_OK);
            g_ops_until_cut = cut;
            make(buf, n, 2); (void)tiles_log_append(&g_log, buf, n, 2, false);
            reboot();
            assert(newest_is(50, 1));                                 /* cut before the magic page: old one stays */
            make(buf, 70, 3); assert(tiles_log_append(&g_log, buf, 70, 2, false) == TILES_LOG_OK);
            reboot(); assert(newest_is(70, 3));
        }
    }
    /* a cut during prepare's erase: the newest record is in the other sector */
    blank(); reboot();
    for (int i = 1; i <= 17; i++) { make(buf, 100, (uint8_t)i); assert(tiles_log_append(&g_log, buf, 100, 2, false) == TILES_LOG_OK); }
    g_ops_until_cut = 0; (void)tiles_log_prepare(&g_log);
    reboot(); assert(newest_is(100, 17));
    assert(tiles_log_prepare(&g_log));
    reboot(); assert(newest_is(100, 17));

    /* 5. foreign data in a sector: ignored, never programmed over */
    blank();
    memset(g_mem[0], 0x5A, 300);
    reboot();
    assert(!tiles_log_read(&g_log, out, sizeof(out), &len, NULL) && g_log.next_free_page[0] == 2);
    make(buf, 40, 9); assert(tiles_log_append(&g_log, buf, 40, 2, false) == TILES_LOG_OK);
    reboot(); assert(newest_is(40, 9));

    printf("log_store: all tests pass\n");
    return 0;
}
