#pragma once

/* One 4 KB buffer for a save built and written within a single call:
 * op_mode's pattern and song sectors, content.c's next blob. All of them
 * run on core 0's loop one at a time, and none keeps it across calls, so
 * they share it instead of holding 4 KB each. */

#include <stdint.h>

#define TILES_FLASH_STAGING_BYTES 4096u

uint8_t *tiles_flash_staging(void);
