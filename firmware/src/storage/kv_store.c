#include "kv_store.h"

#include <string.h>

static const tiles_kv_ops_t *s_ops;
static tiles_kv_info_t s_info;

/* ---- little-endian helpers (the header is byte-packed on purpose: no struct
 * layout, alignment or host-endianness assumptions) ---- */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t tiles_kv_crc32(uint32_t crc, const uint8_t *data, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0u; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

typedef struct {
    bool valid;
    uint32_t seq;
    uint16_t payload_version;
    uint16_t payload_len;
} slot_scan_t;

/* Reads a slot and decides whether it holds a complete, intact payload. The
 * payload CRC is checked in page-sized chunks so no sector-sized buffer is
 * needed. */
static slot_scan_t scan_slot(uint8_t slot) {
    slot_scan_t r = {0};
    uint8_t hdr[TILES_KV_HEADER_SIZE];
    if (!s_ops->read(slot, 0u, hdr, sizeof(hdr))) {
        return r;
    }
    if (get_u32(&hdr[0]) != TILES_KV_MAGIC || get_u16(&hdr[4]) != TILES_KV_LAYOUT_VERSION) {
        return r;
    }
    uint16_t len = get_u16(&hdr[12]);
    if (len > TILES_KV_MAX_PAYLOAD) {
        return r;
    }
    uint32_t crc = tiles_kv_crc32(0u, hdr, 16u);
    uint8_t chunk[TILES_KV_PAGE_SIZE];
    uint32_t done = 0u;
    while (done < len) {
        uint32_t n = len - done;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (!s_ops->read(slot, TILES_KV_HEADER_SIZE + done, chunk, n)) {
            return r;
        }
        crc = tiles_kv_crc32(crc, chunk, n);
        done += n;
    }
    if (crc != get_u32(&hdr[16])) {
        return r;
    }
    r.valid = true;
    r.seq = get_u32(&hdr[8]);
    r.payload_version = get_u16(&hdr[6]);
    r.payload_len = len;
    return r;
}

static void rescan(void) {
    s_info.newest = -1;
    for (uint8_t i = 0u; i < TILES_KV_NUM_SLOTS; i++) {
        slot_scan_t s = scan_slot(i);
        s_info.valid[i] = s.valid;
        s_info.seq[i] = s.valid ? s.seq : 0u;
        if (s.valid && (s_info.newest < 0 || s.seq > s_info.seq[(uint8_t)s_info.newest])) {
            s_info.newest = (int8_t)i;
        }
    }
}

void tiles_kv_init(const tiles_kv_ops_t *ops) {
    s_ops = ops;
    memset(&s_info, 0, sizeof(s_info));
    s_info.newest = -1;
    s_info.last_result = TILES_KV_OK;
    if (s_ops != NULL) {
        rescan();
    }
}

bool tiles_kv_read(uint8_t *out, uint16_t cap, uint16_t *len, uint16_t *payload_version) {
    if (s_ops == NULL || s_info.newest < 0) {
        return false;
    }
    uint8_t slot = (uint8_t)s_info.newest;
    slot_scan_t s = scan_slot(slot); /* re-validated, not trusted from boot */
    if (!s.valid || s.payload_len > cap) {
        return false;
    }
    if (s.payload_len > 0u && !s_ops->read(slot, TILES_KV_HEADER_SIZE, out, s.payload_len)) {
        return false;
    }
    *len = s.payload_len;
    if (payload_version != NULL) {
        *payload_version = s.payload_version;
    }
    return true;
}

/* Builds page `page` of the slot image into buf (TILES_KV_PAGE_SIZE bytes,
 * padded with 0xFF -- the erased value -- past the end of the payload). */
static void build_page(uint8_t page, const uint8_t hdr[TILES_KV_HEADER_SIZE], const uint8_t *payload, uint16_t len,
                       uint8_t *buf) {
    memset(buf, 0xFF, TILES_KV_PAGE_SIZE);
    uint32_t image_start = (uint32_t)page * TILES_KV_PAGE_SIZE;
    for (uint32_t i = 0u; i < TILES_KV_PAGE_SIZE; i++) {
        uint32_t pos = image_start + i;
        if (pos < TILES_KV_HEADER_SIZE) {
            buf[i] = hdr[pos];
        } else if (pos - TILES_KV_HEADER_SIZE < len) {
            buf[i] = payload[pos - TILES_KV_HEADER_SIZE];
        }
    }
}

tiles_kv_result_t tiles_kv_write(const uint8_t *payload, uint16_t len, uint16_t payload_version) {
    if (s_ops == NULL) {
        return TILES_KV_ERR_NOT_INIT;
    }
    if (len > TILES_KV_MAX_PAYLOAD) {
        s_info.last_result = TILES_KV_ERR_TOO_BIG;
        return TILES_KV_ERR_TOO_BIG;
    }

    /* Target: never the slot holding the current payload. */
    uint8_t target = (s_info.newest < 0) ? 0u : (uint8_t)(1 - s_info.newest);
    uint32_t seq = (s_info.newest < 0) ? 1u : s_info.seq[(uint8_t)s_info.newest] + 1u;

    uint8_t hdr[TILES_KV_HEADER_SIZE];
    put_u32(&hdr[0], TILES_KV_MAGIC);
    put_u16(&hdr[4], TILES_KV_LAYOUT_VERSION);
    put_u16(&hdr[6], payload_version);
    put_u32(&hdr[8], seq);
    put_u16(&hdr[12], len);
    put_u16(&hdr[14], 0u);
    uint32_t crc = tiles_kv_crc32(0u, hdr, 16u);
    crc = tiles_kv_crc32(crc, payload, len);
    put_u32(&hdr[16], crc);

    uint8_t pages = (uint8_t)((TILES_KV_HEADER_SIZE + len + TILES_KV_PAGE_SIZE - 1u) / TILES_KV_PAGE_SIZE);
    uint8_t buf[TILES_KV_PAGE_SIZE];

    /* The slot about to be rewritten stops being trusted from this moment: if
     * we lose power now, boot must not consider it (it is erased/half-written
     * anyway) and the OTHER slot remains the newest valid one. */
    s_info.valid[target] = false;

    if (!s_ops->erase(target)) {
        s_info.last_result = TILES_KV_ERR_ERASE;
        return TILES_KV_ERR_ERASE;
    }
    /* Everything except page 0 first, then page 0 (the one with the magic). */
    for (uint8_t p = 1u; p < pages; p++) {
        build_page(p, hdr, payload, len, buf);
        if (!s_ops->program(target, (uint32_t)p * TILES_KV_PAGE_SIZE, buf, TILES_KV_PAGE_SIZE)) {
            s_info.last_result = TILES_KV_ERR_PROGRAM;
            return TILES_KV_ERR_PROGRAM;
        }
    }
    build_page(0u, hdr, payload, len, buf);
    if (!s_ops->program(target, 0u, buf, TILES_KV_PAGE_SIZE)) {
        s_info.last_result = TILES_KV_ERR_PROGRAM;
        return TILES_KV_ERR_PROGRAM;
    }

    slot_scan_t check = scan_slot(target);
    if (!check.valid || check.seq != seq || check.payload_len != len) {
        s_info.last_result = TILES_KV_ERR_VERIFY;
        return TILES_KV_ERR_VERIFY;
    }

    s_info.valid[target] = true;
    s_info.seq[target] = seq;
    s_info.newest = (int8_t)target;
    s_info.writes++;
    s_info.last_result = TILES_KV_OK;
    return TILES_KV_OK;
}

tiles_kv_info_t tiles_kv_get_info(void) {
    return s_info;
}
