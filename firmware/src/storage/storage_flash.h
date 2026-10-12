#pragma once

/*
 * The real flash operations under storage/kv_store.h: reading through the XIP
 * window, erasing and programming a store's two sectors (flash_map.h) --
 * one set of ops for the settings, one for the content store.
 */

#include "kv_store.h"

#include <stdbool.h>

/* False if the application image has grown into the settings region -- then
 * nothing here may write, or it would corrupt the running program. */
bool tiles_storage_settings_region_safe(void);

/* Flash ops for the settings store. Only use when the check above is true. */
const tiles_kv_ops_t *tiles_storage_settings_ops(void);

/* The same pair for the content store (custom scales), just below settings. */
bool tiles_storage_content_region_safe(void);
const tiles_kv_ops_t *tiles_storage_content_ops(void);

/* And for drum mode's pattern, below the content store. */
bool tiles_storage_drum_region_safe(void);
const tiles_kv_ops_t *tiles_storage_drum_ops(void);
