#include "storage_flash.h"

#include "flash_map.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"

#include <stdint.h>
#include <string.h>

/* End of the application image in flash, from the linker script. */
extern char __flash_binary_end;

_Static_assert(TILES_KV_SECTOR_SIZE == FLASH_SECTOR_SIZE, "kv_store sector size must be the chip's erase size");
_Static_assert(TILES_KV_PAGE_SIZE == FLASH_PAGE_SIZE, "kv_store page size must be the chip's program size");
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_SETTINGS_SLOTS, "flash_map.h and kv_store.h disagree on slot count");
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_CONTENT_SLOTS, "flash_map.h and kv_store.h disagree on slot count");
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_DRUM_SLOTS, "flash_map.h and kv_store.h disagree on slot count");
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_STATE_SLOTS, "flash_map.h and kv_store.h disagree on slot count");
_Static_assert(TILES_KV_NUM_SLOTS == TILES_FLASH_SEQ_SLOTS, "flash_map.h and kv_store.h disagree on slot count");

#define FLASH_LOCKOUT_TIMEOUT_US 10000u

static bool s_core1_locked;
static uint32_t s_lockout_fails;

bool tiles_flash_write_begin(uint32_t *irq_state) {
    s_core1_locked = multicore_lockout_victim_is_initialized(1);
    if (s_core1_locked && !multicore_lockout_start_timeout_us(FLASH_LOCKOUT_TIMEOUT_US)) {
        s_core1_locked = false;
        s_lockout_fails++;
        return false;
    }
    *irq_state = save_and_disable_interrupts();
    return true;
}

void tiles_flash_write_end(uint32_t irq_state) {
    restore_interrupts(irq_state);
    if (s_core1_locked) {
        s_core1_locked = false;
        (void)multicore_lockout_end_timeout_us(FLASH_LOCKOUT_TIMEOUT_US);
    }
}

uint32_t tiles_flash_lockout_fails(void) {
    return s_lockout_fails;
}

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

bool tiles_storage_drum_region_safe(void) {
    return region_safe(TILES_FLASH_DRUM_OFFSET);
}

bool tiles_storage_state_region_safe(void) {
    return region_safe(TILES_FLASH_STATE_OFFSET);
}

bool tiles_storage_seq_region_safe(void) {
    return region_safe(TILES_FLASH_SEQ_OFFSET);
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

/* Code runs from flash, so interrupts are off and the audio core paused
 * during erase and program (the SDK doesn't do it); the watchdog is fed
 * before and after. An erase takes tens of ms: callers save with the pads
 * idle or the clock stopped. */
static bool region_erase(uint32_t base, uint8_t slot) {
    if (slot >= TILES_KV_NUM_SLOTS) {
        return false;
    }
    watchdog_update();
    uint32_t prev;
    if (!tiles_flash_write_begin(&prev)) {
        return false;
    }
    flash_range_erase(base + (uint32_t)slot * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
    tiles_flash_write_end(prev);
    watchdog_update();
    return true;
}

static bool region_program(uint32_t base, uint8_t slot, uint32_t offset, const uint8_t *buf, uint32_t len) {
    if (slot >= TILES_KV_NUM_SLOTS || offset % FLASH_PAGE_SIZE != 0u || len % FLASH_PAGE_SIZE != 0u ||
        offset + len > FLASH_SECTOR_SIZE) {
        return false;
    }
    watchdog_update();
    uint32_t prev;
    if (!tiles_flash_write_begin(&prev)) {
        return false;
    }
    flash_range_program(base + (uint32_t)slot * FLASH_SECTOR_SIZE + offset, buf, len); /* buf is RAM (the caller's stack) */
    tiles_flash_write_end(prev);
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
REGION_OPS(drum, TILES_FLASH_DRUM_OFFSET)
REGION_OPS(state, TILES_FLASH_STATE_OFFSET)
REGION_OPS(seq, TILES_FLASH_SEQ_OFFSET)

const tiles_kv_ops_t *tiles_storage_settings_ops(void) {
    return &settings_ops;
}

const tiles_kv_ops_t *tiles_storage_content_ops(void) {
    return &content_ops;
}

const tiles_kv_ops_t *tiles_storage_drum_ops(void) {
    return &drum_ops;
}

const tiles_kv_ops_t *tiles_storage_state_ops(void) {
    return &state_ops;
}

const tiles_kv_ops_t *tiles_storage_seq_ops(void) {
    return &seq_ops;
}
