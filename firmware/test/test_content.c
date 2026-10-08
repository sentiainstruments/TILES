/*
 * profiles/content.c -- the content store (custom scales) on a simulated
 * flash region: push, replace, delete, survive a reboot, refuse bad input,
 * keep records it doesn't understand, stay read-only on a newer format,
 * change nothing when a save fails. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "content.h"
#include "note_map.h"

static uint8_t g_mem[TILES_KV_NUM_SLOTS][TILES_KV_SECTOR_SIZE];
static bool g_fail_erase;
static int g_erases;
static bool sim_read(uint8_t s, uint32_t o, uint8_t *b, uint32_t l) { memcpy(b, &g_mem[s][o], l); return true; }
static bool sim_erase(uint8_t s) { if (g_fail_erase) return false; g_erases++; memset(g_mem[s], 0xFF, TILES_KV_SECTOR_SIZE); return true; }
static bool sim_program(uint8_t s, uint32_t o, const uint8_t *b, uint32_t l) {
    for (uint32_t i = 0; i < l; i++) g_mem[s][o + i] &= b[i];
    return true;
}
static const tiles_kv_ops_t OPS = {sim_read, sim_erase, sim_program};

static void blank(void) { memset(g_mem, 0xFF, sizeof(g_mem)); g_fail_erase = false; g_erases = 0; }
static void reboot(void) { tiles_note_map_init(false); tiles_content_init(&OPS); }

static tiles_content_scale_t scale(uint8_t slot, const char *name, const int8_t *iv, uint8_t n) {
    tiles_content_scale_t s;
    memset(&s, 0, sizeof(s));
    s.slot = slot;
    s.count = n;
    memcpy(s.intervals, iv, n);
    strcpy(s.name, name);
    return s;
}

/* Writes a raw blob straight into the region, as another firmware would. */
static void raw_blob(const uint8_t *blob, uint16_t len, uint16_t version) {
    tiles_kv_t kv;
    tiles_kv_init(&kv, &OPS);
    assert(tiles_kv_write(&kv, blob, len, version) == TILES_KV_OK);
}

static const int8_t HIRAJOSHI[] = {0, 2, 3, 7, 8};

