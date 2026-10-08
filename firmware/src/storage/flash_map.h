#pragma once

/* Every flash region the firmware uses, in ONE place so no two features
 * claim the same sector.
 *
 * All at the END of flash, far from the application image (~160 KB);
 * storage_flash.c checks at boot that the image hasn't grown into them and
 * refuses to write if it has. `picotool load` only writes the image's
 * range, so settings and saved patterns survive a firmware update.
 *
 *   top of flash (PICO_FLASH_SIZE_BYTES)
 *   -1 sector   Sequencer pattern bank           services/op_mode.c
 *   -4 sectors  Song mode store                  services/op_mode.c
 *   -2 sectors  Settings (two alternating slots) profiles/settings_persist.c
 *   -2 sectors  Content: custom scales, later    profiles/content.c
 *               layouts (two alternating slots)
 *   ...         free
 *   0           application image */

#include "hardware/flash.h"

#define TILES_FLASH_PATTERN_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define TILES_FLASH_SONG_SECTORS 4u
#define TILES_FLASH_SONG_OFFSET (TILES_FLASH_PATTERN_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SONG_SECTORS)

#define TILES_FLASH_SETTINGS_SLOTS 2u
#define TILES_FLASH_SETTINGS_OFFSET (TILES_FLASH_SONG_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SETTINGS_SLOTS)

#define TILES_FLASH_CONTENT_SLOTS 2u
#define TILES_FLASH_CONTENT_OFFSET (TILES_FLASH_SETTINGS_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_CONTENT_SLOTS)
