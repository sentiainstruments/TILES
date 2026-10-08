#pragma once

/* Two-slot, CRC-protected blob store: the hardware-free half of storage/,
 * used under the settings table (profiles/settings.h) and the content store
 * (profiles/content.h), one tiles_kv_t each on its own flash region.
 *
 * Guarantee: one caller-defined payload (up to TILES_KV_MAX_PAYLOAD) lives
 * in one of two flash sectors. A save goes to the OTHER slot and only
 * counts once complete and verified, so a power cut at any instant leaves
 * the previous payload intact. Boot picks the newest valid slot.
 *
 * Slot layout (little-endian, 20-byte header, then the payload):
 *   0  u32 magic            TILES_KV_MAGIC
 *   4  u16 layout_version   this header's layout (TILES_KV_LAYOUT_VERSION)
 *   6  u16 payload_version  the CALLER's schema version
 *   8  u32 seq              increasing; newest valid slot wins
 *  12  u16 payload_len
 *  14  u16 reserved (0)
 *  16  u32 crc32            over bytes 0..15 and the payload
 * A save erases the slot, programs every page except the first, then the
 * first page (with the magic) LAST. A torn write has no magic or a bad
 * CRC, so it can never look valid.
 *
 * Flash access goes through tiles_kv_ops_t, so this runs natively in
 * firmware/test/test_kv_store.c, including a simulated power cut after
 * every erase/program step. storage/storage_flash.c supplies the real ops. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_KV_SECTOR_SIZE 4096u
#define TILES_KV_PAGE_SIZE 256u
#define TILES_KV_NUM_SLOTS 2u
#define TILES_KV_HEADER_SIZE 20u
#define TILES_KV_MAX_PAYLOAD (TILES_KV_SECTOR_SIZE - TILES_KV_HEADER_SIZE)

#define TILES_KV_MAGIC 0x31534b54u /* "TKS1" little-endian */
#define TILES_KV_LAYOUT_VERSION 1u

/* Region-relative flash access. `slot` is 0 or 1; offsets are within that
 * slot's sector. program() is always called with a PAGE-aligned offset and a
 * multiple of TILES_KV_PAGE_SIZE; a program can only clear bits (like real
 * NOR flash), which is why every slot is erased first. All return false on a
 * hardware failure. */
typedef struct {
    bool (*read)(uint8_t slot, uint32_t offset, uint8_t *buf, uint32_t len);
    bool (*erase)(uint8_t slot);
    bool (*program)(uint8_t slot, uint32_t offset, const uint8_t *buf, uint32_t len);
} tiles_kv_ops_t;

typedef enum {
    TILES_KV_OK = 0,
    TILES_KV_ERR_NOT_INIT,
    TILES_KV_ERR_TOO_BIG,
    TILES_KV_ERR_ERASE,   /* the erase op failed; the previous payload is untouched */
    TILES_KV_ERR_PROGRAM, /* a program op failed; the previous payload is untouched */
    TILES_KV_ERR_VERIFY,  /* wrote it but it didn't read back valid; previous payload untouched */
} tiles_kv_result_t;

typedef struct {
    bool valid[TILES_KV_NUM_SLOTS];
    uint32_t seq[TILES_KV_NUM_SLOTS];
    int8_t newest;            /* slot holding the current payload, or -1 if none */
    uint32_t writes;          /* successful saves since boot */
    tiles_kv_result_t last_result;
} tiles_kv_info_t;

/* One store: its region's ops and what the last scan/save found. Owned by
 * the caller (a static in the module using it); fields are private. */
typedef struct {
    const tiles_kv_ops_t *ops;
    tiles_kv_info_t info;
} tiles_kv_t;

/* Scans both slots. Call once at boot; safe with blank (all-0xFF) flash. */
void tiles_kv_init(tiles_kv_t *kv, const tiles_kv_ops_t *ops);

/* Copies the current payload into `out` (capacity `cap`). Returns false if
 * there is no valid payload or it doesn't fit. */
bool tiles_kv_read(tiles_kv_t *kv, uint8_t *out, uint16_t cap, uint16_t *len, uint16_t *payload_version);

/* Saves a new payload to the slot that is NOT the current one, then verifies
 * it. On any failure the current payload is left exactly as it was. */
tiles_kv_result_t tiles_kv_write(tiles_kv_t *kv, const uint8_t *payload, uint16_t len, uint16_t payload_version);

tiles_kv_info_t tiles_kv_get_info(const tiles_kv_t *kv);

/* CRC-32 (IEEE 802.3, the zlib/PNG one). Exposed for the tests. */
uint32_t tiles_kv_crc32(uint32_t crc, const uint8_t *data, uint32_t len);
