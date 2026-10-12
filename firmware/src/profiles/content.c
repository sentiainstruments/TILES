#include "content.h"

#include "note_map.h"

#include <string.h>

#define RECORD_HEADER 4u

static tiles_kv_t s_kv;
static bool s_storage;
static bool s_newer_format;

/* What flash holds (and what's applied). Static, not on the stack: one kv
 * payload is ~4 KB. s_work builds the next version of the blob. */
static uint8_t s_blob[TILES_KV_MAX_PAYLOAD];
static uint16_t s_len;
static uint8_t s_work[TILES_KV_MAX_PAYLOAD];

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool tiles_content_text_valid(const char *text, bool allow_empty) {
    if (text == NULL) {
        return false;
    }
    size_t n = strlen(text);
    if (n > TILES_CONTENT_TEXT_MAX || (n == 0u && !allow_empty)) {
        return false;
    }
    for (size_t i = 0u; i < n; i++) {
        if (text[i] <= ' ' || text[i] > '~') {
            return false;
        }
    }
    return true;
}

/* ---- record walking ---- */

/* Offset of the record after the one at `pos`, or 0 if the record at `pos`
 * runs past `len` (a damaged blob: everything from there on is ignored). */
static uint16_t next_record(const uint8_t *blob, uint16_t len, uint16_t pos) {
    if (pos + RECORD_HEADER > len) {
        return 0u;
    }
    uint32_t end = (uint32_t)pos + RECORD_HEADER + get_u16(&blob[pos + 2u]);
    return end <= len ? (uint16_t)end : 0u;
}

/* Reads one short string; false if it runs past the body or is invalid. */
static bool read_text(const uint8_t *body, uint16_t body_len, uint16_t *at, char *out, bool allow_empty) {
    if (*at >= body_len) {
        return false;
    }
    uint8_t n = body[*at];
    if (n > TILES_CONTENT_TEXT_MAX || (uint32_t)*at + 1u + n > body_len) {
        return false;
    }
    memcpy(out, &body[*at + 1u], n);
    out[n] = '\0';
    *at = (uint16_t)(*at + 1u + n);
    return tiles_content_text_valid(out, allow_empty);
}

static bool decode_scale(uint8_t slot, const uint8_t *body, uint16_t body_len, tiles_content_scale_t *out) {
    memset(out, 0, sizeof(*out));
    out->slot = slot;
    if (body_len < 3u) {
        return false;
    }
    out->version = get_u16(&body[0]);
    out->count = body[2];
    if (out->count > TILES_NOTE_MAP_MAX_SCALE_NOTES || 3u + out->count > body_len) {
        return false;
    }
    for (uint8_t i = 0u; i < out->count; i++) {
        out->intervals[i] = (int8_t)body[3u + i];
    }
    uint16_t at = (uint16_t)(3u + out->count);
    return tiles_note_map_custom_scale_valid(out->intervals, out->count) &&
           read_text(body, body_len, &at, out->name, false) && read_text(body, body_len, &at, out->pack, true) &&
           read_text(body, body_len, &at, out->item, true);
}

static uint16_t encode_scale(const tiles_content_scale_t *s, uint8_t *body) {
    put_u16(&body[0], s->version);
    body[2] = s->count;
    uint16_t at = 3u;
    for (uint8_t i = 0u; i < s->count; i++) {
        body[at++] = (uint8_t)s->intervals[i];
    }
    const char *texts[3] = {s->name, s->pack, s->item};
    for (uint8_t t = 0u; t < 3u; t++) {
        uint8_t n = (uint8_t)strlen(texts[t]);
        body[at++] = n;
        memcpy(&body[at], texts[t], n);
        at = (uint16_t)(at + n);
    }
    return at;
}

/* Largest encoded scale body: version, count, 12 intervals, 3 x (1 + 16). */
#define SCALE_BODY_MAX (3u + 12u + 3u * (1u + TILES_CONTENT_TEXT_MAX))

/* Finds the record of `type` in `slot`; its offset, or -1. */
static int32_t find_record(uint8_t type, uint8_t slot) {
    uint16_t pos = 0u;
    while (pos < s_len) {
        uint16_t next = next_record(s_blob, s_len, pos);
        if (next == 0u) {
            break;
        }
        if (s_blob[pos] == type && s_blob[pos + 1u] == slot) {
            return pos;
        }
        pos = next;
    }
    return -1;
}

/* Pushes every scale record into the note map; slots without one are
 * emptied. A damaged scale record leaves its slot empty. */
static void apply_scales(void) {
    for (uint8_t slot = 1u; slot <= TILES_NOTE_MAP_NUM_CUSTOM_SCALES; slot++) {
        tiles_content_scale_t s;
        if (tiles_content_get_scale(slot, &s)) {
            (void)tiles_note_map_set_custom_scale(slot, s.intervals, s.count);
        } else {
            (void)tiles_note_map_set_custom_scale(slot, NULL, 0u);
        }
    }
}

void tiles_content_init(const tiles_kv_ops_t *ops) {
    s_len = 0u;
    s_newer_format = false;
    s_storage = ops != NULL;
    tiles_kv_init(&s_kv, ops);
    if (s_storage) {
        uint16_t len = 0u;
        uint16_t version = 0u;
        if (tiles_kv_read(&s_kv, s_blob, sizeof(s_blob), &len, &version)) {
            if (version == TILES_CONTENT_BLOB_VERSION) {
                s_len = len;
            } else {
                s_newer_format = true; /* not ours to read, and not ours to overwrite */
            }
        }
    }
    apply_scales();
}

