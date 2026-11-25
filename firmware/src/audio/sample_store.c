#include "sample_store.h"

#include "kv_store.h"
#include "sampler.h"

#include <string.h>

_Static_assert(TILES_SAMPLE_SLOT_BYTES % TILES_SAMPLE_SECTOR_BYTES == 0u, "slots are whole sectors");
_Static_assert(TILES_SAMPLE_HEADER_BYTES == TILES_SAMPLE_PAGE_BYTES, "the header is one page");

static const tiles_sample_flash_ops_t *s_ops;
static uint32_t s_region_flash_offset;
static bool s_have[TILES_SAMPLE_SLOT_COUNT];
static tiles_sample_info_t s_info[TILES_SAMPLE_SLOT_COUNT];

/* The load under way. */
static struct {
    bool active;
    uint8_t slot;
    tiles_sample_info_t info;
    uint32_t received; /* audio bytes */
    uint32_t next_page; /* offset in the slot of the page being filled */
    uint32_t erased_to; /* offset in the slot below which it is erased */
    uint32_t fill;
    uint8_t page[TILES_SAMPLE_PAGE_BYTES];
} s_load;

static uint32_t slot_base(uint8_t slot) {
    return (uint32_t)slot * TILES_SAMPLE_SLOT_BYTES;
}

/* Hands the slot to the sampler, its first bytes read from flash. */
static void publish(uint8_t slot) {
    if (!s_have[slot]) {
        tiles_sampler_slot_clear(slot);
        return;
    }
    uint8_t head[TILES_SAMPLER_HEAD_BYTES];
    memset(head, 0, sizeof(head));
    uint32_t bytes = tiles_sample_audio_bytes(&s_info[slot]);
    uint32_t n = bytes < sizeof(head) ? bytes : sizeof(head);
    if (!s_ops->read(slot_base(slot) + TILES_SAMPLE_HEADER_BYTES, head, n)) {
        tiles_sampler_slot_clear(slot);
        return;
    }
    tiles_sampler_slot_set(slot, s_info[slot].channels, s_info[slot].frames,
                           s_region_flash_offset + slot_base(slot) + TILES_SAMPLE_HEADER_BYTES, head);
}

void tiles_sample_store_init(const tiles_sample_flash_ops_t *ops, uint32_t region_flash_offset) {
    s_ops = ops;
    s_region_flash_offset = region_flash_offset;
    memset(&s_load, 0, sizeof(s_load));
    memset(s_have, 0, sizeof(s_have));
    for (uint8_t slot = 0; slot < TILES_SAMPLE_SLOT_COUNT; slot++) {
        uint8_t hdr[TILES_SAMPLE_HEADER_BYTES];
        s_have[slot] = ops != NULL && ops->read(slot_base(slot), hdr, sizeof(hdr)) &&
                       tiles_sample_header_decode(hdr, &s_info[slot]);
        if (ops != NULL) {
            publish(slot);
        }
    }
}

tiles_sample_result_t tiles_sample_store_begin(uint8_t slot, const tiles_sample_info_t *info) {
    if (s_ops == NULL) {
        return TILES_SAMPLE_ERR_NO_STORAGE;
    }
    if (s_load.active) {
        return TILES_SAMPLE_ERR_BUSY;
    }
    if (slot >= TILES_SAMPLE_SLOT_COUNT) {
        return TILES_SAMPLE_ERR_SLOT;
    }
    if (!tiles_sample_info_valid(info)) {
        return TILES_SAMPLE_ERR_INFO;
    }
    s_have[slot] = false;
    publish(slot); /* its voices stop before the flash under them goes */
    if (!s_ops->erase_sector(slot_base(slot))) {
        return TILES_SAMPLE_ERR_FLASH;
    }
    memset(&s_load, 0, sizeof(s_load));
    s_load.active = true;
    s_load.slot = slot;
    s_load.info = *info;
    s_load.next_page = TILES_SAMPLE_HEADER_BYTES;
    s_load.erased_to = TILES_SAMPLE_SECTOR_BYTES;
    return TILES_SAMPLE_OK;
}

/* Programs the filled page, erasing its sector first if the data just got
 * there. */
static bool flush_page(void) {
    uint32_t base = slot_base(s_load.slot);
    if (s_load.next_page >= s_load.erased_to) {
        if (!s_ops->erase_sector(base + s_load.erased_to)) {
            return false;
        }
        s_load.erased_to += TILES_SAMPLE_SECTOR_BYTES;
    }
    if (!s_ops->program_page(base + s_load.next_page, s_load.page)) {
        return false;
    }
    s_load.next_page += TILES_SAMPLE_PAGE_BYTES;
    s_load.fill = 0;
    return true;
}

