#pragma once

/* Every flash region, in one place, at the end of flash (the image is
 * ~160 KB; storage_flash.c refuses to write if it grows into them).
 * picotool only writes the image, so saved data survives updates.
 *   top -1 sector   sequencer pattern bank      op_mode.c
 *       -4          Song store                  op_mode.c
 *       -2          settings                    settings_persist.c
 *       -2          content (custom scales)     content.c
 *       -2          drum pattern                drum_seq.c
 *       -2          mode, scale, key, octave    play_state.c
 *       -2          sequencer autosave          op_mode.c
 *   0               application image */

#include "hardware/flash.h"

#define TILES_FLASH_PATTERN_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define TILES_FLASH_SONG_SECTORS 4u
#define TILES_FLASH_SONG_OFFSET (TILES_FLASH_PATTERN_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SONG_SECTORS)

#define TILES_FLASH_SETTINGS_SLOTS 2u
#define TILES_FLASH_SETTINGS_OFFSET (TILES_FLASH_SONG_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SETTINGS_SLOTS)

#define TILES_FLASH_CONTENT_SLOTS 2u
#define TILES_FLASH_CONTENT_OFFSET (TILES_FLASH_SETTINGS_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_CONTENT_SLOTS)

#define TILES_FLASH_DRUM_SLOTS 2u
#define TILES_FLASH_DRUM_OFFSET (TILES_FLASH_CONTENT_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_DRUM_SLOTS)

#define TILES_FLASH_STATE_SLOTS 2u
#define TILES_FLASH_STATE_OFFSET (TILES_FLASH_DRUM_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_STATE_SLOTS)

#define TILES_FLASH_SEQ_SLOTS 2u
#define TILES_FLASH_SEQ_OFFSET (TILES_FLASH_STATE_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_SEQ_SLOTS)
