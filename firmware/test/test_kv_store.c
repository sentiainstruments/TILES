/* storage/kv_store.c against a simulated NOR flash, including a power cut after
 * every erase/program step and partially-applied operations. */
#include "kv_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_mem[TILES_KV_NUM_SLOTS][TILES_KV_SECTOR_SIZE];
/* Power-loss injection: when > 0, the op that brings it to 0 is only partially applied and every later
 * op fails. -1 = never lose power. */
static int g_ops_until_cut = -1;
static bool g_dead;
static int g_partial_bytes = 0; /* how much of the cut op lands: bytes for program; for erase, bytes erased */
static int g_op_count;

static bool consume_op(void) {
    g_op_count++;
    if (g_dead) return false;
    if (g_ops_until_cut == 0) return false;   /* already cut (shouldn't reach) */
    if (g_ops_until_cut > 0 && --g_ops_until_cut == 0) { g_dead = true; return false; } /* this op is the one cut short */
    return true;
}

static bool sim_read(uint8_t slot, uint32_t off, uint8_t *buf, uint32_t len) {
    assert(slot < TILES_KV_NUM_SLOTS && off + len <= TILES_KV_SECTOR_SIZE);
    memcpy(buf, &g_mem[slot][off], len);
    return true;
}
static bool sim_erase(uint8_t slot) {
    bool ok = consume_op();
    if (!ok) { if (g_dead) memset(g_mem[slot], 0xFF, (size_t)g_partial_bytes); return false; }
    memset(g_mem[slot], 0xFF, TILES_KV_SECTOR_SIZE);
    return true;
}
static bool sim_program(uint8_t slot, uint32_t off, const uint8_t *buf, uint32_t len) {
    assert(off % TILES_KV_PAGE_SIZE == 0 && len % TILES_KV_PAGE_SIZE == 0 && off + len <= TILES_KV_SECTOR_SIZE);
    bool ok = consume_op();
    uint32_t n = ok ? len : (g_dead ? (uint32_t)g_partial_bytes : 0u);
    if (n > len) n = len;
    for (uint32_t i = 0; i < n; i++) {
        /* NOR rule: programming can only clear bits. A page written onto non-erased data is a logic bug. */
        assert((g_mem[slot][off + i] & buf[i]) == buf[i] && "programmed over non-erased flash");
        g_mem[slot][off + i] &= buf[i];
    }
    return ok;
}
static const tiles_kv_ops_t OPS = {sim_read, sim_erase, sim_program};

static void blank(void) { memset(g_mem, 0xFF, sizeof(g_mem)); g_ops_until_cut = -1; g_dead = false; g_op_count = 0; }
static void reboot(void) { g_ops_until_cut = -1; g_dead = false; tiles_kv_init(&OPS); }

static void make_payload(uint8_t *p, uint16_t len, uint8_t tag) { for (uint16_t i = 0; i < len; i++) p[i] = (uint8_t)(tag * 31u + i * 7u); }
static bool payload_is(uint16_t len, uint8_t tag, uint16_t want_version) {
    uint8_t out[TILES_KV_MAX_PAYLOAD], want[TILES_KV_MAX_PAYLOAD]; uint16_t got_len, ver;
    if (!tiles_kv_read(out, sizeof(out), &got_len, &ver)) return false;
    make_payload(want, len, tag);
    return got_len == len && ver == want_version && memcmp(out, want, len) == 0;
}

