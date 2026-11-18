#pragma once

/* Settings in flash: loaded at boot, saved by diffing the sparse snapshot
 * every 500 ms (so no setter can forget), once stable for
 * TILES_SETTINGS_SAVE_DEBOUNCE_MS and with the pads idle. Tested natively. */

#include "kv_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILES_SETTINGS_POLL_MS 500u
#define TILES_SETTINGS_SAVE_DEBOUNCE_MS 2000u
#define TILES_SETTINGS_SAVE_RETRY_MS 30000u

/* Returns true when it is a good moment to stop the world for a flash write. */
typedef bool (*tiles_settings_idle_fn)(void);

/* Loads the saved snapshot (if any) and applies it. Call after the registry has
 * been initialized and defaults captured. `idle` may be NULL (always idle). */
void tiles_settings_persist_init(const tiles_kv_ops_t *ops, tiles_settings_idle_fn idle);

/* Before anything else boots (the USB descriptor depends on device.mode):
 * one setting's saved value straight from flash. False if it isn't saved
 * (it is at its default) or nothing is. Call before persist_init. */
bool tiles_settings_persist_peek(const tiles_kv_ops_t *ops, uint16_t id, uint32_t *value);

/* Call every main-loop iteration; does real work at most every
 * TILES_SETTINGS_POLL_MS. */
void tiles_settings_persist_service(uint32_t now_ms);

/* Writes the current snapshot immediately (the SAVE command). Returns
 * TILES_KV_OK when it is saved (including "nothing to save"). */
tiles_kv_result_t tiles_settings_persist_save_now(void);

typedef struct {
    tiles_kv_info_t kv;
    bool loaded;            /* a saved snapshot was found and applied at boot */
    size_t applied;         /* settings that snapshot restored */
    bool pending;           /* a change is waiting to be saved */
    uint32_t save_failures; /* failed save attempts since boot */
    uint16_t saved_bytes;   /* size of the snapshot currently in flash */
} tiles_settings_persist_info_t;

tiles_settings_persist_info_t tiles_settings_persist_get_info(void);