/* Writes s_work[0..len) as the new store, then makes it current. */
static tiles_content_result_t commit(uint16_t len) {
    if (!s_storage) {
        return TILES_CONTENT_NO_STORAGE;
    }
    if (tiles_kv_write(&s_kv, s_work, len, TILES_CONTENT_BLOB_VERSION) != TILES_KV_OK) {
        return TILES_CONTENT_SAVE_FAILED;
    }
    memcpy(s_blob, s_work, len);
    s_len = len;
    s_newer_format = false;
    apply_scales();
    return TILES_CONTENT_OK;
}

/* Copies every intact record except (type, slot) into s_work; its length. */
static uint16_t copy_without(uint8_t type, uint8_t slot) {
    uint16_t out = 0u;
    uint16_t pos = 0u;
    while (pos < s_len) {
        uint16_t next = next_record(s_blob, s_len, pos);
        if (next == 0u) {
            break;
        }
        if (!(s_blob[pos] == type && s_blob[pos + 1u] == slot)) {
            memcpy(&s_work[out], &s_blob[pos], (size_t)(next - pos));
            out = (uint16_t)(out + (next - pos));
        }
        pos = next;
    }
    return out;
}

tiles_content_result_t tiles_content_put_scale(const tiles_content_scale_t *scale) {
    if (scale == NULL || scale->slot < 1u || scale->slot > TILES_NOTE_MAP_NUM_CUSTOM_SCALES) {
        return TILES_CONTENT_BAD_SLOT;
    }
    if (!tiles_note_map_custom_scale_valid(scale->intervals, scale->count)) {
        return TILES_CONTENT_BAD_INTERVALS;
    }
    if (!tiles_content_text_valid(scale->name, false) || !tiles_content_text_valid(scale->pack, true) ||
        !tiles_content_text_valid(scale->item, true)) {
        return TILES_CONTENT_BAD_TEXT;
    }
    if (s_newer_format) {
        return TILES_CONTENT_NEWER_FORMAT;
    }
    if (!s_storage) {
        return TILES_CONTENT_NO_STORAGE;
    }
    uint16_t len = copy_without(TILES_CONTENT_TYPE_SCALE, scale->slot);
    if ((uint32_t)len + RECORD_HEADER + SCALE_BODY_MAX > sizeof(s_work)) {
        return TILES_CONTENT_FULL;
    }
    uint16_t body = encode_scale(scale, &s_work[len + RECORD_HEADER]);
    s_work[len] = TILES_CONTENT_TYPE_SCALE;
    s_work[len + 1u] = scale->slot;
    put_u16(&s_work[len + 2u], body);
    return commit((uint16_t)(len + RECORD_HEADER + body));
}

tiles_content_result_t tiles_content_delete_scale(uint8_t slot) {
    if (slot < 1u || slot > TILES_NOTE_MAP_NUM_CUSTOM_SCALES) {
        return TILES_CONTENT_BAD_SLOT;
    }
    if (s_newer_format) {
        return TILES_CONTENT_NEWER_FORMAT;
    }
    if (find_record(TILES_CONTENT_TYPE_SCALE, slot) < 0) {
        return TILES_CONTENT_OK; /* already empty: no flash write */
    }
    return commit(copy_without(TILES_CONTENT_TYPE_SCALE, slot));
}

bool tiles_content_get_scale(uint8_t slot, tiles_content_scale_t *out) {
    int32_t pos = find_record(TILES_CONTENT_TYPE_SCALE, slot);
    if (pos < 0) {
        return false;
    }
    return decode_scale(slot, &s_blob[pos + RECORD_HEADER], get_u16(&s_blob[pos + 2]), out);
}

tiles_content_result_t tiles_content_clear(void) {
    if (s_len == 0u && !s_newer_format) {
        return s_storage ? TILES_CONTENT_OK : TILES_CONTENT_NO_STORAGE;
    }
    return commit(0u);
}

bool tiles_content_record_at(size_t i, tiles_content_record_t *out) {
    uint16_t pos = 0u;
    size_t index = 0u;
    while (pos < s_len) {
        uint16_t next = next_record(s_blob, s_len, pos);
        if (next == 0u) {
            return false;
        }
        if (index == i) {
            memset(out, 0, sizeof(*out));
            out->type = s_blob[pos];
            out->slot = s_blob[pos + 1u];
            out->bytes = get_u16(&s_blob[pos + 2u]);
            tiles_content_scale_t s;
            if (out->type == TILES_CONTENT_TYPE_SCALE && decode_scale(out->slot, &s_blob[pos + RECORD_HEADER], out->bytes, &s)) {
                out->version = s.version;
                memcpy(out->pack, s.pack, sizeof(out->pack));
                memcpy(out->item, s.item, sizeof(out->item));
            }
            return true;
        }
        index++;
        pos = next;
    }
    return false;
}

tiles_content_info_t tiles_content_get_info(void) {
    tiles_content_info_t info;
    memset(&info, 0, sizeof(info));
    info.storage = s_storage;
    info.newer_format = s_newer_format;
    info.used_bytes = s_len;
    info.capacity_bytes = (uint16_t)sizeof(s_blob);
    tiles_content_record_t r;
    while (tiles_content_record_at(info.records, &r)) {
        info.records++;
    }
    for (uint8_t slot = 1u; slot <= TILES_NOTE_MAP_NUM_CUSTOM_SCALES; slot++) {
        tiles_content_scale_t s;
        if (tiles_content_get_scale(slot, &s)) {
            info.scales++;
        }
    }
    info.kv = tiles_kv_get_info(&s_kv);
    return info;
}
