#include "log_store.h"

#include <string.h>

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

static uint8_t pages_for(uint16_t len) {
    return (uint8_t)((TILES_LOG_HEADER_SIZE + (uint32_t)len + TILES_KV_PAGE_SIZE - 1u) / TILES_KV_PAGE_SIZE);
}

/* Checks the record starting at `page`; on success fills seq/len/version. */
static bool record_valid(const tiles_log_t *log, uint8_t slot, uint8_t page, uint32_t *seq, uint16_t *len,
                         uint16_t *version) {
    uint8_t hdr[TILES_LOG_HEADER_SIZE];
    uint32_t base = (uint32_t)page * TILES_KV_PAGE_SIZE;
    if (!log->ops->read(slot, base, hdr, sizeof(hdr)) || get_u32(&hdr[0]) != TILES_LOG_MAGIC) {
        return false;
    }
    uint16_t n = get_u16(&hdr[8]);
    if (n > TILES_LOG_MAX_PAYLOAD || (uint32_t)page + pages_for(n) > TILES_LOG_PAGES) {
        return false;
    }
    uint32_t crc = tiles_kv_crc32(0u, hdr, 12u);
    uint8_t chunk[64];
    for (uint32_t done = 0u; done < n;) {
        uint32_t k = n - done < sizeof(chunk) ? n - done : sizeof(chunk);
        if (!log->ops->read(slot, base + TILES_LOG_HEADER_SIZE + done, chunk, k)) {
            return false;
        }
        crc = tiles_kv_crc32(crc, chunk, k);
        done += k;
    }
    if (crc != get_u32(&hdr[12])) {
        return false;
    }
    *seq = get_u32(&hdr[4]);
    *len = n;
    *version = get_u16(&hdr[10]);
    return true;
}

static bool page_erased(const tiles_log_t *log, uint8_t slot, uint8_t page) {
    uint8_t buf[64];
    for (uint32_t off = 0u; off < TILES_KV_PAGE_SIZE; off += sizeof(buf)) {
        if (!log->ops->read(slot, (uint32_t)page * TILES_KV_PAGE_SIZE + off, buf, sizeof(buf))) {
            return false;
        }
        for (uint32_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != 0xFFu) {
                return false;
            }
        }
    }
    return true;
}

static void scan_slot(tiles_log_t *log, uint8_t slot) {
    /* The erased tail starts after the last page with anything on it (a torn
     * record leaves programmed pages that can't be programmed again). */
    int last_dirty = -1;
    for (uint8_t p = 0; p < TILES_LOG_PAGES; p++) {
        if (!page_erased(log, slot, p)) {
            last_dirty = p;
        }
    }
    log->next_free_page[slot] = (uint8_t)(last_dirty + 1);
    log->blank[slot] = last_dirty < 0;
    for (uint8_t p = 0; p < log->next_free_page[slot];) {
        uint32_t seq;
        uint16_t len, version;
        if (record_valid(log, slot, p, &seq, &len, &version)) {
            if (!log->have_newest || seq > log->newest_seq) {
                log->have_newest = true;
                log->newest_slot = slot;
                log->newest_page = p;
                log->newest_seq = seq;
                log->newest_len = len;
                log->newest_version = version;
            }
            p = (uint8_t)(p + pages_for(len));
        } else {
            p++;
        }
    }
}

void tiles_log_init(tiles_log_t *log, const tiles_kv_ops_t *ops) {
    memset(log, 0, sizeof(*log));
    log->ops = ops;
    if (ops == NULL) {
        return;
    }
    for (uint8_t slot = 0; slot < TILES_KV_NUM_SLOTS; slot++) {
        scan_slot(log, slot);
    }
}

bool tiles_log_read(tiles_log_t *log, uint8_t *out, uint16_t cap, uint16_t *len, uint16_t *payload_version) {
    if (log->ops == NULL || !log->have_newest || log->newest_len > cap) {
        return false;
    }
    uint32_t seq;
    uint16_t n, version;
    if (!record_valid(log, log->newest_slot, log->newest_page, &seq, &n, &version)) {
        return false; /* re-validated, not trusted from boot */
    }
    uint32_t base = (uint32_t)log->newest_page * TILES_KV_PAGE_SIZE + TILES_LOG_HEADER_SIZE;
    if (n > 0u && !log->ops->read(log->newest_slot, base, out, n)) {
        return false;
    }
    *len = n;
    if (payload_version != NULL) {
        *payload_version = version;
    }
    return true;
}

