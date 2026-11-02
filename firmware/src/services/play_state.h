#pragma once

/* Mode, scale, key and octave kept in flash across power cycles, through
 * log_store: a page append ~2 s after a change, no erase while the clock
 * runs. Tested in test_play_state.c. */

#include "kv_store.h"

#include <stdbool.h>
#include <stdint.h>

/* Call right after tiles_note_map_init(). On a fresh boot it applies the
 * saved scale, key and octave to note_map; on a crash recovery RAM already
 * holds them, and flash is only read to know what's saved. NULL ops = no
 * storage (nothing restored or saved). */
void tiles_play_state_init(const tiles_kv_ops_t *ops, bool crash_recovered);

/* The mode saved before power-off (op_mode.c's mode number); false on a
 * crash recovery or when nothing is saved. */
bool tiles_play_state_boot_mode(uint8_t *mode);

/* Every scan. `mode` is the active mode now. Saves when due. */
void tiles_play_state_service(uint32_t now_ms, uint8_t mode, bool clock_running, bool pads_idle);

typedef struct {
    bool storage;
    bool have_saved;
    bool pending; /* changed since the last save */
    uint8_t mode, scale;
    int8_t octave, key;
    uint32_t saves;
} tiles_play_state_info_t;

tiles_play_state_info_t tiles_play_state_get_info(void);
