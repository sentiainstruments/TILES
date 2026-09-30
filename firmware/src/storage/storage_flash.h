#pragma once

/*
 * The real flash operations under storage/kv_store.h: reading through the XIP
 * window, erasing and programming the two settings sectors (flash_map.h).
 */

#include "kv_store.h"

#include <stdbool.h>

/* False if the application image has grown into the settings region -- then
 * nothing here may write, or it would corrupt the running program. */
bool tiles_storage_settings_region_safe(void);

/* Flash ops for the settings store. Only use when the check above is true. */
const tiles_kv_ops_t *tiles_storage_settings_ops(void);
