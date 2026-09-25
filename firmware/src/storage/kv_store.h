#pragma once

/*
 * Two-slot, CRC-protected blob store -- the hardware-free half of storage/.
 *
 * Real feedback: "yes start with the settings table and flash saving." Every
 * setting used to reset on reboot (shared/protocol/README.md's own
 * "Persistence: None"), and the ones worth tuning by ear -- LED levels, DIN
 * polarity, the melodic-harmonics switch -- were compile-time constants, so
 * each tweak meant edit / build / commit / flash. This is the persistence
 * layer under the settings table (profiles/settings.h).
 *
 * What it guarantees: one caller-defined payload (a few hundred bytes today,
 * up to TILES_KV_MAX_PAYLOAD) is kept in one of two flash sectors; a save goes
 * to the OTHER one and only counts once it is complete and verified, so a
 * power cut or crash at any instant during a save leaves the previous payload
 * intact. Boot picks the newest valid slot. That is the "two alternating slots
 * so a failed/interrupted write never bricks the active profile" that
 * storage/README.md always promised.
 *
 * How a slot is laid out (little-endian, header 20 bytes then the payload):
 *   0  u32 magic            TILES_KV_MAGIC
 *   4  u16 layout_version   this header's own layout (TILES_KV_LAYOUT_VERSION)
 *   6  u16 payload_version  the CALLER's schema version for the payload
 *   8  u32 seq              monotonically increasing; newest valid slot wins
 *  12  u16 payload_len
 *  14  u16 reserved (0)
 *  16  u32 crc32            over bytes 0..15 and the payload
 * A save erases the slot, programs every page EXCEPT the first, then the first
 * page (the one holding the magic) LAST. A torn write therefore has either no
 * magic at all or a CRC that doesn't match -- it can never look valid.
 *
 * Pure logic on purpose: flash access goes through tiles_kv_ops_t, so this
 * file is compiled and exercised natively off-target, including a simulated
 * power cut after every single erase/program step (firmware/test/
 * test_kv_store.c). storage/storage_flash.c supplies the real ops.
 */

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

/* Scans both slots. Call once at boot; safe with blank (all-0xFF) flash. */
void tiles_kv_init(const tiles_kv_ops_t *ops);

/* Copies the current payload into `out` (capacity `cap`). Returns false if
 * there is no valid payload or it doesn't fit. */
bool tiles_kv_read(uint8_t *out, uint16_t cap, uint16_t *len, uint16_t *payload_version);

/* Saves a new payload to the slot that is NOT the current one, then verifies
 * it. On any failure the current payload is left exactly as it was. */
tiles_kv_result_t tiles_kv_write(const uint8_t *payload, uint16_t len, uint16_t payload_version);

tiles_kv_info_t tiles_kv_get_info(void);

/* CRC-32 (IEEE 802.3, the zlib/PNG one). Exposed for the tests. */
uint32_t tiles_kv_crc32(uint32_t crc, const uint8_t *data, uint32_t len);
