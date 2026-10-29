/*
 * services/play_state.c -- mode, scale, key and octave survive a power
 * cycle: saved after a quiet gap with the pads idle, never erasing while
 * the clock runs, restored into the real note map on a fresh boot and left
 * alone on a crash recovery. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "log_store.h"
#include "note_map.h"
#include "play_state.h"

static uint8_t g_mem[TILES_KV_NUM_SLOTS][TILES_KV_SECTOR_SIZE];
static int g_erases, g_programs;

static bool sim_read(uint8_t s, uint32_t o, uint8_t *b, uint32_t l) { memcpy(b, &g_mem[s][o], l); return true; }
static bool sim_erase(uint8_t s) { memset(g_mem[s], 0xFF, TILES_KV_SECTOR_SIZE); g_erases++; return true; }
static bool sim_program(uint8_t s, uint32_t o, const uint8_t *b, uint32_t l) {
    for (uint32_t i = 0; i < l; i++) { assert(g_mem[s][o + i] == 0xFF); g_mem[s][o + i] = b[i]; }
    g_programs++;
    return true;
}
static const tiles_kv_ops_t OPS = {sim_read, sim_erase, sim_program};

/* A power cycle: RAM back to defaults, then boot. */
static void power_cycle(void) {
    tiles_note_map_init(false);
    tiles_play_state_init(&OPS, false);
}

int main(void) {
    uint8_t mode = 0;
    uint32_t t = 0;

    /* 1. blank flash: defaults, no saved mode; the defaults are saved once */
    memset(g_mem, 0xFF, sizeof(g_mem));
    power_cycle();
    assert(!tiles_play_state_boot_mode(&mode));
    assert(tiles_note_map_get_scale() == TILES_SCALE_CHROMATIC);
    tiles_play_state_service(t = 2500, 0, false, true);
    assert(g_programs == 1 && tiles_play_state_get_info().have_saved);
    tiles_play_state_service(t = 2600, 0, false, true);
    assert(g_programs == 1);                                      /* nothing new */

    /* 2. a change waits for a quiet gap and idle pads */
    tiles_note_map_set_scale(TILES_SCALE_DORIAN);
    tiles_note_map_set_octave_shift(-2);
    tiles_note_map_set_key_offset(5);
    tiles_play_state_service(t = 3000, 6, false, true);           /* changed now */
    tiles_play_state_service(t = 4500, 6, false, true);
    assert(g_programs == 1 && tiles_play_state_get_info().pending);
    tiles_play_state_service(t = 5100, 6, false, false);          /* a pad is touched */
    assert(g_programs == 1);
    tiles_play_state_service(t = 5200, 6, false, true);
    assert(g_programs == 2 && !tiles_play_state_get_info().pending);
    tiles_play_state_info_t info = tiles_play_state_get_info();
    assert(info.mode == 6 && info.scale == TILES_SCALE_DORIAN && info.octave == -2 && info.key == 5);

    /* 3. power cycle: all four come back */
    power_cycle();
    assert(tiles_play_state_boot_mode(&mode) && mode == 6);
    assert(tiles_note_map_get_scale() == TILES_SCALE_DORIAN);
    assert(tiles_note_map_get_octave_shift() == -2 && tiles_note_map_get_key_offset() == 5);

    /* 4. crash recovery: RAM wins, the saved mode isn't offered */
    tiles_note_map_set_scale(TILES_SCALE_EGYPTIAN);
    tiles_play_state_init(&OPS, true);
    assert(!tiles_play_state_boot_mode(&mode));
    assert(tiles_note_map_get_scale() == TILES_SCALE_EGYPTIAN);
    assert(tiles_play_state_get_info().have_saved);

    /* 5. many changes while the clock runs: appends only, never an erase;
     * the one that needs an erase waits for the clock to stop */
    power_cycle();
    g_erases = 0;
    int saves_before = (int)tiles_play_state_get_info().saves, waited = 0;
    for (int i = 0; i < 40; i++) {
        tiles_note_map_set_key_offset((int8_t)(i % 12));
        tiles_note_map_set_octave_shift((int8_t)(i % 3));
        t += 100; tiles_play_state_service(t, 0, true, true);
        t += 2100; tiles_play_state_service(t, 0, true, true);
        if (tiles_play_state_get_info().pending) waited++;
    }
    assert(g_erases == 0 && waited > 0);
    int saves_running = (int)tiles_play_state_get_info().saves - saves_before;
    assert(saves_running > 0 && saves_running < 40);
    tiles_play_state_service(t += 100, 0, false, true);           /* clock stops: saved, with an erase */
    assert(!tiles_play_state_get_info().pending && g_erases == 1);
    power_cycle();
    assert(tiles_note_map_get_key_offset() == 39 % 12 && tiles_note_map_get_octave_shift() == 39 % 3);

    /* 6. idle with the clock stopped: the spare sector is erased ahead */
    g_erases = 0;
    for (int i = 0; i < 20; i++) {
        tiles_note_map_set_key_offset((int8_t)(i % 12));
        t += 100; tiles_play_state_service(t, 0, false, true);
        t += 2100; tiles_play_state_service(t, 0, false, true);
        t += 100; tiles_play_state_service(t, 0, false, true);    /* nothing due: prepare may erase */
    }
    assert(g_erases >= 1);
    power_cycle();
    assert(tiles_note_map_get_key_offset() == 19 % 12);

    /* 7. a record from a newer format is ignored, not misread */
    memset(g_mem, 0xFF, sizeof(g_mem));
    tiles_log_t log;
    tiles_log_init(&log, &OPS);
    const uint8_t future[4] = {3, TILES_SCALE_DORIAN, 1, 2};
    assert(tiles_log_append(&log, future, 4, 2, false) == TILES_LOG_OK);
    power_cycle();
    assert(!tiles_play_state_boot_mode(&mode) && tiles_note_map_get_scale() == TILES_SCALE_CHROMATIC);

    /* 8. no storage: nothing restored or saved */
    tiles_note_map_init(false);
    tiles_play_state_init(NULL, false);
    g_programs = 0;
    tiles_note_map_set_scale(TILES_SCALE_DORIAN);
    tiles_play_state_service(t += 5000, 0, false, true);
    assert(g_programs == 0 && !tiles_play_state_get_info().storage);

    printf("play_state: all tests pass\n");
    return 0;
}