int main(void) {
    uint8_t buf[TILES_KV_MAX_PAYLOAD];
    uint8_t out[TILES_KV_MAX_PAYLOAD]; uint16_t len, ver;

    /* CRC-32 known-answer test (zlib): "123456789" -> 0xCBF43926 */
    assert(tiles_kv_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u);

    /* 1. blank flash: nothing valid; first save goes to slot 0 */
    blank(); tiles_kv_init(&OPS);
    assert(!tiles_kv_read(out, sizeof(out), &len, &ver));
    assert(tiles_kv_get_info().newest == -1);
    make_payload(buf, 100, 1);
    assert(tiles_kv_write(buf, 100, 7) == TILES_KV_OK);
    assert(tiles_kv_get_info().newest == 0 && tiles_kv_get_info().seq[0] == 1);
    assert(payload_is(100, 1, 7));
    reboot(); assert(payload_is(100, 1, 7));                    /* survives a reboot */

    /* 2. saves alternate slots and seq climbs; the newest always wins */
    for (int i = 2; i <= 9; i++) {
        make_payload(buf, (uint16_t)(50 * i), (uint8_t)i);
        assert(tiles_kv_write(buf, (uint16_t)(50 * i), 7) == TILES_KV_OK);
        assert(tiles_kv_get_info().newest == (i - 1) % 2 && tiles_kv_get_info().seq[(i - 1) % 2] == (uint32_t)i);
    }
    reboot(); assert(payload_is(450, 9, 7) && tiles_kv_get_info().newest == 0);

    /* 3. sizes: empty, one full page boundary cases, and the maximum */
    uint16_t sizes[] = {0, 1, 235, 236, 237, 491, 492, 2000, TILES_KV_MAX_PAYLOAD};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        make_payload(buf, sizes[s], (uint8_t)(s + 1));
        assert(tiles_kv_write(buf, sizes[s], 3) == TILES_KV_OK);
        assert(payload_is(sizes[s], (uint8_t)(s + 1), 3));
        reboot(); assert(payload_is(sizes[s], (uint8_t)(s + 1), 3));
    }
    assert(tiles_kv_write(buf, TILES_KV_MAX_PAYLOAD + 1, 3) == TILES_KV_ERR_TOO_BIG);
    assert(payload_is(TILES_KV_MAX_PAYLOAD, (uint8_t)(sizeof(sizes) / sizeof(sizes[0])), 3)); /* untouched */
    /* read into a buffer that is too small fails cleanly instead of overrunning */
    assert(!tiles_kv_read(out, 10, &len, &ver));

    /* 4. POWER LOSS: cut the power at every single erase/program step of a save, with several
     *    partial-application amounts, and check the store always comes back as the OLD or the NEW
     *    payload -- never garbage, never empty -- and keeps working afterwards. */
    int total_ops;
    {   /* count the ops one save of a 1000-byte payload takes */
        blank(); tiles_kv_init(&OPS);
        make_payload(buf, 300, 1); assert(tiles_kv_write(buf, 300, 1) == TILES_KV_OK);
        g_op_count = 0; make_payload(buf, 1000, 2); assert(tiles_kv_write(buf, 1000, 1) == TILES_KV_OK);
        total_ops = g_op_count;  /* 1 erase + 4 programs: (20 + 1000) bytes = 4 pages */
        assert(total_ops == 1 + 4);
    }
    int partials[] = {0, 1, 100, 255};
    int checked = 0, got_old = 0, got_new = 0;
    for (int cut = 1; cut <= total_ops + 1; cut++) {   /* +1: no cut during the save at all */
        for (unsigned pi = 0; pi < sizeof(partials) / sizeof(partials[0]); pi++) {
            blank(); tiles_kv_init(&OPS);
            make_payload(buf, 300, 1); assert(tiles_kv_write(buf, 300, 1) == TILES_KV_OK);   /* OLD: slot 0 */
            g_ops_until_cut = cut; g_dead = false; g_partial_bytes = partials[pi];
            make_payload(buf, 1000, 2);
            (void)tiles_kv_write(buf, 1000, 1);                                                /* NEW: cut short */
            reboot();
            bool old_ok = payload_is(300, 1, 1), new_ok = payload_is(1000, 2, 1);
            assert(old_ok || new_ok);
            assert(!(old_ok && new_ok));
            if (old_ok) got_old++; else got_new++;
            checked++;
            /* and the store is still fully usable: the next save succeeds and is what boots */
            make_payload(buf, 64, 3); assert(tiles_kv_write(buf, 64, 1) == TILES_KV_OK);
            assert(payload_is(64, 3, 1)); reboot(); assert(payload_is(64, 3, 1));
        }
    }
    assert(got_old > 0 && got_new > 0);   /* a cut anywhere in the save keeps the old payload; no cut lands the new one */
    assert(got_new == 4);                 /* exactly the no-cut scenarios (one per partial-amount) */
    printf("kv_store: %d power-cut scenarios ok (old kept: %d, new landed: %d)\n", checked, got_old, got_new);

    /* 5. cut in the middle of the ERASE of the slot that is about to be reused: old data survives */
    blank(); tiles_kv_init(&OPS);
    make_payload(buf, 200, 1); assert(tiles_kv_write(buf, 200, 1) == TILES_KV_OK);    /* slot 0, seq 1 */
    make_payload(buf, 200, 2); assert(tiles_kv_write(buf, 200, 1) == TILES_KV_OK);    /* slot 1, seq 2 */
    g_ops_until_cut = 1; g_dead = false; g_partial_bytes = 2000;                       /* erase of slot 0 dies half way */
    make_payload(buf, 200, 3); (void)tiles_kv_write(buf, 200, 1);
    reboot(); assert(payload_is(200, 2, 1));                                          /* seq 2 in slot 1 still wins */

    /* 6. corruption: a flipped payload bit invalidates that slot; the other slot takes over */
    blank(); tiles_kv_init(&OPS);
    make_payload(buf, 200, 1); assert(tiles_kv_write(buf, 200, 1) == TILES_KV_OK);    /* slot 0 */
    make_payload(buf, 200, 2); assert(tiles_kv_write(buf, 200, 1) == TILES_KV_OK);    /* slot 1 (newest) */
    g_mem[1][TILES_KV_HEADER_SIZE + 50] ^= 0x04;
    reboot(); assert(payload_is(200, 1, 1) && tiles_kv_get_info().newest == 0 && !tiles_kv_get_info().valid[1]);
    g_mem[0][TILES_KV_HEADER_SIZE + 3] ^= 0x80;
    reboot(); assert(!tiles_kv_read(out, sizeof(out), &len, &ver) && tiles_kv_get_info().newest == -1);
    /* and it recovers: with nothing valid the next save lands in slot 0 */
    make_payload(buf, 40, 9); assert(tiles_kv_write(buf, 40, 1) == TILES_KV_OK); assert(payload_is(40, 9, 1));

    /* 7. garbage that happens to carry the magic but a wrong length / CRC is rejected */
    blank();
    memset(g_mem[0], 0, 32); g_mem[0][0] = 0x54; g_mem[0][1] = 0x4b; g_mem[0][2] = 0x53; g_mem[0][3] = 0x31;
    g_mem[0][4] = 1; g_mem[0][12] = 0xFF; g_mem[0][13] = 0xFF;   /* len 65535 */
    tiles_kv_init(&OPS); assert(tiles_kv_get_info().newest == -1);

    printf("kv_store: all tests pass\n");
    return 0;
}
