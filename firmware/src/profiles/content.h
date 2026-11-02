#pragma once

/* Content store: what the companion app pushes (today: custom scales in
 * CUSTOM_1..9), each item tagged with its pack (content-packs.md). One blob
 * in a two-slot kv_store region, records of:
 *   u8 type, u8 slot, u16 len, body   (little-endian)
 * Unknown types are kept as-is. Scale body: u16 version, u8 count, count x
 * i8 interval, then name, pack, item (u8 length + bytes). Every change is
 * saved before it's applied, so "OK" means saved. Tested natively. */

#include "kv_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TILES_CONTENT_BLOB_VERSION 1u

#define TILES_CONTENT_TYPE_SCALE 1u

/* Name, pack and item: 1-16 printable ASCII characters, no spaces (the
 * vendor protocol is space-separated; an app shows '_' as a space if it
 * likes). pack and item may be empty: a user-made scale. */
#define TILES_CONTENT_TEXT_MAX 16u

typedef struct {
    uint8_t slot; /* 1-9 = TILES_SCALE_CUSTOM_1..9 */
    uint16_t version;
    uint8_t count;
    int8_t intervals[12];
    char name[TILES_CONTENT_TEXT_MAX + 1u];
    char pack[TILES_CONTENT_TEXT_MAX + 1u];
    char item[TILES_CONTENT_TEXT_MAX + 1u];
} tiles_content_scale_t;

typedef enum {
    TILES_CONTENT_OK = 0,
    TILES_CONTENT_BAD_SLOT,
    TILES_CONTENT_BAD_INTERVALS, /* see tiles_note_map_custom_scale_valid() */
    TILES_CONTENT_BAD_TEXT,      /* name missing, or a field too long / with a bad character */
    TILES_CONTENT_FULL,          /* the store's blob would exceed one kv payload */
    TILES_CONTENT_NEWER_FORMAT,  /* flash holds a newer firmware's blob: read-only until CLEAR */
    TILES_CONTENT_NO_STORAGE,    /* no flash region (image overlaps it): changes refused */
    TILES_CONTENT_SAVE_FAILED,   /* the kv write failed; see tiles_content_get_info().kv.last_result */
} tiles_content_result_t;

typedef struct {
    bool storage;      /* has a flash region */
    bool newer_format; /* flash blob from a newer firmware (not applied) */
    uint16_t used_bytes;
    uint16_t capacity_bytes;
    uint8_t scales;     /* custom slots holding a scale */
    size_t records;     /* all records, unknown types included */
    tiles_kv_info_t kv;
} tiles_content_info_t;

/* Loads the store and pushes its scales into the note map. Call after
 * tiles_note_map_init(). ops NULL = no flash region: starts empty and
 * refuses changes. */
void tiles_content_init(const tiles_kv_ops_t *ops);

/* Validates, saves, then applies. Replaces whatever is in that slot. */
tiles_content_result_t tiles_content_put_scale(const tiles_content_scale_t *scale);

/* Empties a slot (OK if it was already empty). */
tiles_content_result_t tiles_content_delete_scale(uint8_t slot);

/* Copies slot 1-9 into `out`; false if empty. */
bool tiles_content_get_scale(uint8_t slot, tiles_content_scale_t *out);

/* Wipes everything, including records of unknown types and a newer-format
 * blob (the way out of TILES_CONTENT_NEWER_FORMAT). */
tiles_content_result_t tiles_content_clear(void);

/* Record i (0..records-1) in store order: type, slot, body size, and for a
 * scale its pack/item/version (empty strings and 0 for unknown types).
 * False past the end. */
typedef struct {
    uint8_t type;
    uint8_t slot;
    uint16_t bytes;
    uint16_t version;
    char pack[TILES_CONTENT_TEXT_MAX + 1u];
    char item[TILES_CONTENT_TEXT_MAX + 1u];
} tiles_content_record_t;
bool tiles_content_record_at(size_t i, tiles_content_record_t *out);

tiles_content_info_t tiles_content_get_info(void);

/* True if `text` is a valid name/pack/item (allow_empty for pack/item). */
bool tiles_content_text_valid(const char *text, bool allow_empty);
