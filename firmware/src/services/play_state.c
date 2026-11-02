#include "play_state.h"

#include "log_store.h"
#include "note_map.h"

#include <stddef.h>
#include <string.h>

/* Record payload, version 1: mode, scale, octave (int8), key. */
#define PLAY_STATE_VERSION 1u
#define PLAY_STATE_BYTES 4u
/* First guesses: long enough that stepping through scales or octaves saves
 * once, at the end; a failed write retries a few seconds later. */
#define PLAY_STATE_QUIET_MS 2000u
#define PLAY_STATE_RETRY_MS 5000u

static tiles_log_t s_log;
static bool s_storage;
static uint8_t s_saved[PLAY_STATE_BYTES];
static bool s_have_saved;
/* What the last scan saw, and when it last changed (times the quiet gap). */
static uint8_t s_seen[PLAY_STATE_BYTES];
static uint32_t s_changed_ms;
static uint32_t s_retry_after_ms; /* 0 = no retry pending */
static bool s_boot_mode_valid;
static uint8_t s_boot_mode;
static uint32_t s_saves;

static void snapshot(uint8_t mode, uint8_t *out) {
    out[0] = mode;
    out[1] = (uint8_t)tiles_note_map_get_scale();
    out[2] = (uint8_t)tiles_note_map_get_octave_shift();
    out[3] = (uint8_t)tiles_note_map_get_key_offset();
}

void tiles_play_state_init(const tiles_kv_ops_t *ops, bool crash_recovered) {
    s_storage = ops != NULL;
    s_have_saved = false;
    s_boot_mode_valid = false;
    s_changed_ms = 0u;
    s_retry_after_ms = 0u;
    s_saves = 0u;
    memset(s_seen, 0, sizeof(s_seen));
    tiles_log_init(&s_log, ops);
    uint8_t buf[16];
    uint16_t len = 0u, version = 0u;
    if (!s_storage || !tiles_log_read(&s_log, buf, sizeof(buf), &len, &version) || version != PLAY_STATE_VERSION ||
        len < PLAY_STATE_BYTES) {
        return;
    }
    memcpy(s_saved, buf, PLAY_STATE_BYTES);
    s_have_saved = true;
    if (crash_recovered) {
        return;
    }
    if (buf[1] < (uint8_t)TILES_NUM_SCALE_VALUES) {
        tiles_note_map_set_scale((tiles_scale_mode_t)buf[1]);
    }
    tiles_note_map_set_octave_shift((int8_t)buf[2]); /* clamps */
    tiles_note_map_set_key_offset((int8_t)buf[3]);   /* wraps */
    s_boot_mode = buf[0];
    s_boot_mode_valid = true;
}

bool tiles_play_state_boot_mode(uint8_t *mode) {
    if (s_boot_mode_valid) {
        *mode = s_boot_mode;
    }
    return s_boot_mode_valid;
}

void tiles_play_state_service(uint32_t now_ms, uint8_t mode, bool clock_running, bool pads_idle) {
    if (!s_storage) {
        return;
    }
    uint8_t now_state[PLAY_STATE_BYTES];
    snapshot(mode, now_state);
    if (memcmp(now_state, s_seen, sizeof(now_state)) != 0) {
        memcpy(s_seen, now_state, sizeof(now_state));
        s_changed_ms = now_ms;
    }
    if (s_have_saved && memcmp(now_state, s_saved, sizeof(now_state)) == 0) {
        /* Nothing to save. Idle and stopped: erase the spare sector now, so
         * saves while playing never need to. */
        if (!clock_running && pads_idle && s_log.have_newest && !s_log.blank[1u - s_log.newest_slot]) {
            (void)tiles_log_prepare(&s_log);
        }
        return;
    }
    if (now_ms - s_changed_ms < PLAY_STATE_QUIET_MS || !pads_idle ||
        (s_retry_after_ms != 0u && (int32_t)(now_ms - s_retry_after_ms) < 0)) {
        return;
    }
    tiles_log_result_t r = tiles_log_append(&s_log, now_state, PLAY_STATE_BYTES, PLAY_STATE_VERSION, !clock_running);
    if (r == TILES_LOG_ERR_NEEDS_ERASE) {
        return; /* saved once the clock stops */
    }
    if (r != TILES_LOG_OK) {
        s_retry_after_ms = now_ms + PLAY_STATE_RETRY_MS;
        if (s_retry_after_ms == 0u) {
            s_retry_after_ms = 1u;
        }
        return;
    }
    memcpy(s_saved, now_state, sizeof(now_state));
    s_have_saved = true;
    s_retry_after_ms = 0u;
    s_saves++;
}

tiles_play_state_info_t tiles_play_state_get_info(void) {
    tiles_play_state_info_t info;
    memset(&info, 0, sizeof(info));
    info.storage = s_storage;
    info.have_saved = s_have_saved;
    info.pending = s_storage && (!s_have_saved || memcmp(s_seen, s_saved, sizeof(s_saved)) != 0);
    if (s_have_saved) {
        info.mode = s_saved[0];
        info.scale = s_saved[1];
        info.octave = (int8_t)s_saved[2];
        info.key = (int8_t)s_saved[3];
    }
    info.saves = s_saves;
    return info;
}