/* The sector the next record goes to and its first page, or false if that
 * needs an erase first (*needs_erase_slot says which). */
static bool find_room(const tiles_log_t *log, uint8_t pages, uint8_t *slot, uint8_t *page, uint8_t *needs_erase_slot) {
    uint8_t active = log->have_newest ? log->newest_slot : 0u;
    if (log->next_free_page[active] + pages <= TILES_LOG_PAGES) {
        *slot = active;
        *page = log->next_free_page[active];
        return true;
    }
    uint8_t other = (uint8_t)(1u - active);
    if (log->blank[other]) {
        *slot = other;
        *page = 0u;
        return true;
    }
    *needs_erase_slot = other;
    return false;
}

bool tiles_log_fits_without_erase(const tiles_log_t *log, uint16_t len) {
    uint8_t slot, page, erase_slot;
    return log->ops != NULL && len <= TILES_LOG_MAX_PAYLOAD && find_room(log, pages_for(len), &slot, &page, &erase_slot);
}

static bool erase_slot(tiles_log_t *log, uint8_t slot) {
    if (!log->ops->erase(slot)) {
        return false;
    }
    log->blank[slot] = true;
    log->next_free_page[slot] = 0u;
    log->erases++;
    return true;
}

bool tiles_log_prepare(tiles_log_t *log) {
    if (log->ops == NULL || !log->have_newest) {
        return false;
    }
    uint8_t other = (uint8_t)(1u - log->newest_slot);
    return !log->blank[other] && erase_slot(log, other);
}

tiles_log_result_t tiles_log_append(tiles_log_t *log, const uint8_t *payload, uint16_t len, uint16_t payload_version,
                                    bool allow_erase) {
    if (log->ops == NULL) {
        return TILES_LOG_ERR_NOT_INIT;
    }
    if (len > TILES_LOG_MAX_PAYLOAD) {
        return TILES_LOG_ERR_TOO_BIG;
    }
    uint8_t pages = pages_for(len), slot, page, needs_erase = 0u;
    if (!find_room(log, pages, &slot, &page, &needs_erase)) {
        if (!allow_erase) {
            return TILES_LOG_ERR_NEEDS_ERASE;
        }
        /* Never the sector holding the newest record: find_room() only asks to
         * erase the other one. */
        if (!erase_slot(log, needs_erase)) {
            return TILES_LOG_ERR_ERASE;
        }
        slot = needs_erase;
        page = 0u;
    }

    uint32_t seq = log->have_newest ? log->newest_seq + 1u : 1u;
    uint8_t hdr[TILES_LOG_HEADER_SIZE];
    put_u32(&hdr[0], TILES_LOG_MAGIC);
    put_u32(&hdr[4], seq);
    put_u16(&hdr[8], len);
    put_u16(&hdr[10], payload_version);
    uint32_t crc = tiles_kv_crc32(0u, hdr, 12u);
    crc = tiles_kv_crc32(crc, payload, len);
    put_u32(&hdr[12], crc);

    /* From here the pages count as used, whatever happens. */
    log->next_free_page[slot] = (uint8_t)(page + pages);
    log->blank[slot] = false;

    uint8_t buf[TILES_KV_PAGE_SIZE];
    for (int k = (int)pages - 1; k >= 0; k--) { /* page 0 (the magic) last */
        memset(buf, 0xFF, sizeof(buf));
        for (uint32_t i = 0; i < TILES_KV_PAGE_SIZE; i++) {
            uint32_t pos = (uint32_t)k * TILES_KV_PAGE_SIZE + i;
            if (pos < TILES_LOG_HEADER_SIZE) {
                buf[i] = hdr[pos];
            } else if (pos - TILES_LOG_HEADER_SIZE < len) {
                buf[i] = payload[pos - TILES_LOG_HEADER_SIZE];
            }
        }
        if (!log->ops->program(slot, (uint32_t)(page + k) * TILES_KV_PAGE_SIZE, buf, TILES_KV_PAGE_SIZE)) {
            return TILES_LOG_ERR_PROGRAM;
        }
    }

    uint32_t got_seq;
    uint16_t got_len, got_version;
    if (!record_valid(log, slot, page, &got_seq, &got_len, &got_version) || got_seq != seq || got_len != len) {
        return TILES_LOG_ERR_VERIFY;
    }
    log->have_newest = true;
    log->newest_slot = slot;
    log->newest_page = page;
    log->newest_seq = seq;
    log->newest_len = len;
    log->newest_version = payload_version;
    log->writes++;
    return TILES_LOG_OK;
}
