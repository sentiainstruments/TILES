#include "settings_persist.h"

#include "settings.h"

#include <string.h>

/* 64 settings x 7 bytes is the registry's own ceiling (see settings.c). */
#define BLOB_CAP 512u

static bool s_ready;
static tiles_settings_idle_fn s_idle;

static uint8_t s_saved[BLOB_CAP]; /* what flash holds */
static uint16_t s_saved_len;
static uint8_t s_pending[BLOB_CAP]; /* the changed snapshot being debounced */
static uint16_t s_pending_len;
static bool s_pending_valid;
static uint32_t s_changed_at_ms;
static uint32_t s_last_poll_ms;
static uint32_t s_retry_after_ms;
static bool s_have_poll;

static bool s_loaded;
static size_t s_applied;
static uint32_t s_save_failures;

void tiles_settings_persist_init(const tiles_kv_ops_t *ops, tiles_settings_idle_fn idle) {
    s_idle = idle;
    s_saved_len = 0u;
    s_pending_valid = false;
    s_have_poll = false;
    s_retry_after_ms = 0u;
    s_loaded = false;
    s_applied = 0u;
    s_save_failures = 0u;

    tiles_kv_init(ops);
    uint16_t len = 0u;
    uint16_t version = 0u;
    if (tiles_kv_read(s_saved, sizeof(s_saved), &len, &version) && version == TILES_SETTINGS_BLOB_VERSION) {
        s_saved_len = len;
        s_applied = tiles_settings_apply_blob(s_saved, len);
        s_loaded = true;
    }
    /* A snapshot from an unknown (newer) blob version is ignored, not applied
     * and not "fixed" -- the first real change simply overwrites it. */
    s_ready = true;
}

static bool same(const uint8_t *a, uint16_t alen, const uint8_t *b, uint16_t blen) {
    return alen == blen && (alen == 0u || memcmp(a, b, alen) == 0);
}

static tiles_kv_result_t write_snapshot(const uint8_t *blob, uint16_t len) {
    tiles_kv_result_t r = tiles_kv_write(blob, len, TILES_SETTINGS_BLOB_VERSION);
    if (r == TILES_KV_OK) {
        memcpy(s_saved, blob, len);
        s_saved_len = len;
        s_pending_valid = false;
    } else {
        s_save_failures++;
    }
    return r;
}

void tiles_settings_persist_service(uint32_t now_ms) {
    if (!s_ready) {
        return;
    }
    if (s_have_poll && (uint32_t)(now_ms - s_last_poll_ms) < TILES_SETTINGS_POLL_MS) {
        return;
    }
    s_have_poll = true;
    s_last_poll_ms = now_ms;

    uint8_t cur[BLOB_CAP];
    bool overflow;
    size_t n = tiles_settings_serialize(cur, sizeof(cur), &overflow);
    if (overflow) {
        return; /* can't be saved; never expected -- BLOB_CAP covers the whole registry */
    }
    uint16_t cur_len = (uint16_t)n;

    if (same(cur, cur_len, s_saved, s_saved_len)) {
        s_pending_valid = false;
        return;
    }
    if (!s_pending_valid || !same(cur, cur_len, s_pending, s_pending_len)) {
        memcpy(s_pending, cur, cur_len);
        s_pending_len = cur_len;
        s_pending_valid = true;
        s_changed_at_ms = now_ms; /* still changing: restart the quiet period */
        return;
    }
    if ((uint32_t)(now_ms - s_changed_at_ms) < TILES_SETTINGS_SAVE_DEBOUNCE_MS) {
        return;
    }
    if (s_retry_after_ms != 0u && (int32_t)(now_ms - s_retry_after_ms) < 0) {
        return;
    }
    if (s_idle != NULL && !s_idle()) {
        return; /* hands on the pads: wait, keep the change pending */
    }
    if (write_snapshot(cur, cur_len) != TILES_KV_OK) {
        s_retry_after_ms = now_ms + TILES_SETTINGS_SAVE_RETRY_MS;
        if (s_retry_after_ms == 0u) {
            s_retry_after_ms = 1u;
        }
    } else {
        s_retry_after_ms = 0u;
    }
}

tiles_kv_result_t tiles_settings_persist_save_now(void) {
    if (!s_ready) {
        return TILES_KV_ERR_NOT_INIT;
    }
    uint8_t cur[BLOB_CAP];
    bool overflow;
    size_t n = tiles_settings_serialize(cur, sizeof(cur), &overflow);
    if (overflow) {
        return TILES_KV_ERR_TOO_BIG;
    }
    if (same(cur, (uint16_t)n, s_saved, s_saved_len)) {
        return TILES_KV_OK;
    }
    return write_snapshot(cur, (uint16_t)n);
}

tiles_settings_persist_info_t tiles_settings_persist_get_info(void) {
    tiles_settings_persist_info_t i;
    i.kv = tiles_kv_get_info();
    i.loaded = s_loaded;
    i.applied = s_applied;
    i.pending = s_pending_valid;
    i.save_failures = s_save_failures;
    i.saved_bytes = s_saved_len;
    return i;
}
