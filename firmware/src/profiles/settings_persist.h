#pragma once

/*
 * Saving the settings table to flash: load-and-apply at boot, then watch for
 * changes and save them without anyone asking.
 *
 * "Watch for changes" is done by DIFFING, not by hooking every setter: every
 * 500 ms the current sparse snapshot (profiles/settings.h) is compared to what
 * flash holds. That means a change made by the app, by a USB script, or by a
 * button on the device itself is all noticed the same way, and a setter can
 * never forget to mark itself dirty. A changed snapshot is only written once it
 * has stopped changing for TILES_SETTINGS_SAVE_DEBOUNCE_MS (dragging a slider
 * in the app doesn't wear the flash) AND nothing is being played (a flash
 * erase stops the whole firmware for tens of milliseconds -- see the pattern
 * bank's own comment in services/op_mode.c -- so it waits for hands off the
 * pads rather than landing in the middle of a phrase).
 *
 * Pure logic over tiles_kv_ops_t + the registry, so it is tested natively with
 * a simulated flash (firmware/test/test_settings.c).
 */

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