int main(void) {
    tiles_content_scale_t got;
    tiles_content_record_t rec;

    /* 1. blank flash: nothing pushed, custom slots unavailable */
    blank(); reboot();
    assert(!tiles_content_get_scale(1, &got));
    assert(!tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_1));
    assert(tiles_content_get_info().used_bytes == 0 && tiles_content_get_info().records == 0);

    /* 2. push one: saved, applied to the note map, and it plays */
    tiles_content_scale_t h = scale(1, "Hirajoshi", HIRAJOSHI, 5);
    strcpy(h.pack, "japan"); strcpy(h.item, "hirajoshi"); h.version = 3;
    assert(tiles_content_put_scale(&h) == TILES_CONTENT_OK);
    assert(tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_1));
    assert(!tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_2));
    tiles_note_map_set_scale(TILES_SCALE_CUSTOM_1);
    assert(tiles_note_map_get_note(19) == TILES_NOTE_MAP_BASE_NOTE);
    assert(tiles_note_map_get_note(20) == TILES_NOTE_MAP_BASE_NOTE + 2);
    assert(tiles_note_map_get_note(23) == TILES_NOTE_MAP_BASE_NOTE + 8);
    assert(tiles_note_map_get_note(24) == TILES_NOTE_MAP_BASE_NOTE + 12);

    /* 3. survives a reboot, fields intact */
    reboot();
    assert(tiles_content_get_scale(1, &got));
    assert(got.count == 5 && memcmp(got.intervals, HIRAJOSHI, 5) == 0 && got.version == 3);
    assert(strcmp(got.name, "Hirajoshi") == 0 && strcmp(got.pack, "japan") == 0 && strcmp(got.item, "hirajoshi") == 0);
    assert(tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_1));
    assert(tiles_content_record_at(0, &rec) && rec.type == TILES_CONTENT_TYPE_SCALE && rec.slot == 1 && rec.version == 3);
    assert(strcmp(rec.pack, "japan") == 0 && !tiles_content_record_at(1, &rec));

    /* 4. replacing a slot keeps one record */
    static const int8_t MAJ_PENT[] = {0, 2, 4, 7, 9};
    tiles_content_scale_t p = scale(1, "Pent", MAJ_PENT, 5);
    assert(tiles_content_put_scale(&p) == TILES_CONTENT_OK);
    assert(tiles_content_get_info().records == 1 && tiles_content_get_info().scales == 1);
    assert(tiles_content_get_scale(1, &got) && got.pack[0] == '\0' && got.intervals[2] == 4);

    /* 5. bad input is refused without touching flash */
    int erases = g_erases;
    tiles_content_scale_t bad = scale(10, "X", MAJ_PENT, 5);
    assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_SLOT);
    bad = scale(0, "X", MAJ_PENT, 5);
    assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_SLOT);
    static const int8_t NO_ROOT[] = {2, 4, 7};
    static const int8_t UNSORTED[] = {0, 4, 2};
    static const int8_t OCTAVE[] = {0, 4, 12};
    bad = scale(2, "X", NO_ROOT, 3); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_INTERVALS);
    bad = scale(2, "X", UNSORTED, 3); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_INTERVALS);
    bad = scale(2, "X", OCTAVE, 3); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_INTERVALS);
    bad = scale(2, "X", MAJ_PENT, 0); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_INTERVALS);
    bad = scale(2, "", MAJ_PENT, 5); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_TEXT);
    bad = scale(2, "has space", MAJ_PENT, 5); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_TEXT);
    bad = scale(2, "ok", MAJ_PENT, 5); strcpy(bad.pack, "tab\there"); assert(tiles_content_put_scale(&bad) == TILES_CONTENT_BAD_TEXT);
    assert(g_erases == erases);
    assert(tiles_content_text_valid("Raga_Bhairav-2", false) && !tiles_content_text_valid("seventeen_chars_x", false));

    /* 6. delete empties the slot (and the note map); deleting nothing doesn't write */
    assert(tiles_content_delete_scale(1) == TILES_CONTENT_OK);
    assert(!tiles_content_get_scale(1, &got) && !tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_1));
    assert(tiles_note_map_get_note(20) == TILES_NOTE_MAP_BASE_NOTE + 1); /* selected but empty: chromatic fallback */
    erases = g_erases;
    assert(tiles_content_delete_scale(1) == TILES_CONTENT_OK && g_erases == erases);
    assert(tiles_content_delete_scale(10) == TILES_CONTENT_BAD_SLOT);
    reboot(); assert(!tiles_content_get_scale(1, &got));

    /* 7. all nine slots at the largest size fit */
    static const int8_t CHROM[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    for (uint8_t slot = 1; slot <= 9; slot++) {
        tiles_content_scale_t big = scale(slot, "sixteen_chars_ab", CHROM, 12);
        strcpy(big.pack, "sixteen_chars_ab"); strcpy(big.item, "sixteen_chars_ab"); big.version = 65535;
        assert(tiles_content_put_scale(&big) == TILES_CONTENT_OK);
    }
    reboot();
    assert(tiles_content_get_info().scales == 9);
    for (uint8_t slot = 1; slot <= 9; slot++) assert(tiles_note_map_scale_is_defined((tiles_scale_mode_t)(TILES_SCALE_CUSTOM_1 + slot - 1)));

    /* 8. a record of a type this firmware doesn't know is kept, never applied */
    blank();
    uint8_t blob[64]; uint16_t n = 0;
    blob[n++] = 7; blob[n++] = 2; blob[n++] = 3; blob[n++] = 0; blob[n++] = 0xAA; blob[n++] = 0xBB; blob[n++] = 0xCC;
    raw_blob(blob, n, TILES_CONTENT_BLOB_VERSION);
    reboot();
    assert(tiles_content_get_info().records == 1 && tiles_content_get_info().scales == 0);
    tiles_content_scale_t one = scale(4, "Four", HIRAJOSHI, 5);
    assert(tiles_content_put_scale(&one) == TILES_CONTENT_OK);
    reboot();
    assert(tiles_content_get_info().records == 2);
    assert(tiles_content_record_at(0, &rec) && rec.type == 7 && rec.slot == 2 && rec.bytes == 3);
    assert(tiles_content_record_at(1, &rec) && rec.type == TILES_CONTENT_TYPE_SCALE && rec.slot == 4);

    /* 9. a damaged scale record leaves its slot empty; a truncated tail is ignored */
    blank(); n = 0;
    blob[n++] = TILES_CONTENT_TYPE_SCALE; blob[n++] = 3; blob[n++] = 7; blob[n++] = 0;
    blob[n++] = 1; blob[n++] = 0; blob[n++] = 2; blob[n++] = 5; blob[n++] = 2; blob[n++] = 1; blob[n++] = 'x'; /* intervals 5,2 */
    blob[n++] = TILES_CONTENT_TYPE_SCALE; blob[n++] = 4; blob[n++] = 40; blob[n++] = 0; /* body runs past the end */
    raw_blob(blob, n, TILES_CONTENT_BLOB_VERSION);
    reboot();
    assert(!tiles_content_get_scale(3, &got) && !tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_3));
    assert(!tiles_content_get_scale(4, &got) && tiles_content_get_info().records == 1);

    /* 10. a newer firmware's blob: not applied, not overwritten, until CLEAR */
    blank();
    raw_blob(blob, 4, TILES_CONTENT_BLOB_VERSION + 1);
    reboot();
    assert(tiles_content_get_info().newer_format);
    erases = g_erases;
    assert(tiles_content_put_scale(&one) == TILES_CONTENT_NEWER_FORMAT);
    assert(tiles_content_delete_scale(4) == TILES_CONTENT_NEWER_FORMAT);
    assert(g_erases == erases);
    assert(tiles_content_clear() == TILES_CONTENT_OK && !tiles_content_get_info().newer_format);
    assert(tiles_content_put_scale(&one) == TILES_CONTENT_OK);

    /* 11. a failed save changes nothing */
    g_fail_erase = true;
    tiles_content_scale_t two = scale(5, "Five", MAJ_PENT, 5);
    assert(tiles_content_put_scale(&two) == TILES_CONTENT_SAVE_FAILED);
    assert(!tiles_content_get_scale(5, &got) && !tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_5));
    assert(tiles_content_delete_scale(4) == TILES_CONTENT_SAVE_FAILED && tiles_content_get_scale(4, &got));
    g_fail_erase = false;
    reboot(); assert(tiles_content_get_scale(4, &got) && !tiles_content_get_scale(5, &got));

    /* 12. clear wipes everything */
    assert(tiles_content_clear() == TILES_CONTENT_OK);
    reboot(); assert(tiles_content_get_info().records == 0 && !tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_4));

    /* 13. no flash region: starts empty, refuses changes */
    tiles_note_map_init(false); tiles_content_init(NULL);
    assert(tiles_content_put_scale(&one) == TILES_CONTENT_NO_STORAGE);
    assert(!tiles_content_get_info().storage && !tiles_note_map_scale_is_defined(TILES_SCALE_CUSTOM_4));

    printf("content: all tests pass\n");
    return 0;
}
