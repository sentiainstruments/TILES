#pragma once

/* The 8 drum sample slots in flash: loading one (a stream of bytes, as the
 * settings link receives them), deleting one, and finding them at boot.
 * Each change is handed to the sampler (sampler.h). Plain C over the flash
 * ops below, so test/test_sampler.c runs it against a simulated flash
 * that loses power mid-write.
 *
 * Loading: begin erases the slot's first sector (the slot reads empty from
 * then on), the audio is programmed a page at a time, each further sector
 * erased as the data reaches it, then finish reads it all back, checks
 * the CRC and programs the header last. A load cut off anywhere leaves an
 * empty slot, never a broken one. */

#include <stdbool.h>
#include <stdint.h>

#include "sample_slot.h"

#define TILES_SAMPLE_SECTOR_BYTES 4096u
#define TILES_SAMPLE_PAGE_BYTES 256u

/* Offsets count from the start of the slots' region. */
typedef struct {
    bool (*read)(uint32_t offset, uint8_t *buf, uint32_t len);
    bool (*erase_sector)(uint32_t offset);
    bool (*program_page)(uint32_t offset, const uint8_t *buf); /* TILES_SAMPLE_PAGE_BYTES */
} tiles_sample_flash_ops_t;

typedef enum {
    TILES_SAMPLE_OK = 0,
    TILES_SAMPLE_ERR_NO_STORAGE,
    TILES_SAMPLE_ERR_SLOT,
    TILES_SAMPLE_ERR_INFO,  /* channels, length or name not storable */
    TILES_SAMPLE_ERR_BUSY,  /* a load is under way */
    TILES_SAMPLE_ERR_IDLE,  /* write/finish without begin */
    TILES_SAMPLE_ERR_SIZE,  /* more or fewer bytes than announced */
    TILES_SAMPLE_ERR_FLASH, /* an erase or program failed */
    TILES_SAMPLE_ERR_CRC,   /* what flash holds isn't what was sent */
} tiles_sample_result_t;

/* Finds the slots and hands each to the sampler. ops NULL: no storage.
 * region_flash_offset: where the region starts in flash (for streaming). */
void tiles_sample_store_init(const tiles_sample_flash_ops_t *ops, uint32_t region_flash_offset);

/* Slot 0-7. info->audio_crc is the CRC-32 of the audio bytes to come. */
tiles_sample_result_t tiles_sample_store_begin(uint8_t slot, const tiles_sample_info_t *info);
tiles_sample_result_t tiles_sample_store_write(const uint8_t *data, uint32_t len);
tiles_sample_result_t tiles_sample_store_finish(void);
/* Drops a load (the slot stays empty). */
void tiles_sample_store_abort(void);
/* Audio bytes the load under way still expects (0: none under way). */
uint32_t tiles_sample_store_remaining(void);

/* What a slot holds; false if empty. */
bool tiles_sample_store_info(uint8_t slot, tiles_sample_info_t *info);

/* Empties a slot by clearing its header (one page program, no erase). */
tiles_sample_result_t tiles_sample_store_delete(uint8_t slot);

const char *tiles_sample_result_name(tiles_sample_result_t r);
