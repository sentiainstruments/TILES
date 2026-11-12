#pragma once

/* Append-only log over two sectors: saves are page programs only (under a
 * millisecond each; a record can go out a page at a time), so they're
 * never heard. Erasing the other sector is separate (tiles_log_prepare()),
 * done when a stall doesn't matter.
 * Record, page-aligned, little-endian: 0 u32 magic, 4 u32 seq, 8 u16
 * payload_len, 10 u16 payload_version, 12 u32 crc32, then the payload;
 * the first page is programmed last. Tested natively with power cuts. */

#include "kv_store.h"

#include <stdbool.h>
#include <stdint.h>

#define TILES_LOG_MAGIC 0x31474c54u /* "TLG1" */
#define TILES_LOG_HEADER_SIZE 16u
#define TILES_LOG_MAX_PAYLOAD (TILES_KV_SECTOR_SIZE - TILES_LOG_HEADER_SIZE)
#define TILES_LOG_PAGES (TILES_KV_SECTOR_SIZE / TILES_KV_PAGE_SIZE)

typedef enum {
    TILES_LOG_OK = 0,
    TILES_LOG_ERR_NOT_INIT,
    TILES_LOG_ERR_TOO_BIG,
    TILES_LOG_ERR_NEEDS_ERASE, /* current sector full, other not blank: try again with erase allowed */
    TILES_LOG_ERR_ERASE,
    TILES_LOG_ERR_PROGRAM,
    TILES_LOG_ERR_VERIFY,
    TILES_LOG_PENDING,  /* append_start/step: pages left, step again */
    TILES_LOG_ERR_BUSY, /* a record is still going out */
} tiles_log_result_t;

typedef struct {
    const tiles_kv_ops_t *ops;
    bool have_newest;
    uint8_t newest_slot;
    uint8_t newest_page;
    uint32_t newest_seq;
    uint16_t newest_len;
    uint16_t newest_version;
    uint8_t next_free_page[TILES_KV_NUM_SLOTS]; /* first page of the erased tail; TILES_LOG_PAGES = full */
    bool blank[TILES_KV_NUM_SLOTS];            /* the whole sector reads erased */
    uint32_t writes;
    uint32_t erases;
    /* The record going out a page at a time. */
    bool pending;
    const uint8_t *pend_payload;
    uint8_t pend_hdr[TILES_LOG_HEADER_SIZE];
    uint32_t pend_seq;
    uint16_t pend_len;
    uint16_t pend_version;
    uint8_t pend_slot;
    uint8_t pend_page;
    uint8_t pend_left;
} tiles_log_t;

/* Scans both sectors. Safe with blank or foreign data. */
void tiles_log_init(tiles_log_t *log, const tiles_kv_ops_t *ops);

/* Copies the newest record's payload; false if none or it doesn't fit. */
bool tiles_log_read(tiles_log_t *log, uint8_t *out, uint16_t cap, uint16_t *len, uint16_t *payload_version);

/* Appends a record. With allow_erase false it never erases (returns
 * TILES_LOG_ERR_NEEDS_ERASE if it would have to). */
tiles_log_result_t tiles_log_append(tiles_log_t *log, const uint8_t *payload, uint16_t len, uint16_t payload_version,
                                    bool allow_erase);

/* The same append one page per call, for saves while sound plays: start
 * programs the first page (after any erase it is allowed), then step once
 * per main-loop pass while it returns TILES_LOG_PENDING. The payload must
 * stay unchanged until it returns something else. A power cut between
 * steps leaves the previous record, as with append. */
tiles_log_result_t tiles_log_append_start(tiles_log_t *log, const uint8_t *payload, uint16_t len,
                                          uint16_t payload_version, bool allow_erase);
tiles_log_result_t tiles_log_append_step(tiles_log_t *log);

/* True if a record of `len` bytes would be appended without an erase. */
bool tiles_log_fits_without_erase(const tiles_log_t *log, uint16_t len);

/* Erases the sector not holding the newest record, if it isn't blank, so
 * the next sector switch costs no erase. Call when a stall is harmless.
 * True if it erased something. */
bool tiles_log_prepare(tiles_log_t *log);
