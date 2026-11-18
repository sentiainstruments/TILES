#include "fm_bank.h"

#include <string.h>

#define VOICES_BYTES (TILES_DX7_BANK_VOICES * TILES_DX7_PACKED_BYTES) /* 4,096 */
#define PAGE 256u
#define HEADER_MAGIC 0x4B4E4258u /* "XBNK" little-endian */
#define HEADER_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t crc; /* of the 4,096 voice bytes */
} header_t;

static const tiles_kv_ops_t *s_ops;
static bool s_present;

static struct {
    bool active;
    uint32_t pos; /* sysex bytes taken */
    uint32_t sum; /* the DX7 checksum's running sum */
    uint32_t sysex_crc, want_crc, voices_crc;
    tiles_fm_bank_result_t result;
    uint32_t page_fill;
    uint8_t page[PAGE];
} s_load;

static bool read_header(header_t *h) {
    return s_ops != NULL && s_ops->read(1u, 0u, (uint8_t *)h, sizeof(*h)) && h->magic == HEADER_MAGIC &&
           h->version == HEADER_VERSION;
}

void tiles_fm_bank_init(const tiles_kv_ops_t *ops) {
    s_ops = ops;
    memset(&s_load, 0, sizeof(s_load));
    header_t h;
    s_present = false;
    if (read_header(&h)) {
        uint8_t buf[PAGE];
        uint32_t crc = 0u;
        bool ok = true;
        for (uint32_t off = 0; off < VOICES_BYTES && ok; off += PAGE) {
            ok = s_ops->read(0u, off, buf, PAGE);
            crc = tiles_kv_crc32(crc, buf, PAGE);
        }
        s_present = ok && crc == h.crc;
    }
}

bool tiles_fm_bank_present(void) {
    return s_present;
}

tiles_fm_bank_result_t tiles_fm_bank_begin(uint32_t sysex_crc32) {
    if (s_ops == NULL) {
        return TILES_FM_BANK_ERR_NO_STORAGE;
    }
    if (s_load.active) {
        return TILES_FM_BANK_ERR_STATE;
    }
    s_present = false;
    if (!s_ops->erase(1u) || !s_ops->erase(0u)) {
        return TILES_FM_BANK_ERR_FLASH;
    }
    memset(&s_load, 0, sizeof(s_load));
    s_load.active = true;
    s_load.want_crc = sysex_crc32;
    return TILES_FM_BANK_OK;
}

uint32_t tiles_fm_bank_remaining(void) {
    return s_load.active ? TILES_DX7_BANK_SYSEX_BYTES - s_load.pos : 0u;
}

static void take(uint8_t b) {
    static const uint8_t k_head[6] = {0xF0u, 0x43u, 0x00u, 0x09u, 0x20u, 0x00u};
    uint32_t i = s_load.pos++;
    if (s_load.result != TILES_FM_BANK_OK) {
        return;
    }
    if (i < 6u) {
        bool ok = i == 2u ? (b & 0xF0u) == 0u : b == k_head[i]; /* 0n: any MIDI channel */
        s_load.result = ok ? TILES_FM_BANK_OK : TILES_FM_BANK_ERR_NOT_DX7;
    } else if (i < 6u + VOICES_BYTES) {
        s_load.sum += b;
        s_load.page[s_load.page_fill++] = b;
        if (s_load.page_fill == PAGE) {
            s_load.voices_crc = tiles_kv_crc32(s_load.voices_crc, s_load.page, PAGE);
            if (!s_ops->program(0u, i - 6u + 1u - PAGE, s_load.page, PAGE)) {
                s_load.result = TILES_FM_BANK_ERR_FLASH;
            }
            s_load.page_fill = 0u;
        }
    } else if (i == 6u + VOICES_BYTES) {
        if (((128u - (s_load.sum & 127u)) & 127u) != b) {
            s_load.result = TILES_FM_BANK_ERR_CHECKSUM;
        }
    } else if (b != 0xF7u) {
        s_load.result = TILES_FM_BANK_ERR_NOT_DX7;
    }
}

tiles_fm_bank_result_t tiles_fm_bank_write(const uint8_t *data, uint32_t len) {
    if (!s_load.active) {
        return TILES_FM_BANK_ERR_STATE;
    }
    for (uint32_t k = 0; k < len && s_load.pos < TILES_DX7_BANK_SYSEX_BYTES; k++) {
        take(data[k]);
    }
    s_load.sysex_crc = tiles_kv_crc32(s_load.sysex_crc, data, len);
    return s_load.result;
}

tiles_fm_bank_result_t tiles_fm_bank_finish(void) {
    if (!s_load.active) {
        return TILES_FM_BANK_ERR_STATE;
    }
    s_load.active = false;
    tiles_fm_bank_result_t r = s_load.result;
    if (r == TILES_FM_BANK_OK && s_load.pos != TILES_DX7_BANK_SYSEX_BYTES) {
        r = TILES_FM_BANK_ERR_NOT_DX7;
    }
    if (r == TILES_FM_BANK_OK && s_load.sysex_crc != s_load.want_crc) {
        r = TILES_FM_BANK_ERR_CRC;
    }
    if (r != TILES_FM_BANK_OK) {
        return r;
    }
    uint8_t page[PAGE];
    memset(page, 0xFF, sizeof(page));
    header_t h = {HEADER_MAGIC, HEADER_VERSION, s_load.voices_crc};
    memcpy(page, &h, sizeof(h));
    if (!s_ops->program(1u, 0u, page, PAGE)) {
        return TILES_FM_BANK_ERR_FLASH;
    }
    s_present = true;
    return TILES_FM_BANK_OK;
}

static bool read_packed(uint8_t voice, uint8_t packed[TILES_DX7_PACKED_BYTES]) {
    return s_present && voice < TILES_DX7_BANK_VOICES &&
           s_ops->read(0u, (uint32_t)voice * TILES_DX7_PACKED_BYTES, packed, TILES_DX7_PACKED_BYTES);
}

bool tiles_fm_bank_name(uint8_t voice, char name[11]) {
    uint8_t packed[TILES_DX7_PACKED_BYTES], v[TILES_DX7_VOICE_BYTES];
    if (!read_packed(voice, packed)) {
        return false;
    }
    tiles_dx7_unpack(packed, v);
    tiles_dx7_name(v, name);
    return true;
}

bool tiles_fm_bank_voice(uint8_t voice, uint8_t out[TILES_DX7_VOICE_BYTES]) {
    uint8_t packed[TILES_DX7_PACKED_BYTES];
    if (!read_packed(voice, packed)) {
        return false;
    }
    tiles_dx7_unpack(packed, out);
    return true;
}

const char *tiles_fm_bank_result_name(tiles_fm_bank_result_t r) {
    switch (r) {
    case TILES_FM_BANK_OK:
        return "ok";
    case TILES_FM_BANK_ERR_NO_STORAGE:
        return "no-storage";
    case TILES_FM_BANK_ERR_FLASH:
        return "flash";
    case TILES_FM_BANK_ERR_NOT_DX7:
        return "not-a-dx7-bank";
    case TILES_FM_BANK_ERR_CHECKSUM:
        return "checksum";
    case TILES_FM_BANK_ERR_CRC:
        return "crc";
    default:
        return "state";
    }
}
