#include "seq_store.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

#include <stddef.h>
#include <string.h>

/* "SEQ0" as a plain 32-bit constant, not literal ASCII bytes -- just
 * needs to be distinctive against erased flash (which reads as all
 * 0xFF) and unlikely to collide with anything else that might ever
 * land at this offset; the exact value carries no other meaning. */
#define STORE_MAGIC 0x53455130u
/* Bump whenever tiles_seq_store_data_t's own layout changes -- a stale
 * version on either slot is treated exactly like a corrupt one (see
 * blob_is_valid() below), so a firmware update that changes the format
 * safely falls back to fresh defaults instead of misreading old data. */
#define STORE_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    /* Monotonically increasing across saves (never reset) -- whichever
     * of the two slots has the HIGHER sequence number is the more
     * recent one, letting tiles_seq_store_load() and the alternation
     * logic below always agree on which is "current" without needing
     * a separate index/pointer stored anywhere else. */
    uint32_t sequence;
    tiles_seq_store_data_t data;
    /* Over every byte above (magic through the end of `data`), computed
     * fresh on every save and checked on every load -- see blob_is_
     * valid() below. */
    uint32_t crc32;
} store_blob_t;

#define SLOT_A_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)
#define SLOT_B_OFFSET (PICO_FLASH_SIZE_BYTES - 1 * FLASH_SECTOR_SIZE)

/* Fits comfortably today (well under half the sector), but a build-time
 * guard rather than a hoped-for assumption -- if this struct ever grows
 * past one sector, the build fails loudly here instead of silently
 * truncating writes at runtime. */
_Static_assert(sizeof(store_blob_t) <= FLASH_SECTOR_SIZE, "tiles_seq_store's on-flash blob must fit one sector");

/* Set up once, the first time either tiles_seq_store_load() or _save()
 * runs, by actually reading both flash slots -- NOT just seeded at 1/
 * SLOT_A every boot, which would pick the wrong "next" slot (and risk
 * a sequence-number tie) if both slots already hold real data from a
 * previous session. */
static bool s_alternation_initialized;
static uint32_t s_next_sequence;
static uint32_t s_next_slot_offset;

static uint32_t crc32_compute(const uint8_t *data, size_t len) {
    /* Plain bit-by-bit CRC32 (standard 0xEDB88320 polynomial), not a
     * table-driven one -- this runs once per save, on ~3KB, nowhere
     * near hot enough to justify a 256-entry lookup table's extra code
     * size for a firmware this small. */
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; bit++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static const store_blob_t *slot_ptr(uint32_t offset) {
    return (const store_blob_t *)(XIP_BASE + offset);
}

static bool blob_is_valid(const store_blob_t *blob) {
    if (blob->magic != STORE_MAGIC || blob->version != STORE_VERSION) {
        return false;
    }
    return crc32_compute((const uint8_t *)blob, offsetof(store_blob_t, crc32)) == blob->crc32;
}

static void init_alternation_if_needed(void) {
    if (s_alternation_initialized) {
        return;
    }
    s_alternation_initialized = true;

    const store_blob_t *a = slot_ptr(SLOT_A_OFFSET);
    const store_blob_t *b = slot_ptr(SLOT_B_OFFSET);
    bool a_valid = blob_is_valid(a);
    bool b_valid = blob_is_valid(b);

    if (!a_valid && !b_valid) {
        /* Genuinely first boot ever (or both slots corrupt/stale) --
         * start the alternation fresh from slot A. */
        s_next_sequence = 1u;
        s_next_slot_offset = SLOT_A_OFFSET;
        return;
    }

    uint32_t winner_sequence;
    if (a_valid && b_valid) {
        bool a_is_newer = a->sequence >= b->sequence;
        winner_sequence = a_is_newer ? a->sequence : b->sequence;
        s_next_slot_offset = a_is_newer ? SLOT_B_OFFSET : SLOT_A_OFFSET;
    } else if (a_valid) {
        winner_sequence = a->sequence;
        s_next_slot_offset = SLOT_B_OFFSET;
    } else {
        winner_sequence = b->sequence;
        s_next_slot_offset = SLOT_A_OFFSET;
    }
    s_next_sequence = winner_sequence + 1u;
}

bool tiles_seq_store_load(tiles_seq_store_data_t *out) {
    init_alternation_if_needed();

    const store_blob_t *a = slot_ptr(SLOT_A_OFFSET);
    const store_blob_t *b = slot_ptr(SLOT_B_OFFSET);
    bool a_valid = blob_is_valid(a);
    bool b_valid = blob_is_valid(b);

    const store_blob_t *winner = NULL;
    if (a_valid && b_valid) {
        winner = (a->sequence >= b->sequence) ? a : b;
    } else if (a_valid) {
        winner = a;
    } else if (b_valid) {
        winner = b;
    }
    if (winner == NULL) {
        return false;
    }
    memcpy(out, &winner->data, sizeof(*out));
    return true;
}

void tiles_seq_store_save(const tiles_seq_store_data_t *data) {
    init_alternation_if_needed();

    /* Fully assembled on the stack (well, .bss -- see `static` below)
     * BEFORE touching flash at all, so the actual erase+program window
     * with interrupts disabled is as short as possible -- no formatting
     * or computation happens while the system is stalled. static, not a
     * local array, so this doesn't need FLASH_SECTOR_SIZE (4KB) of
     * stack. Zeroed first so the unused tail past sizeof(store_blob_t)
     * (this struct is well under one sector -- see the _Static_assert
     * above) reads back as a clean, deterministic 0 rather than
     * whatever happened to be on the stack/bss before, even though
     * nothing ever reads that tail. */
    static uint8_t sector_buf[FLASH_SECTOR_SIZE];
    memset(sector_buf, 0, sizeof(sector_buf));
    store_blob_t *blob = (store_blob_t *)sector_buf;
    blob->magic = STORE_MAGIC;
    blob->version = STORE_VERSION;
    blob->sequence = s_next_sequence;
    blob->data = *data;
    blob->crc32 = crc32_compute((const uint8_t *)blob, offsetof(store_blob_t, crc32));

    uint32_t offset = s_next_slot_offset;

    uint32_t saved_irq = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    flash_range_program(offset, sector_buf, FLASH_SECTOR_SIZE);
    restore_interrupts(saved_irq);

    s_next_sequence++;
    s_next_slot_offset = (offset == SLOT_A_OFFSET) ? SLOT_B_OFFSET : SLOT_A_OFFSET;
}
