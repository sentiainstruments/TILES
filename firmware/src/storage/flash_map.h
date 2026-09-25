#pragma once

/*
 * Where everything the firmware keeps in flash lives -- ONE place, so two
 * features can't quietly claim the same sector.
 *
 * All of it sits at the very END of the chip's flash, as far as possible from
 * the application image at the start (which is ~160 KB today), so the image can
 * grow a long way before this matters -- storage_flash.c checks at boot that it
 * still hasn't, and refuses to touch flash if it has. Reflashing with
 * `picotool load` only writes the image's own range, so none of this is erased
 * by a firmware update (settings and saved patterns survive a reflash).
 *
 *   top of flash (PICO_FLASH_SIZE_BYTES)
 *   -1 sector   Sequencer pattern bank      services/op_mode.c
 *   -4 sectors  Song mode store             services/op_mode.c
 *   -2 sectors  Settings (two alternating slots)   profiles/settings_persist.c
 *   ...         free
 *   0           application image
 *
 * The pattern/song offsets are exactly what op_mode.c always used (it now takes
 * them from here); the settings region is new, placed directly below them.
 */

#include "hardware/flash.h"

#define TILES_FLASH_PATTERN_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define TILES_FLASH_SONG_SECTORS 4u
#define TILES_FLASH_SONG_OFFSET (TILES_FLASH_PATTERN_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SONG_SECTORS)

#define TILES_FLASH_SETTINGS_SLOTS 2u
#define TILES_FLASH_SETTINGS_OFFSET (TILES_FLASH_SONG_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SETTINGS_SLOTS)
