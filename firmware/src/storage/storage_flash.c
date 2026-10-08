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
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_CONTENT_SLOTS, "flash_map.h and kv_store.h disagree on slot count");

static bool region_safe(uint32_t region_offset) {
    uintptr_t image_end = (uintptr_t)&__flash_binary_end - XIP_BASE;
    return image_end <= region_offset;
}

bool tiles_storage_settings_region_safe(void) {
    return region_safe(TILES_FLASH_SETTINGS_OFFSET);
}

bool tiles_storage_content_region_safe(void) {
    return region_safe(TILES_FLASH_CONTENT_OFFSET);
}

/* The ops take no context, so each region gets thin wrappers (bottom of the
 * file) around these, which take the region's flash offset. */
static bool region_read(uint32_t base, uint8_t slot, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (slot >= TILES_KV_NUM_SLOTS || offset + len > FLASH_SECTOR_SIZE) {
        return false;
    }
    memcpy(buf, (const void *)(XIP_BASE + base + (uint32_t)slot * FLASH_SECTOR_SIZE + offset), len);
    return true;
}

/* Erase/program stall everything: code runs from flash (XIP), so no
 * interrupt (USB, DIN RX) may run during the write. The SDK's
 * flash_range_*() don't disable them, so we do. The watchdog is fed before
 * and after, not during (watchdog_update() lives in flash); same pattern
 * as the pattern bank in services/op_mode.c. A sector erase takes tens of
 * ms, a page program well under 1 ms. settings_persist.c only saves with
 * the pads idle; the content store saves when the app pushes. */
static bool region_erase(uint32_t base, uint8_t slot) {
    if (slot >= TILES_KV_NUM_SLOTS) {
        return false;
    }
    watchdog_update();
    uint32_t prev = save_and_disable_interrupts();
    flash_range_erase(base + (uint32_t)slot * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    restore_interrupts(prev);
    watchdog_update();
    return true;
}

static bool region_program(uint32_t base, uint8_t slot, uint32_t offset, const uint8_t *buf, uint32_t len) {
    if (slot >= TILES_KV_NUM_SLOTS || offset % FLASH_PAGE_SIZE != 0u || len % FLASH_PAGE_SIZE != 0u ||
        offset + len > FLASH_SECTOR_SIZE) {
        return false;
    }
    watchdog_update();
    uint32_t prev = save_and_disable_interrupts();
    flash_range_program(base + (uint32_t)slot * FLASH_SECTOR_SIZE + offset, buf, len); /* buf is RAM (the caller's stack) */
    restore_interrupts(prev);
    watchdog_update();
    return true;
}

#define REGION_OPS(name, base)                                                                         \
    static bool name##_read(uint8_t slot, uint32_t offset, uint8_t *buf, uint32_t len) {               \
        return region_read((base), slot, offset, buf, len);                                            \
    }                                                                                                  \
    static bool name##_erase(uint8_t slot) { return region_erase((base), slot); }                      \
    static bool name##_program(uint8_t slot, uint32_t offset, const uint8_t *buf, uint32_t len) {      \
        return region_program((base), slot, offset, buf, len);                                         \
    }                                                                                                  \
    static const tiles_kv_ops_t name##_ops = {name##_read, name##_erase, name##_program};

REGION_OPS(settings, TILES_FLASH_SETTINGS_OFFSET)
REGION_OPS(content, TILES_FLASH_CONTENT_OFFSET)

const tiles_kv_ops_t *tiles_storage_settings_ops(void) {
    return &settings_ops;
}

const tiles_kv_ops_t *tiles_storage_content_ops(void) {
    return &content_ops;
}
