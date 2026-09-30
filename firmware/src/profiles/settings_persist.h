#pragma once

/* Settings in flash: load-and-apply at boot, then save changes on its own.
 *
 * Changes are found by DIFFING, not by hooking setters: every 500 ms the
 * sparse snapshot (profiles/settings.h) is compared with what flash holds,
 * so app, script and on-device changes are all caught and no setter can
 * forget to mark itself dirty. A changed snapshot is written only after
 * it has been stable for TILES_SETTINGS_SAVE_DEBOUNCE_MS (dragging a
 * slider doesn't wear the flash) AND the pads are idle (an erase stalls
 * the firmware for tens of ms).
 *
 * Pure logic over tiles_kv_ops_t + the registry, tested natively with a
 * simulated flash (firmware/test/test_settings.c). */

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