tiles_sample_result_t tiles_sample_store_write(const uint8_t *data, uint32_t len) {
    if (!s_load.active) {
        return TILES_SAMPLE_ERR_IDLE;
    }
    if (len > tiles_sample_audio_bytes(&s_load.info) - s_load.received) {
        tiles_sample_store_abort();
        return TILES_SAMPLE_ERR_SIZE;
    }
    while (len > 0u) {
        uint32_t n = TILES_SAMPLE_PAGE_BYTES - s_load.fill;
        n = n < len ? n : len;
        memcpy(&s_load.page[s_load.fill], data, n);
        s_load.fill += n;
        s_load.received += n;
        data += n;
        len -= n;
        if (s_load.fill == TILES_SAMPLE_PAGE_BYTES && !flush_page()) {
            tiles_sample_store_abort();
            return TILES_SAMPLE_ERR_FLASH;
        }
    }
    return TILES_SAMPLE_OK;
}

tiles_sample_result_t tiles_sample_store_finish(void) {
    if (!s_load.active) {
        return TILES_SAMPLE_ERR_IDLE;
    }
    uint32_t bytes = tiles_sample_audio_bytes(&s_load.info);
    if (s_load.received != bytes) {
        tiles_sample_store_abort();
        return TILES_SAMPLE_ERR_SIZE;
    }
    if (s_load.fill > 0u) {
        memset(&s_load.page[s_load.fill], 0xFF, TILES_SAMPLE_PAGE_BYTES - s_load.fill);
        if (!flush_page()) {
            tiles_sample_store_abort();
            return TILES_SAMPLE_ERR_FLASH;
        }
    }
    /* What flash holds, not what arrived, must match the sender's CRC. */
    uint32_t base = slot_base(s_load.slot) + TILES_SAMPLE_HEADER_BYTES;
    uint32_t crc = 0u;
    for (uint32_t done = 0; done < bytes;) {
        uint8_t buf[TILES_SAMPLE_PAGE_BYTES];
        uint32_t n = bytes - done < sizeof(buf) ? bytes - done : sizeof(buf);
        if (!s_ops->read(base + done, buf, n)) {
            tiles_sample_store_abort();
            return TILES_SAMPLE_ERR_FLASH;
        }
        crc = tiles_kv_crc32(crc, buf, n);
        done += n;
    }
    if (crc != s_load.info.audio_crc) {
        tiles_sample_store_abort();
        return TILES_SAMPLE_ERR_CRC;
    }
    uint8_t hdr[TILES_SAMPLE_HEADER_BYTES];
    if (!tiles_sample_header_encode(&s_load.info, hdr) || !s_ops->program_page(slot_base(s_load.slot), hdr)) {
        tiles_sample_store_abort();
        return TILES_SAMPLE_ERR_FLASH;
    }
    uint8_t slot = s_load.slot;
    s_info[slot] = s_load.info;
    s_have[slot] = true;
    s_load.active = false;
    publish(slot);
    return TILES_SAMPLE_OK;
}

void tiles_sample_store_abort(void) {
    s_load.active = false;
}

uint32_t tiles_sample_store_remaining(void) {
    return s_load.active ? tiles_sample_audio_bytes(&s_load.info) - s_load.received : 0u;
}

bool tiles_sample_store_info(uint8_t slot, tiles_sample_info_t *info) {
    if (slot >= TILES_SAMPLE_SLOT_COUNT || !s_have[slot]) {
        return false;
    }
    *info = s_info[slot];
    return true;
}

tiles_sample_result_t tiles_sample_store_delete(uint8_t slot) {
    if (s_ops == NULL) {
        return TILES_SAMPLE_ERR_NO_STORAGE;
    }
    if (slot >= TILES_SAMPLE_SLOT_COUNT) {
        return TILES_SAMPLE_ERR_SLOT;
    }
    if (s_load.active && s_load.slot == slot) {
        return TILES_SAMPLE_ERR_BUSY;
    }
    if (!s_have[slot]) {
        return TILES_SAMPLE_OK;
    }
    s_have[slot] = false;
    publish(slot);
    uint8_t zeros[TILES_SAMPLE_PAGE_BYTES];
    memset(zeros, 0, sizeof(zeros)); /* programming only clears bits: the magic goes */
    return s_ops->program_page(slot_base(slot), zeros) ? TILES_SAMPLE_OK : TILES_SAMPLE_ERR_FLASH;
}

const char *tiles_sample_result_name(tiles_sample_result_t r) {
    switch (r) {
    case TILES_SAMPLE_OK:
        return "ok";
    case TILES_SAMPLE_ERR_NO_STORAGE:
        return "no-storage";
    case TILES_SAMPLE_ERR_SLOT:
        return "bad-slot";
    case TILES_SAMPLE_ERR_INFO:
        return "bad-format";
    case TILES_SAMPLE_ERR_BUSY:
        return "busy";
    case TILES_SAMPLE_ERR_IDLE:
        return "no-load";
    case TILES_SAMPLE_ERR_SIZE:
        return "size";
    case TILES_SAMPLE_ERR_FLASH:
        return "flash";
    case TILES_SAMPLE_ERR_CRC:
        return "crc";
    }
    return "unknown";
}
