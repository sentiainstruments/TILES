#include "storage_flash.h"

#include "flash_map.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include <stdint.h>
#include <string.h>

/* End of the application image in flash, from the linker script. */
extern char __flash_binary_end;

_Static_assert(TILES_KV_SECTOR_SIZE == FLASH_SECTOR_SIZE, "kv_store sector size must be the chip's erase size");
_Static_assert(TILES_KV_PAGE_SIZE == FLASH_PAGE_SIZE, "kv_store page size must be the chip's program size");
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_SETTINGS_SLOTS, "flash_map.h and kv_store.h disagree on slot count");

bool tiles_storage_settings_region_safe(void) {
    uintptr_t image_end = (uintptr_t)&__flash_binary_end - XIP_BASE;
    return image_end <= TILES_FLASH_SETTINGS_OFFSET;
}

static uint32_t slot_offset(uint8_t slot) {
    return TILES_FLASH_SETTINGS_OFFSET + (uint32_t)slot * FLASH_SECTOR_SIZE;
}

static bool ops_read(uint8_t slot, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (slot >= TILES_KV_NUM_SLOTS || offset + len > FLASH_SECTOR_SIZE) {
        return false;
    }
    memcpy(buf, (const void *)(XIP_BASE + slot_offset(slot) + offset), len);
    return true;
}

/* Erase and program stall everything: the chip is memory-mapped and executed
 * from (XIP), so nothing can be fetched from flash while it is being written,
 * and every interrupt (USB, DIN RX) must stay off for the whole span. The SDK's
 * flash_range_*() do NOT do that for us -- same single-core pattern, and the
 * same watchdog pets before and after (not during: watchdog_update() is
 * flash-resident code), as services/op_mode.c's pattern bank documents at
 * length. A sector erase is tens of milliseconds; a page program well under a
 * millisecond. settings_persist.c only ever gets here with the pads idle. */
static bool ops_erase(uint8_t slot) {
    if (slot >= TILES_KV_NUM_SLOTS) {
        return false;
    }
    watchdog_update();
    uint32_t prev = save_and_disable_interrupts();
    flash_range_erase(slot_offset(slot), FLASH_SECTOR_SIZE);
    restore_interrupts(prev);
    watchdog_update();
    return true;
}

static bool ops_program(uint8_t slot, uint32_t offset, const uint8_t *buf, uint32_t len) {
    if (slot >= TILES_KV_NUM_SLOTS || offset % FLASH_PAGE_SIZE != 0u || len % FLASH_PAGE_SIZE != 0u ||
        offset + len > FLASH_SECTOR_SIZE) {
        return false;
    }
    watchdog_update();
    uint32_t prev = save_and_disable_interrupts();
    flash_range_program(slot_offset(slot) + offset, buf, len); /* buf is RAM (the caller's stack) */
    restore_interrupts(prev);
    watchdog_update();
    return true;
}

static const tiles_kv_ops_t s_ops = {ops_read, ops_erase, ops_program};

const tiles_kv_ops_t *tiles_storage_settings_ops(void) {
    return &s_ops;
}
