#pragma once

/* Every flash region, in one place, at the end of flash (the image is
 * ~340 KB; storage_flash.c refuses to write if it grows into them).
 * picotool only writes the image, so saved data survives updates.
 *   top -1 sector   sequencer pattern bank      op_mode.c
 *       -4          Song store                  op_mode.c
 *       -2          settings                    settings_persist.c
 *       -2          content (custom scales)     content.c
 *       -2          drum pattern                drum_seq.c
 *       -2          mode, scale, key, octave    play_state.c
 *       -2          sequencer autosave          op_mode.c
 *       -2          the last caught crash       diagnostics/fault.c
 *       -2          the DX7 bank (32 voices)    audio/fm_bank.c
 *   (the saved data keeps a 256 KB band at the top for what comes next)
 *   below the band  8 drum sample slots, 1.5 MB  audio/sample_store.c
 *   0               application image (and its wavetables), < 2.25 MB */

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

#define TILES_FLASH_FAULT_SLOTS 2u
#define TILES_FLASH_FAULT_OFFSET (TILES_FLASH_SEQ_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_FAULT_SLOTS)

#define TILES_FLASH_FM_BANK_SLOTS 2u
#define TILES_FLASH_FM_BANK_OFFSET (TILES_FLASH_FAULT_OFFSET - FLASH_SECTOR_SIZE * TILES_FLASH_FM_BANK_SLOTS)

/* The drum sampler's slots: 192 KB each (three 64 KB blocks), below the
 * saved data's 256 KB band. */
#define TILES_FLASH_SAVED_BAND (256u * 1024u)
#define TILES_FLASH_SAMPLE_SLOTS 8u
#define TILES_FLASH_SAMPLE_SLOT_BYTES (192u * 1024u)
#define TILES_FLASH_SAMPLE_OFFSET                                                                                  \
    (PICO_FLASH_SIZE_BYTES - TILES_FLASH_SAVED_BAND - TILES_FLASH_SAMPLE_SLOTS * TILES_FLASH_SAMPLE_SLOT_BYTES)

_Static_assert(TILES_FLASH_FM_BANK_OFFSET >= PICO_FLASH_SIZE_BYTES - TILES_FLASH_SAVED_BAND,
               "the saved data outgrew its band: move the sample slots down");
_Static_assert(TILES_FLASH_SAMPLE_OFFSET % (64u * 1024u) == 0u, "sample slots start on a 64 KB block");
