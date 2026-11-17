#include "usb_vendor.h"

#include "bend_core.h"
#include "board/unit_id.h"
#include "board_pins.h"
#include "buttons.h"
#include "content.h"
#include "device_mode.h"
#include "diagnostics/capture.h"
#include "diagnostics/boot_stage.h"
#include "diagnostics/fault.h"
#include "diagnostics/loop_stats.h"
#include "drum_seq.h"
#include "expression.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_clock.h"
#include "note_map.h"
#include "op_mode.h"
#include "play_state.h"
#include "power.h"
#include "product_identity.h"
#include "sample_store.h"
#include "settings.h"
#include "settings_persist.h"
#include "storage_flash.h"
#include "touch.h"
#include "audio_engine.h"
#include "fx.h"
#include "usb_audio.h"
#include "usb_device.h"
#include "tune.h"

#include "tusb.h"

#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_VENDOR_LINE_MAX 128u

/* Replies are queued here and drained into the 64-byte TinyUSB FIFO as it
 * has room (pump_out()); the next command is read only once the previous
 * reply is fully sent. Listings that can outgrow it (LIST, SCHEMA -- ~5 KB
 * with the colour rows -- SCALES, CONTENT LIST) are streamed instead: rows
 * are added as the buffer drains (fill_listing()), so their size is
 * unbounded. Single replies (INFO, ~1 KB) are built whole. */
#define USB_VENDOR_OUT_MAX 4096u

static char s_rx_line[USB_VENDOR_LINE_MAX];
static size_t s_rx_len;

/* SAMPLE BEGIN switches the input to raw audio bytes until the announced
 * count has arrived (a failed load keeps taking them, so nothing is read
 * as a command). Up to 4 KB per pass (16 page programs and one sector
 * erase, ~55 ms), pumping USB for up to 6 ms to get it: one 64-byte packet
 * per pass would take 9 s a slot. A sender silent this long is dropped. */
#define USB_VENDOR_RAW_PER_PASS 4096u
#define USB_VENDOR_RAW_PASS_US 6000u
#define USB_VENDOR_RAW_TIMEOUT_MS 3000u
static uint32_t s_raw_left;
static uint32_t s_raw_last_ms;
static tiles_sample_result_t s_raw_result;

/* A partial line (no newline yet) is dropped once nothing has arrived for
 * this long while the device waits for input. A host that gave up
 * mid-write (a batch that timed out, an app quitting) would otherwise glue
 * its tail onto the next session's first command. A real command arrives
 * in one write, far quicker. */
#define USB_VENDOR_PARTIAL_LINE_MS 1000u
static uint32_t s_rx_idle_since_ms;

/* Request tags: a command line may start with "@<tag> " (1-16 of
 * A-Z a-z 0-9 _ -), and then every line of its reply starts with the same
 * "@<tag> ". A host can then keep several commands in flight and match
 * each line to its request, and skip lines left over from an earlier
 * session (a reply still streaming when the last app quit). Untagged
 * commands are answered untagged, as always. */
#define USB_VENDOR_TAG_MAX 16u
static char s_tag[USB_VENDOR_TAG_MAX + 3u]; /* "@" tag " " NUL; empty = untagged */

static char s_out[USB_VENDOR_OUT_MAX];
static size_t s_out_len;  /* bytes queued */
static size_t s_out_sent; /* of those, bytes already handed to TinyUSB */

/* The listing being streamed, and the next row/slot to produce. */
typedef enum {
    LISTING_NONE = 0,
    LISTING_SETTINGS,
    LISTING_SCHEMA,
    LISTING_SCALES,
    LISTING_CONTENT,
    LISTING_CAPTURE,
    LISTING_HALL,
    LISTING_PATHS,
    LISTING_TUNE,
} listing_t;
static listing_t s_listing;
static size_t s_listing_next;
/* CAPTURE READ: the sequence number to stop before, and whether the rows
 * are the compact live ones (READ <from>) with a "seq=" line first. */
static uint32_t s_listing_end;
static bool s_listing_live;
static bool s_listing_header;

/* CAPTURE READ <from> sends at most this many rows; the host asks again. */
#define CAPTURE_READ_MAX_ROWS 300u

/* The recorded pad's bend setup when the capture started, so a replay
 * starts where the device did (CAPTURE STATUS). */
static struct {
    uint8_t mode;
    float sensitivity;
    float bx, by;
    uint16_t strokes;
} s_capture_setup;

static void out_append(const char *text) {
    size_t n = strlen(text);
    /* Written so the sum can't wrap (s_out_len + n + 1 could, in principle,
     * which GCC 15 flags): room for n bytes plus a spare one. */
    if (s_out_len >= sizeof(s_out) || n >= sizeof(s_out) - s_out_len) {
        return; /* never happens for any reply built here; truncates rather than overrun */
    }
    memcpy(&s_out[s_out_len], text, n);
    s_out_len += n;
}

static void reply(const char *text) {
    out_append(s_tag);
    out_append(text);
    out_append("\n");
}

static void reply_ok(void) {
    reply("OK");
}

static void reply_err(const char *reason) {
    char buf[USB_VENDOR_LINE_MAX];
    snprintf(buf, sizeof(buf), "ERR %s", reason);
    reply(buf);
}

/* Hands as much of the queued response to TinyUSB as fits right now. Called
 * every scan, so a long response streams out across iterations. */
static void pump_out(void) {
    while (s_out_sent < s_out_len) {
        uint32_t room = tud_vendor_write_available();
        if (room == 0u) {
            break;
        }
        size_t n = s_out_len - s_out_sent;
        if (n > room) {
            n = room;
        }
        uint32_t wrote = tud_vendor_write(&s_out[s_out_sent], (uint32_t)n);
        if (wrote == 0u) {
            break;
        }
        s_out_sent += wrote;
    }
    tud_vendor_flush();
    if (s_out_sent >= s_out_len) {
        s_out_len = 0u;
        s_out_sent = 0u;
    }
}

static const char *kv_result_name(tiles_kv_result_t r) {
    switch (r) {
    case TILES_KV_OK:
        return "ok";
    case TILES_KV_ERR_NOT_INIT:
        return "not-initialized";
    case TILES_KV_ERR_TOO_BIG:
        return "too-big";
    case TILES_KV_ERR_ERASE:
        return "erase-failed";
    case TILES_KV_ERR_PROGRAM:
        return "program-failed";
    case TILES_KV_ERR_VERIFY:
        return "verify-failed";
    }
    return "unknown";
}

/* "0,2,3,7,8" -- the form SCALE PUT takes and SCALES prints. */
static void format_intervals(const int8_t *intervals, uint8_t count, char *out, size_t cap) {
    size_t at = 0u;
    out[0] = '\0';
    for (uint8_t i = 0u; i < count && at < cap; i++) {
        int n = snprintf(&out[at], cap - at, i == 0u ? "%d" : ",%d", (int)intervals[i]);
        if (n < 0) {
            break;
        }
        at += (size_t)n;
    }
}

static bool parse_intervals(char *text, int8_t *out, uint8_t *count) {
    *count = 0u;
    for (char *tok = strtok(text, ","); tok != NULL; tok = strtok(NULL, ",")) {
        char *end;
        long v = strtol(tok, &end, 10);
        if (*tok == '\0' || *end != '\0' || v < 0 || v > 11 || *count >= TILES_NOTE_MAP_MAX_SCALE_NOTES) {
            return false;
        }
        out[(*count)++] = (int8_t)v;
    }
    return *count > 0u;
}

/* One SCALES row: "<slot> <name> <intervals> [pack=..] [item=..] version=N",
 * which is exactly SCALE PUT's arguments, so a pulled scale pushes back. */
static void format_scale(const tiles_content_scale_t *s, char *out, size_t cap) {
    char intervals[40];
    format_intervals(s->intervals, s->count, intervals, sizeof(intervals));
    int n = snprintf(out, cap, "%u %s %s", (unsigned)s->slot, s->name, intervals);
    if (n > 0 && (size_t)n < cap && s->pack[0] != '\0') {
        n += snprintf(&out[n], cap - (size_t)n, " pack=%s", s->pack);
    }
    if (n > 0 && (size_t)n < cap && s->item[0] != '\0') {
        n += snprintf(&out[n], cap - (size_t)n, " item=%s", s->item);
    }
    if (n > 0 && (size_t)n < cap) {
        snprintf(&out[n], cap - (size_t)n, " version=%u", (unsigned)s->version);
    }
}

/* Produces the current listing's next row into `text`; false when done. */
static bool listing_row(char *text, size_t cap) {
    switch (s_listing) {
    case LISTING_SETTINGS:
        if (s_listing_next < tiles_settings_count()) {
            const tiles_setting_def_t *def = tiles_settings_at(s_listing_next++);
            char value[48];
            tiles_settings_format(def, def->get(), value, sizeof(value));
            snprintf(text, cap, "%s=%s", def->key, value);
            return true;
        }
        return false;
    case LISTING_SCHEMA:
        if (s_listing_next < tiles_settings_count()) {
            tiles_settings_describe(tiles_settings_at(s_listing_next++), text, cap);
            return true;
        }
        return false;
    case LISTING_SCALES:
        while (s_listing_next < TILES_NOTE_MAP_NUM_CUSTOM_SCALES) {
            tiles_content_scale_t s;
            if (tiles_content_get_scale((uint8_t)(++s_listing_next), &s)) {
                format_scale(&s, text, cap);
                return true;
            }
        }
        return false;
    case LISTING_CONTENT: {
        tiles_content_record_t r;
        if (!tiles_content_record_at(s_listing_next++, &r)) {
            return false;
        }
        if (r.type == TILES_CONTENT_TYPE_SCALE) {
            int n = snprintf(text, cap, "scale %u bytes=%u version=%u", (unsigned)r.slot, (unsigned)r.bytes,
                             (unsigned)r.version);
            if (n > 0 && (size_t)n < cap && r.pack[0] != '\0') {
                n += snprintf(&text[n], cap - (size_t)n, " pack=%s", r.pack);
            }
            if (n > 0 && (size_t)n < cap && r.item[0] != '\0') {
                snprintf(&text[n], cap - (size_t)n, " item=%s", r.item);
            }
        } else {
            snprintf(text, cap, "type%u %u bytes=%u", (unsigned)r.type, (unsigned)r.slot, (unsigned)r.bytes);
        }
        return true;
    }
    case LISTING_CAPTURE: {
        if (s_listing_header) {
            s_listing_header = false;
            snprintf(text, cap, "seq=%lu", (unsigned long)s_listing_next);
            return true;
        }
        /* t,x,y,z,depth,smoothed_depth,bend,flags,state,velocity,aftertouch,
         * strike_ms,peak_depth; %.9g round-trips a float, so a replay gets
         * bend_core's exact input. Live rows round it to fit the USB pipe. */
        const tiles_capture_record_t *r =
            s_listing_next < s_listing_end ? tiles_capture_get((uint32_t)s_listing_next++) : NULL;
        if (r == NULL) {
            return false;
        }
        snprintf(text, cap,
                 s_listing_live ? "%lu,%d,%d,%d,%u,%.1f,%u,%u,%u,%u,%u,%u,%u" : "%lu,%d,%d,%d,%u,%.9g,%u,%u,%u,%u,%u,%u,%u",
                 (unsigned long)r->t_ms, r->x, r->y, r->z, (unsigned)r->depth, (double)r->smoothed_depth,
                 (unsigned)r->bend, (unsigned)r->flags, (unsigned)r->state, (unsigned)r->velocity,
                 (unsigned)r->aftertouch, (unsigned)r->strike_ms, (unsigned)r->peak_depth);
        return true;
    }
    case LISTING_HALL: {
        /* pad,x,y,z,depth,valid,touched,rest_z,rest_confirmed,sensor_ok,
         * depth_gain: every pad now. */
        if (s_listing_next >= TILES_NUM_PADS) {
            return false;
        }
        uint8_t pad = (uint8_t)(++s_listing_next);
        tiles_hall_sample_t hs = tiles_hall_get_sample(pad);
        int16_t rest_z = 0;
        bool rest_ok = false;
        bool sensor_ok = tiles_hall_get_rest(pad, &rest_z, &rest_ok);
        snprintf(text, cap, "%u,%d,%d,%d,%u,%u,%u,%d,%u,%u,%.3f", (unsigned)pad, hs.x, hs.y, hs.z,
                 (unsigned)tiles_hall_get_depth(pad), hs.valid ? 1u : 0u, tiles_touch_is_touched(pad) ? 1u : 0u,
                 rest_z, rest_ok ? 1u : 0u, sensor_ok ? 1u : 0u, (double)tiles_hall_get_depth_gain(pad));
        return true;
    }
    case LISTING_PATHS: {
        float bx;
        float by;
        uint16_t strokes;
        if (s_listing_next >= TILES_NUM_PADS ||
            !tiles_expression_get_press_path((uint8_t)(s_listing_next + 1u), &bx, &by, &strokes)) {
            return false;
        }
        s_listing_next++;
        snprintf(text, cap, "pad=%u bx=%.9g by=%.9g strokes=%u", (unsigned)s_listing_next, (double)bx, (double)by,
                 (unsigned)strokes);
        return true;
    }
    case LISTING_TUNE:
        return tiles_tune_describe((uint16_t)s_listing_next++, text, cap);
    case LISTING_NONE:
    default:
        return false;
    }
}

/* Adds listing rows while there's room for another full line, then "OK"
 * when the listing runs out. */
static void fill_listing(void) {
    char text[USB_VENDOR_LINE_MAX];
    while (s_listing != LISTING_NONE && sizeof(s_out) - s_out_len > USB_VENDOR_LINE_MAX + sizeof(s_tag) + 4u) {
        if (listing_row(text, sizeof(text))) {
            reply(text);
        } else {
            reply_ok();
            s_listing = LISTING_NONE;
        }
    }
}

static void start_listing(listing_t kind) {
    s_listing = kind;
    s_listing_next = 0u;
    fill_listing();
}

static void reply_content_result(tiles_content_result_t r) {
    char text[48];
    switch (r) {
    case TILES_CONTENT_OK:
        reply_ok();
        return;
    case TILES_CONTENT_BAD_SLOT:
        reply_err("bad-slot");
        return;
    case TILES_CONTENT_BAD_INTERVALS:
        reply_err("bad-intervals");
        return;
    case TILES_CONTENT_BAD_TEXT:
        reply_err("bad-name");
        return;
    case TILES_CONTENT_FULL:
        reply_err("content-full");
        return;
    case TILES_CONTENT_NEWER_FORMAT:
        reply_err("newer-format");
        return;
    case TILES_CONTENT_NO_STORAGE:
        reply_err("no-storage");
        return;
    case TILES_CONTENT_SAVE_FAILED:
        snprintf(text, sizeof(text), "save-failed-%s", kv_result_name(tiles_content_get_info().kv.last_result));
        reply_err(text);
        return;
    }
    reply_err("unknown");
}

static uint8_t parse_slot(const char *text) {
    if (text == NULL) {
        return 0u;
    }
    char *end;
    long v = strtol(text, &end, 10);
    return (*text != '\0' && *end == '\0' && v >= 1 && v <= (long)TILES_NOTE_MAP_NUM_CUSTOM_SCALES) ? (uint8_t)v : 0u;
}

/* SCALE PUT <slot> <name> <intervals> [pack=<id>] [item=<id>] [version=<n>]
 * SCALE GET <slot> | SCALE DELETE <slot> */
static void handle_scale(void) {
    char *what = strtok(NULL, " ");
    if (what == NULL) {
        reply_err("missing-key");
        return;
    }
    if (strcmp(what, "GET") == 0) {
        tiles_content_scale_t s;
        uint8_t slot = parse_slot(strtok(NULL, " "));
        if (slot == 0u) {
            reply_err("bad-slot");
        } else if (!tiles_content_get_scale(slot, &s)) {
            reply_err("empty");
        } else {
            char text[USB_VENDOR_LINE_MAX];
            format_scale(&s, text, sizeof(text));
            reply(text);
            reply_ok();
        }
        return;
    }
    if (strcmp(what, "DELETE") == 0) {
        uint8_t slot = parse_slot(strtok(NULL, " "));
        reply_content_result(slot == 0u ? TILES_CONTENT_BAD_SLOT : tiles_content_delete_scale(slot));
        return;
    }
    if (strcmp(what, "PUT") != 0) {
        reply_err("unknown-key");
        return;
    }
    tiles_content_scale_t s;
    memset(&s, 0, sizeof(s));
    s.slot = parse_slot(strtok(NULL, " "));
    char *name = strtok(NULL, " ");
    char *intervals = strtok(NULL, " ");
    if (s.slot == 0u) {
        reply_err("bad-slot");
        return;
    }
    if (name == NULL || strlen(name) > TILES_CONTENT_TEXT_MAX) {
        reply_err("bad-name");
        return;
    }
    strcpy(s.name, name);
    /* Optional fields first (strtok can't be nested: parse_intervals uses it). */
    for (char *field = strtok(NULL, " "); field != NULL; field = strtok(NULL, " ")) {
        char *value = strchr(field, '=');
        if (value == NULL) {
            reply_err("bad-field");
            return;
        }
        *value++ = '\0';
        if ((strcmp(field, "pack") == 0 || strcmp(field, "item") == 0) && strlen(value) <= TILES_CONTENT_TEXT_MAX) {
            strcpy(strcmp(field, "pack") == 0 ? s.pack : s.item, value);
        } else if (strcmp(field, "version") == 0) {
            char *end;
            long v = strtol(value, &end, 10);
            if (*value == '\0' || *end != '\0' || v < 0 || v > 65535) {
                reply_err("bad-field");
                return;
            }
            s.version = (uint16_t)v;
        } else {
            reply_err("bad-field");
            return;
        }
    }
    if (intervals == NULL || !parse_intervals(intervals, s.intervals, &s.count)) {
        reply_err("bad-intervals");
        return;
    }
    reply_content_result(tiles_content_put_scale(&s));
}

/* Takes a leading "@<tag> " off `line` into s_tag (cleared when there's
 * none). False for a malformed tag. */
static bool take_tag(char **line) {
    s_tag[0] = '\0';
    if ((*line)[0] != '@') {
        return true;
    }
    size_t n = strcspn(*line + 1, " ");
    if (n == 0u || n > USB_VENDOR_TAG_MAX) {
        return false;
    }
    for (size_t i = 1u; i <= n; i++) {
        char c = (*line)[i];
        if (!isalnum((unsigned char)c) && c != '_' && c != '-') {
            return false;
        }
    }
    memcpy(s_tag, *line, n + 1u);
    s_tag[n + 1u] = ' ';
    s_tag[n + 2u] = '\0';
    *line += n + 1u;
    return true;
}

/* SAMPLE LIST | BEGIN <slot> <channels> <frames> <crc32 hex> <name>, then
 * the audio as raw bytes (16-bit LE, 48 kHz, stereo interleaved; replies
 * OK <bytes>, then OK or ERR once they are in) | DELETE <slot> |
 * PLAY <slot> [velocity]. Slots 1-8: the first bank's drums. */
static void handle_sample(void) {
    char *what = strtok(NULL, " ");
    char text[USB_VENDOR_LINE_MAX];
    if (what != NULL && strcmp(what, "LIST") == 0) {
        for (uint8_t slot = 0; slot < TILES_SAMPLE_SLOT_COUNT; slot++) {
            tiles_sample_info_t info;
            if (tiles_sample_store_info(slot, &info)) {
                snprintf(text, sizeof(text), "%u %s %s %lu", (unsigned)slot + 1u, info.name,
                         info.channels == 2u ? "stereo" : "mono", (unsigned long)info.frames);
            } else {
                snprintf(text, sizeof(text), "%u empty", (unsigned)slot + 1u);
            }
            reply(text);
        }
        reply_ok();
        return;
    }
    char *slot_arg = strtok(NULL, " ");
    int slot = slot_arg != NULL ? atoi(slot_arg) : 0;
    if (slot < 1 || slot > (int)TILES_SAMPLE_SLOT_COUNT) {
        reply_err("slot-1-8");
        return;
    }
    if (what != NULL && strcmp(what, "PLAY") == 0) {
        char *vel = strtok(NULL, " ");
        int velocity = vel != NULL ? atoi(vel) : 100;
        tiles_usb_audio_drum_hit((uint8_t)(slot - 1), (uint8_t)(velocity < 1 ? 1 : velocity > 127 ? 127 : velocity));
        reply_ok();
        return;
    }
    if (what != NULL && strcmp(what, "DELETE") == 0) {
        tiles_sample_result_t r = tiles_sample_store_delete((uint8_t)(slot - 1));
        r == TILES_SAMPLE_OK ? reply_ok() : reply_err(tiles_sample_result_name(r));
        return;
    }
    if (what == NULL || strcmp(what, "BEGIN") != 0) {
        reply_err("unknown-key");
        return;
    }
    char *channels = strtok(NULL, " ");
    char *frames = strtok(NULL, " ");
    char *crc = strtok(NULL, " ");
    char *name = strtok(NULL, " ");
    if (channels == NULL || frames == NULL || crc == NULL || name == NULL) {
        reply_err("missing-arg");
        return;
    }
    /* Loading erases flash (tens of ms each sector): never while playing. */
    if (tiles_midi_clock_is_running() || !tiles_usb_audio_is_quiet()) {
        reply_err("playing");
        return;
    }
    tiles_sample_info_t info;
    memset(&info, 0, sizeof(info));
    info.channels = (uint8_t)atoi(channels);
    info.frames = (uint32_t)strtoul(frames, NULL, 10);
    info.audio_crc = (uint32_t)strtoul(crc, NULL, 16);
    strncpy(info.name, name, TILES_SAMPLE_NAME_MAX);
    tiles_sample_result_t r = tiles_sample_store_begin((uint8_t)(slot - 1), &info);
    if (r != TILES_SAMPLE_OK) {
        reply_err(tiles_sample_result_name(r));
        return;
    }
    s_raw_left = tiles_sample_store_remaining();
    s_raw_result = TILES_SAMPLE_OK;
    s_raw_last_ms = to_ms_since_boot(get_absolute_time());
    snprintf(text, sizeof(text), "OK %lu", (unsigned long)s_raw_left);
    reply(text);
}

/* The audio after SAMPLE BEGIN: into the store as it arrives, then the
 * load's result. */
static void receive_raw(uint32_t now_ms) {
    uint32_t budget = USB_VENDOR_RAW_PER_PASS;
    uint32_t start_us = time_us_32();
    while (s_raw_left > 0u && budget > 0u && time_us_32() - start_us < USB_VENDOR_RAW_PASS_US) {
        if (!tud_vendor_available()) {
            tiles_usb_task(); /* re-arms the endpoint: the next packet */
            continue;
        }
        uint8_t buf[256];
        uint32_t want = s_raw_left < sizeof(buf) ? s_raw_left : sizeof(buf);
        want = want < budget ? want : budget;
        uint32_t got = tud_vendor_read(buf, want);
        if (got == 0u) {
            break;
        }
        s_raw_last_ms = now_ms;
        s_raw_left -= got;
        budget -= got;
        if (s_raw_result == TILES_SAMPLE_OK) {
            s_raw_result = tiles_sample_store_write(buf, got);
        }
    }
    if (s_raw_left == 0u) {
        if (s_raw_result == TILES_SAMPLE_OK) {
            s_raw_result = tiles_sample_store_finish();
        } else {
            tiles_sample_store_abort();
        }
        s_raw_result == TILES_SAMPLE_OK ? reply_ok() : reply_err(tiles_sample_result_name(s_raw_result));
        pump_out();
    } else if (now_ms - s_raw_last_ms > USB_VENDOR_RAW_TIMEOUT_MS) {
        tiles_sample_store_abort();
        s_raw_left = 0u;
        reply_err("timeout");
        pump_out();
    }
}

static uint32_t bench_cycles(void) {
    return m33_hw->dwt_cyccnt;
}

/* SYNTH FX names, in audio_engine.h's TILES_FX_* order. Values are typed
 * as 0-100, except the bus's predelay (ms) and time (sixteenths). */
static const char *const k_fx_track_names[TILES_FX_TRACK_PARAMS] = {"reverb", "delay", "chorus", "drive",
                                                                     "dark",   "crush", "pump"};
static const char *const k_fx_bus_names[TILES_FX_BUS_PARAMS] = {
    "decay", "damping", "predelay", "mod", "time", "feedback", "tone", "echo-verb", "chorus-rate",
    "chorus-depth", "pump-release"};

static float fx_unit(uint8_t target, uint8_t param) {
    bool literal = target == TILES_FX_TARGET_BUS &&
                   (param == TILES_FX_BUS_PREDELAY_MS || param == TILES_FX_BUS_SIXTEENTHS);
    return literal ? 1.0f : 0.01f;
}

/* SYNTH FX: every track's effects, then the shared ones' settings. SYNTH FX
 * <track 1-4|BUS> <name> <value>: sets one. */
static void handle_synth_fx(void) {
    char text[USB_VENDOR_LINE_MAX];
    char *target_arg = strtok(NULL, " ");
    if (target_arg == NULL) {
        for (uint8_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
            int n = snprintf(text, sizeof(text), "track %u", (unsigned)t + 1u);
            for (uint8_t p = 0; p < TILES_FX_TRACK_PARAMS && n < (int)sizeof(text); p++) {
                n += snprintf(text + n, sizeof(text) - (size_t)n, " %s=%d", k_fx_track_names[p],
                              (int)(tiles_audio_engine_fx(t, p) / fx_unit(t, p) + 0.5f));
            }
            reply(text);
        }
        for (uint8_t p = 0; p < TILES_FX_BUS_PARAMS; p++) {
            float v = tiles_audio_engine_fx(TILES_FX_TARGET_BUS, p) / fx_unit(TILES_FX_TARGET_BUS, p);
            snprintf(text, sizeof(text), "bus %s=%d", k_fx_bus_names[p], (int)(v + 0.5f));
            reply(text);
        }
        reply_ok();
        return;
    }
    char *name = strtok(NULL, " ");
    char *value_arg = strtok(NULL, " ");
    uint8_t target = strcmp(target_arg, "BUS") == 0 ? TILES_FX_TARGET_BUS : (uint8_t)(atoi(target_arg) - 1);
    if (target > TILES_FX_TARGET_BUS || name == NULL || value_arg == NULL) {
        reply_err("fx-<1-4|BUS>-<name>-<value>");
        return;
    }
    const char *const *names = target == TILES_FX_TARGET_BUS ? k_fx_bus_names : k_fx_track_names;
    uint8_t count = target == TILES_FX_TARGET_BUS ? TILES_FX_BUS_PARAMS : TILES_FX_TRACK_PARAMS;
    for (uint8_t p = 0; p < count; p++) {
        if (strcmp(name, names[p]) == 0) {
            float v = (float)atof(value_arg) * fx_unit(target, p);
            tiles_usb_audio_set_fx(target, p, v) ? reply_ok() : reply_err("busy");
            return;
        }
    }
    reply_err("unknown-fx");
}

/* SYNTH LIST (the sounds, then each track's) | PATCH <track 1-4>
 * <number|name> | MIDI <status hex> <data1> <data2>: a message to the
 * engine only (not sent out), to play it from the computer | FX (above) |
 * BENCH: each sound's cycles per sample for one voice. Standalone mode
 * only. */
static void handle_synth(void) {
    char *what = strtok(NULL, " ");
    char text[USB_VENDOR_LINE_MAX];
    if (!tiles_usb_audio_started()) {
        reply_err("standalone-only");
        return;
    }
    if (what != NULL && strcmp(what, "LIST") == 0) {
        for (uint8_t p = 0; p < tiles_audio_engine_patch_count(); p++) {
            snprintf(text, sizeof(text), "%u %s", (unsigned)p, tiles_audio_engine_patch_name(p));
            reply(text);
        }
        for (uint8_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
            snprintf(text, sizeof(text), "track %u %s", (unsigned)t + 1u,
                     tiles_audio_engine_patch_name(tiles_audio_engine_patch(t)));
            reply(text);
        }
        reply_ok();
        return;
    }
    if (what != NULL && strcmp(what, "PATCH") == 0) {
        char *track_arg = strtok(NULL, " ");
        char *patch_arg = strtok(NULL, " ");
        int track = track_arg != NULL ? atoi(track_arg) : 0;
        if (track < 1 || track > (int)TILES_AUDIO_TRACKS || patch_arg == NULL) {
            reply_err("track-1-4");
            return;
        }
        int patch = -1;
        for (uint8_t p = 0; p < tiles_audio_engine_patch_count(); p++) {
            if (strcmp(patch_arg, tiles_audio_engine_patch_name(p)) == 0) {
                patch = p;
            }
        }
        if (patch < 0 && patch_arg[0] >= '0' && patch_arg[0] <= '9') {
            patch = atoi(patch_arg);
        }
        if (patch < 0 || patch >= (int)tiles_audio_engine_patch_count()) {
            reply_err("unknown-patch");
            return;
        }
        tiles_usb_audio_set_patch((uint8_t)(track - 1), (uint8_t)patch) ? reply_ok() : reply_err("busy");
        return;
    }
    if (what != NULL && strcmp(what, "FX") == 0) {
        handle_synth_fx();
        return;
    }
    if (what != NULL && strcmp(what, "BENCH") == 0) {
        /* On core 0 with its interrupts off, ~5 ms a sound. */
        m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
        m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
        for (uint8_t p = 0; p < tiles_audio_engine_patch_count(); p++) {
            uint32_t irq = save_and_disable_interrupts();
            uint32_t c = tiles_audio_engine_bench(p, 2400u, bench_cycles);
            restore_interrupts(irq);
            snprintf(text, sizeof(text), "%s %lu", tiles_audio_engine_patch_name(p), (unsigned long)c);
            reply(text);
        }
        /* The effects share core 1's state: only while it isn't rendering. */
        static const char *const k_fx_bench_names[TILES_FX_BENCH_COUNT] = {"chorus", "delay", "reverb",
                                                                          "drive",  "crush", "pump"};
        for (uint32_t f = 0; f < TILES_FX_BENCH_COUNT && !tiles_usb_audio_get_info().streaming; f++) {
            uint32_t irq = save_and_disable_interrupts();
            uint32_t c = tiles_fx_bench(f, 2400u, bench_cycles);
            restore_interrupts(irq);
            snprintf(text, sizeof(text), "fx-%s %lu", k_fx_bench_names[f], (unsigned long)c);
            reply(text);
        }
        reply_ok();
        return;
    }
    if (what != NULL && strcmp(what, "MIDI") == 0) {
        char *st = strtok(NULL, " ");
        char *d1 = strtok(NULL, " ");
        char *d2 = strtok(NULL, " ");
        unsigned long status = st != NULL ? strtoul(st, NULL, 16) : 0u;
        if (status < 0x80u || status > 0xEFu || d1 == NULL) {
            reply_err("status-80-ef");
            return;
        }
        tiles_usb_audio_midi((uint8_t)status, (uint8_t)(atoi(d1) & 0x7F), (uint8_t)(d2 != NULL ? atoi(d2) & 0x7F : 0));
        reply_ok();
        return;
    }
    reply_err("unknown-key");
}

static void handle_line(char *line) {
    if (!take_tag(&line)) {
        reply_err("bad-tag");
        return;
    }
    char *cmd = strtok(line, " ");
    if (cmd == NULL) {
        if (s_tag[0] != '\0') {
            reply_err("missing-command"); /* a tagged request always gets an answer */
        }
        return; /* blank line: no reply, like a terminal */
    }
    char text[USB_VENDOR_LINE_MAX];

    if (strcmp(cmd, "LIST") == 0) {
        start_listing(LISTING_SETTINGS);
        return;
    }

    /* One line per setting (id, key, type, range/values, default): enough for
     * a UI to build a control without a hard-coded list. */
    if (strcmp(cmd, "SCHEMA") == 0) {
        start_listing(LISTING_SCHEMA);
        return;
    }

    /* Custom scales (profiles/content.h): SCALES lists the filled slots, one
     * SCALE PUT-shaped line each; SCALE GET/PUT/DELETE edit one slot. */
    if (strcmp(cmd, "SCALES") == 0) {
        start_listing(LISTING_SCALES);
        return;
    }
    if (strcmp(cmd, "SYNTH") == 0) {
        handle_synth();
        return;
    }
    if (strcmp(cmd, "SAMPLE") == 0) {
        handle_sample();
        return;
    }
    /* Sensor capture (diagnostics/capture.h): START <pad> records one pad
     * (~8 s), WATCH <pad> keeps recording (the studio), STOP ends it,
     * RELEASE gives its memory back to the effects (quiet until then),
     * STATUS reports, READ streams every record held, READ <from> the live
     * rows from that sequence number on. */
    if (strcmp(cmd, "CAPTURE") == 0) {
        char *what = strtok(NULL, " ");
        char *arg = strtok(NULL, " ");
        bool watch = what != NULL && strcmp(what, "WATCH") == 0;
        if (what != NULL && (watch || strcmp(what, "START") == 0) && arg != NULL) {
            int pad = atoi(arg);
            if (pad < 1 || pad > 24) {
                reply_err("pad-1-24");
                return;
            }
            s_capture_setup.mode = tiles_expression_get_pitch_bend_mode();
            s_capture_setup.sensitivity = tiles_expression_get_pitch_bend_sensitivity();
            tiles_expression_get_press_path((uint8_t)pad, &s_capture_setup.bx, &s_capture_setup.by,
                                            &s_capture_setup.strokes);
            bool started = watch ? tiles_capture_watch((uint8_t)pad) : tiles_capture_start((uint8_t)pad);
            started ? reply_ok() : reply_err("memory-busy");
        } else if (what != NULL && strcmp(what, "STOP") == 0) {
            tiles_capture_stop();
            reply_ok();
        } else if (what != NULL && strcmp(what, "RELEASE") == 0) {
            tiles_capture_release();
            reply_ok();
        } else if (what != NULL && strcmp(what, "STATUS") == 0) {
            snprintf(text, sizeof(text), "pad=%u running=%u count=%u max=%u watching=%u total=%lu",
                     (unsigned)tiles_capture_pad(), tiles_capture_running() ? 1u : 0u,
                     (unsigned)tiles_capture_count(), (unsigned)TILES_CAPTURE_MAX,
                     tiles_capture_watching() ? 1u : 0u, (unsigned long)tiles_capture_total());
            reply(text);
            snprintf(text, sizeof(text), "bend mode=%s sensitivity=%.9g bx=%.9g by=%.9g strokes=%u",
                     TILES_BEND_MODE_NAMES[s_capture_setup.mode % TILES_BEND_MODE_COUNT],
                     (double)s_capture_setup.sensitivity, (double)s_capture_setup.bx, (double)s_capture_setup.by,
                     (unsigned)s_capture_setup.strokes);
            reply(text);
            reply_ok();
        } else if (what != NULL && strcmp(what, "READ") == 0) {
            uint32_t total = tiles_capture_total();
            uint32_t oldest = total > TILES_CAPTURE_MAX ? total - TILES_CAPTURE_MAX : 0u;
            uint32_t from = oldest;
            s_listing_live = arg != NULL;
            if (s_listing_live) {
                from = (uint32_t)strtoul(arg, NULL, 10);
                from = from < oldest ? oldest : (from > total ? total : from);
            }
            s_listing_end = total;
            if (s_listing_live && total - from > CAPTURE_READ_MAX_ROWS) {
                s_listing_end = from + CAPTURE_READ_MAX_ROWS;
            }
            s_listing_header = s_listing_live;
            s_listing = LISTING_CAPTURE;
            s_listing_next = from;
            fill_listing();
        } else {
            reply_err("capture-start|watch|stop|release|status|read");
        }
        return;
    }
    /* Every pad's Hall reading now, one row each (the studio's grid). */
    if (strcmp(cmd, "HALL") == 0) {
        start_listing(LISTING_HALL);
        return;
    }
    /* Live tuning (services/tune.h), lost at power-off: TUNE lists every
     * key, TUNE SET <key> <value>, TUNE RESET [<key>]. */
    if (strcmp(cmd, "TUNE") == 0) {
        char *what = strtok(NULL, " ");
        char *key = strtok(NULL, " ");
        if (what == NULL || strcmp(what, "LIST") == 0) {
            start_listing(LISTING_TUNE);
        } else if (strcmp(what, "SET") == 0) {
            switch (tiles_tune_set(key, strtok(NULL, " "))) {
            case TILES_TUNE_OK:
                reply_ok();
                break;
            case TILES_TUNE_UNKNOWN_KEY:
                reply_err("unknown-key");
                break;
            case TILES_TUNE_OUT_OF_RANGE:
                reply_err("out-of-range");
                break;
            case TILES_TUNE_CONFLICT:
                reply_err("conflict");
                break;
            case TILES_TUNE_BAD_VALUE:
            default:
                reply_err("bad-value");
                break;
            }
        } else if (strcmp(what, "RESET") == 0) {
            if (tiles_tune_reset(key)) {
                reply_ok();
            } else {
                reply_err("unknown-key");
            }
        } else {
            reply_err("tune-list|set|reset");
        }
        return;
    }
    /* Learned press paths (services/bend_core.h): PATHS lists them, RESET
     * forgets them all. */
    if (strcmp(cmd, "BEND") == 0) {
        char *what = strtok(NULL, " ");
        if (what != NULL && strcmp(what, "PATHS") == 0) {
            start_listing(LISTING_PATHS);
        } else if (what != NULL && strcmp(what, "RESET") == 0) {
            tiles_expression_reset_press_paths();
            reply_ok();
        } else {
            reply_err("bend-paths|reset");
        }
        return;
    }
    if (strcmp(cmd, "SCALE") == 0) {
        handle_scale();
        return;
    }

    /* CONTENT LIST: every record in the content store (packs show here);
     * CONTENT CLEAR: wipe it (all custom scales, everything). */
    if (strcmp(cmd, "CONTENT") == 0) {
        char *what = strtok(NULL, " ");
        if (what != NULL && strcmp(what, "LIST") == 0) {
            start_listing(LISTING_CONTENT);
        } else if (what != NULL && strcmp(what, "CLEAR") == 0) {
            reply_content_result(tiles_content_clear());
        } else {
            reply_err(what == NULL ? "missing-key" : "unknown-key");
        }
        return;
    }

    /* Unit, firmware version, then flash-store status (for "did it save?"). */
#if TILES_DSP_BENCH
    /* BENCH START runs the DSP stress test on core 1; BENCH reports it
     * (one line per kernel: cycles per sample per voice), or "running".
     * BENCH SOAK <ms> <cached 0|1> streams the sample slots from flash. */
    if (strcmp(cmd, "BENCH") == 0) {
        char *what = strtok(NULL, " ");
        if (what != NULL && strcmp(what, "START") == 0) {
            tiles_usb_audio_bench_start();
            reply_ok();
            return;
        }
        if (what != NULL && strcmp(what, "SOAK") == 0) {
            char *ms = strtok(NULL, " ");
            char *cached = strtok(NULL, " ");
            if (ms == NULL || cached == NULL) {
                reply_err("missing-arg");
                return;
            }
            uint32_t soak_ms = (uint32_t)strtoul(ms, NULL, 10);
            tiles_usb_audio_bench_soak(soak_ms > 60000u ? 60000u : soak_ms, cached[0] == '1');
            reply_ok();
            return;
        }
        const tiles_dsp_bench_t *b = tiles_usb_audio_bench_result();
        if (b == NULL) {
            reply("running");
        } else {
            snprintf(text, sizeof(text), "clk_mhz=%lu", (unsigned long)b->clk_mhz);
            reply(text);
            for (uint32_t i = 0; i < b->count; i++) {
                snprintf(text, sizeof(text), "%s units=%lu cycles=%lu.%03lu peak=%ld", b->item[i].name,
                         (unsigned long)b->item[i].units, (unsigned long)(b->item[i].milli_cycles / 1000u),
                         (unsigned long)(b->item[i].milli_cycles % 1000u), (long)b->item[i].peak_milli);
                reply(text);
            }
        }
        reply_ok();
        return;
    }
#endif
    if (strcmp(cmd, "INFO") == 0) {
        tiles_settings_persist_info_t info = tiles_settings_persist_get_info();
        snprintf(text, sizeof(text), "unit=%u/%u", (unsigned)TILES_UNIT_NUMBER, (unsigned)TILES_UNIT_COUNT);
        reply(text);
        if (TILES_FW_POINT == 0u) {
            snprintf(text, sizeof(text), "firmware=%s.%u", TILES_FW_STAGE_NAME, (unsigned)TILES_FW_BUILD);
        } else {
            snprintf(text, sizeof(text), "firmware=%s.%u.%u", TILES_FW_STAGE_NAME, (unsigned)TILES_FW_BUILD,
                     (unsigned)TILES_FW_POINT);
        }
        reply(text);
        snprintf(text, sizeof(text), "settings=%u", (unsigned)tiles_settings_count());
        reply(text);
        tiles_power_state_t power = tiles_power_get_state();
        static const char *const k_power_names[] = {"usb_only", "external_only", "usb_and_external", "fault"};
        snprintf(text, sizeof(text), "power=%s", (unsigned)power.mode < 4u ? k_power_names[power.mode] : "unknown");
        reply(text);
        snprintf(text, sizeof(text), "power.budget_ma=%lu", (unsigned long)power.main_5v_budget_ma);
        reply(text);
        snprintf(text, sizeof(text), "power.led_ceiling_percent=%u", (unsigned)power.led_brightness_ceiling_percent);
        reply(text);
        snprintf(text, sizeof(text), "power.haptic_voices=%u", (unsigned)power.max_haptic_voices);
        reply(text);
        snprintf(text, sizeof(text), "store.loaded=%d", info.loaded ? 1 : 0);
        reply(text);
        snprintf(text, sizeof(text), "store.restored=%u", (unsigned)info.applied);
        reply(text);
        snprintf(text, sizeof(text), "store.slot=%d", (int)info.kv.newest);
        reply(text);
        snprintf(text, sizeof(text), "store.seq=%lu", (unsigned long)(info.kv.newest < 0 ? 0u : info.kv.seq[info.kv.newest]));
        reply(text);
        snprintf(text, sizeof(text), "store.saved_bytes=%u", (unsigned)info.saved_bytes);
        reply(text);
        snprintf(text, sizeof(text), "store.writes=%lu", (unsigned long)info.kv.writes);
        reply(text);
        snprintf(text, sizeof(text), "store.last_result=%s", kv_result_name(info.kv.last_result));
        reply(text);
        snprintf(text, sizeof(text), "store.pending=%d", info.pending ? 1 : 0);
        reply(text);
        snprintf(text, sizeof(text), "store.failures=%lu", (unsigned long)info.save_failures);
        reply(text);
        tiles_content_info_t content = tiles_content_get_info();
        snprintf(text, sizeof(text), "content.storage=%s",
                 !content.storage ? "none" : (content.newer_format ? "newer-format" : "ok"));
        reply(text);
        snprintf(text, sizeof(text), "content.scales=%u/%u", (unsigned)content.scales,
                 (unsigned)TILES_NOTE_MAP_NUM_CUSTOM_SCALES);
        reply(text);
        snprintf(text, sizeof(text), "content.records=%u", (unsigned)content.records);
        reply(text);
        snprintf(text, sizeof(text), "content.bytes=%u/%u", (unsigned)content.used_bytes,
                 (unsigned)content.capacity_bytes);
        reply(text);
        snprintf(text, sizeof(text), "content.writes=%lu", (unsigned long)content.kv.writes);
        reply(text);
        snprintf(text, sizeof(text), "content.last_result=%s", kv_result_name(content.kv.last_result));
        reply(text);
        tiles_usb_audio_info_t audio = tiles_usb_audio_get_info();
        snprintf(text, sizeof(text), "device.mode=%s", tiles_device_mode_is_standalone() ? "standalone" : "controller");
        reply(text);
        snprintf(text, sizeof(text), "audio.streaming=%d", audio.streaming ? 1 : 0);
        reply(text);
        snprintf(text, sizeof(text), "audio.packets=%lu", (unsigned long)audio.packets);
        reply(text);
        snprintf(text, sizeof(text), "audio.late_frames=%lu", (unsigned long)audio.late_frames);
        reply(text);
        snprintf(text, sizeof(text), "audio.underruns=%lu", (unsigned long)audio.underruns);
        reply(text);
        snprintf(text, sizeof(text), "audio.usb_gap_us=%lu", (unsigned long)audio.usb_gap_us);
        reply(text);
        snprintf(text, sizeof(text), "audio.fx=%s", audio.fx_lent ? "lent" : "on");
        reply(text);
        snprintf(text, sizeof(text), "audio.fx_cost=%lu", (unsigned long)audio.fx_cost);
        reply(text);
        snprintf(text, sizeof(text), "audio.midi_drops=%lu", (unsigned long)audio.midi_drops);
        reply(text);
        snprintf(text, sizeof(text), "audio.voices=%u", (unsigned)audio.voices);
        reply(text);
        snprintf(text, sizeof(text), "audio.load_pct=%u", (unsigned)audio.load_pct);
        reply(text);
        snprintf(text, sizeof(text), "audio.load_peak_pct=%u", (unsigned)audio.load_peak_pct);
        reply(text);
        snprintf(text, sizeof(text), "audio.stack_free=%lu", (unsigned long)audio.stack_free);
        reply(text);
        snprintf(text, sizeof(text), "audio.shed=%lu", (unsigned long)audio.shed);
        reply(text);
        snprintf(text, sizeof(text), "audio.drums=%u", (unsigned)audio.drum_voices);
        reply(text);
        snprintf(text, sizeof(text), "audio.drum_underruns=%lu", (unsigned long)audio.drum_underruns);
        reply(text);
        snprintf(text, sizeof(text), "audio.cost=%u,%u,%u,%u", (unsigned)audio.cost[0], (unsigned)audio.cost[1],
                 (unsigned)audio.cost[2], (unsigned)audio.cost[3]);
        reply(text);
        if (tiles_usb_audio_started()) {
            snprintf(text, sizeof(text), "audio.patch=%s,%s,%s,%s",
                     tiles_audio_engine_patch_name(tiles_audio_engine_patch(0)),
                     tiles_audio_engine_patch_name(tiles_audio_engine_patch(1)),
                     tiles_audio_engine_patch_name(tiles_audio_engine_patch(2)),
                     tiles_audio_engine_patch_name(tiles_audio_engine_patch(3)));
            reply(text);
        }
        tiles_buttons_pca_health_t pca = tiles_buttons_pca_health();
        snprintf(text, sizeof(text), "pca.mode1=%02x,%02x", (unsigned)pca.mode1[0], (unsigned)pca.mode1[1]);
        reply(text);
        snprintf(text, sizeof(text), "pca.resets=%lu", (unsigned long)pca.resets);
        reply(text);
        snprintf(text, sizeof(text), "pca.read_fails=%lu", (unsigned long)pca.read_fails);
        reply(text);
        snprintf(text, sizeof(text), "flash.lockout_fails=%lu", (unsigned long)tiles_flash_lockout_fails());
        reply(text);
        tiles_loop_stats_t loop = tiles_loop_stats_get();
        snprintf(text, sizeof(text), "loop.avg_us=%lu", (unsigned long)loop.avg_us);
        reply(text);
        snprintf(text, sizeof(text), "loop.max_us=%lu", (unsigned long)loop.max_us);
        reply(text);
        snprintf(text, sizeof(text), "loop.max_ever_us=%lu", (unsigned long)loop.max_ever_us);
        reply(text);
        tiles_hall_rest_info_t rest = tiles_hall_get_rest_info();
        snprintf(text, sizeof(text), "hall.rezeros=%lu", (unsigned long)rest.rezeros);
        reply(text);
        snprintf(text, sizeof(text), "hall.unconfirmed=%u", (unsigned)rest.unconfirmed);
        reply(text);
        snprintf(text, sizeof(text), "hall.reinits=%lu", (unsigned long)rest.reinits);
        reply(text);
        snprintf(text, sizeof(text), "hall.dead=%u", (unsigned)rest.dead);
        reply(text);
        tiles_touch_info_t touch = tiles_touch_get_info();
        snprintf(text, sizeof(text), "touch.recoveries=%lu", (unsigned long)touch.recoveries);
        reply(text);
        snprintf(text, sizeof(text), "tune.changed=%u", (unsigned)tiles_tune_changed());
        reply(text);
        snprintf(text, sizeof(text), "boot.ms=%lu", (unsigned long)loop.boot_ms);
        reply(text);
        tiles_fault_t fault;
        if (tiles_fault_last(&fault)) {
            snprintf(text, sizeof(text), "fault=core%lu pc=%08lx lr=%08lx cfsr=%08lx hfsr=%08lx bfar=%08lx sp=%08lx",
                     (unsigned long)fault.core, (unsigned long)fault.pc, (unsigned long)fault.lr,
                     (unsigned long)fault.cfsr, (unsigned long)fault.hfsr, (unsigned long)fault.bfar,
                     (unsigned long)fault.sp);
            reply(text);
            snprintf(text, sizeof(text), "fault.callers=%08lx,%08lx,%08lx,%08lx", (unsigned long)fault.callers[0],
                     (unsigned long)fault.callers[1], (unsigned long)fault.callers[2],
                     (unsigned long)fault.callers[3]);
            reply(text);
            snprintf(text, sizeof(text), "fault.message=%s", fault.message);
        } else {
            snprintf(text, sizeof(text), "fault=none");
        }
        reply(text);
        tiles_fault_saved_t saved;
        if (tiles_fault_saved(&saved)) {
            snprintf(text, sizeof(text), "fault.saved=%s image=%lu count=%lu", saved.firmware,
                     (unsigned long)saved.image_bytes, (unsigned long)saved.count);
            reply(text);
            snprintf(text, sizeof(text),
                     "fault.saved_at=core%lu pc=%08lx lr=%08lx cfsr=%08lx hfsr=%08lx bfar=%08lx sp=%08lx",
                     (unsigned long)saved.fault.core, (unsigned long)saved.fault.pc, (unsigned long)saved.fault.lr,
                     (unsigned long)saved.fault.cfsr, (unsigned long)saved.fault.hfsr,
                     (unsigned long)saved.fault.bfar, (unsigned long)saved.fault.sp);
            reply(text);
            snprintf(text, sizeof(text), "fault.saved_callers=%08lx,%08lx,%08lx,%08lx",
                     (unsigned long)saved.fault.callers[0], (unsigned long)saved.fault.callers[1],
                     (unsigned long)saved.fault.callers[2], (unsigned long)saved.fault.callers[3]);
            reply(text);
            snprintf(text, sizeof(text), "fault.saved_message=%s", saved.fault.message);
        } else {
            snprintf(text, sizeof(text), "fault.saved=none");
        }
        reply(text);
        snprintf(text, sizeof(text), "boot.reset=%s (0x%08lx)", tiles_fault_reset_cause(),
                 (unsigned long)tiles_fault_reset_bits());
        reply(text);
        snprintf(text, sizeof(text), "uptime_s=%lu", (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000u));
        reply(text);
        snprintf(text, sizeof(text), "boot.hang=%s",
                 tiles_boot_stage_last_hang() != NULL ? tiles_boot_stage_last_hang() : "none");
        reply(text);
        snprintf(text, sizeof(text), "boot.sensors_ms=%lu", (unsigned long)rest.boot_ms);
        reply(text);
        tiles_op_mode_seq_store_info_t seq = tiles_op_mode_get_seq_store_info();
        snprintf(text, sizeof(text), "seq.storage=%s", seq.storage ? "ok" : "none");
        reply(text);
        snprintf(text, sizeof(text), "seq.saved_bytes=%u", (unsigned)seq.saved_bytes);
        reply(text);
        snprintf(text, sizeof(text), "seq.saves=%lu", (unsigned long)seq.saves);
        reply(text);
        snprintf(text, sizeof(text), "seq.pending=%d", seq.pending ? 1 : 0);
        reply(text);
        snprintf(text, sizeof(text), "seq.truncated=%d", seq.truncated ? 1 : 0);
        reply(text);
        tiles_drum_seq_store_info_t drums = tiles_drum_seq_get_store_info();
        snprintf(text, sizeof(text), "drums.storage=%s", drums.storage ? "ok" : "none");
        reply(text);
        snprintf(text, sizeof(text), "drums.saved_bytes=%u", (unsigned)drums.saved_bytes);
        reply(text);
        snprintf(text, sizeof(text), "drums.saves=%lu", (unsigned long)drums.saves);
        reply(text);
        snprintf(text, sizeof(text), "drums.pending=%d", drums.pending ? 1 : 0);
        reply(text);
        tiles_play_state_info_t play = tiles_play_state_get_info();
        snprintf(text, sizeof(text), "state.storage=%s", play.storage ? "ok" : "none");
        reply(text);
        if (play.have_saved) {
            snprintf(text, sizeof(text), "state.saved=mode:%u scale:%u octave:%d key:%d", (unsigned)play.mode,
                     (unsigned)play.scale, (int)play.octave, (int)play.key);
            reply(text);
        }
        snprintf(text, sizeof(text), "state.saves=%lu", (unsigned long)play.saves);
        reply(text);
        snprintf(text, sizeof(text), "state.pending=%d", play.pending ? 1 : 0);
        reply(text);
        reply_ok();
        return;
    }

    /* TOUCH: every pad's electrode: touched, filtered reading, baseline and
     * how far below the baseline it is (a touch needs more than 12). */
    if (strcmp(cmd, "TOUCH") == 0) {
        for (uint8_t pad = 1u; pad <= 24u; pad++) {
            uint16_t filtered = 0, baseline = 0;
            bool ok = tiles_touch_read_electrode(pad, &filtered, &baseline);
            snprintf(text, sizeof(text), "pad=%u touched=%d filtered=%u baseline=%u delta=%d%s", (unsigned)pad,
                     tiles_touch_is_touched(pad) ? 1 : 0, (unsigned)filtered, (unsigned)baseline,
                     (int)baseline - (int)filtered, ok ? "" : " read=fail");
            reply(text);
        }
        reply_ok();
        return;
    }

    /* HAPTICS: per-pad counts (haptics.h) and the global state; HAPTICS
     * RESET clears the counts. */
    if (strcmp(cmd, "HAPTICS") == 0) {
        char *what = strtok(NULL, " ");
        if (what != NULL && strcmp(what, "RESET") == 0) {
            tiles_haptics_reset_stats();
            reply_ok();
            return;
        }
        snprintf(text, sizeof(text), "intensity=%.2f silenced=%d voices=%u", (double)tiles_haptics_get_intensity(),
                 tiles_haptics_is_silenced() ? 1 : 0, (unsigned)tiles_power_get_state().max_haptic_voices);
        reply(text);
        for (uint8_t pad = 1u; pad <= 24u; pad++) {
            tiles_haptics_pad_stats_t st = tiles_haptics_pad_stats(pad);
            snprintf(text, sizeof(text), "pad=%u asked=%u started=%u pulses=%u stops=%u cancelled=%u stolen=%u dropped=%u phase=%u",
                     (unsigned)pad, (unsigned)st.asked, (unsigned)st.started, (unsigned)st.pulses, (unsigned)st.stops,
                     (unsigned)st.cancelled, (unsigned)st.stolen, (unsigned)st.dropped, (unsigned)st.phase);
            reply(text);
        }
        reply_ok();
        return;
    }

    /* Bench tests for measuring current (tools/README.md "Current tests"):
     *   TEST LEDS <0-100>         every LED white at that % of full scale
     *   TEST MOTORS <n> [duty%]   pads 1..n's motors (capped at the power
     *                             mode's voices) for 8 s; duty default 100
     *   TEST MOTOR <pad> [duty%]  one pad's motor for 1 s
     *   TEST OFF                  both off, normal rendering back */
    if (strcmp(cmd, "TEST") == 0) {
        char *what = strtok(NULL, " ");
        char *arg = strtok(NULL, " ");
        char *arg2 = strtok(NULL, " ");
        if (what != NULL && strcmp(what, "LEDS") == 0 && arg != NULL) {
            int percent = atoi(arg);
            if (percent < 0 || percent > 100) {
                reply_err("percent-0-100");
                return;
            }
            tiles_lighting_set_test_white((uint8_t)percent);
            reply_ok();
            return;
        }
        if (what != NULL && strcmp(what, "MOTORS") == 0 && arg != NULL) {
            int count = atoi(arg);
            int duty = arg2 != NULL ? atoi(arg2) : 100;
            if (count < 0 || count > 24 || duty < 0 || duty > 100) {
                reply_err("motors-0-24-duty-0-100");
                return;
            }
            uint8_t started = tiles_haptics_test_motors((uint8_t)count, (float)duty / 100.0f);
            snprintf(text, sizeof(text), "motors=%u", (unsigned)started);
            reply(text);
            reply_ok();
            return;
        }
        if (what != NULL && strcmp(what, "MOTOR") == 0 && arg != NULL) {
            int pad = atoi(arg);
            int duty = arg2 != NULL ? atoi(arg2) : 100;
            if (pad < 1 || pad > 24 || duty < 0 || duty > 100) {
                reply_err("pad-1-24-duty-0-100");
                return;
            }
            tiles_haptics_test_motor((uint8_t)pad, (float)duty / 100.0f);
            reply_ok();
            return;
        }
        if (what != NULL && strcmp(what, "OFF") == 0) {
            tiles_haptics_test_stop();
            tiles_lighting_set_test_white(0u);
            reply_ok();
            return;
        }
        if (what != NULL && strcmp(what, "MODE") == 0 && arg != NULL) {
            /* Switch mode without the pads (loop-timing checks per mode). */
            if (tiles_op_mode_test_set_mode(arg)) {
                reply_ok();
            } else {
                reply_err("mode-melodic|duet|sequencer|bass|song|ableton|drums|volca|prism|bounce|ripples|sand|orbits|cradle");
            }
            return;
        }
        reply_err("usage-TEST-LEDS-n|MOTORS-n-[duty]|MODE-name|OFF");
        return;
    }

    /* Saves any pending change now instead of waiting for the debounced,
     * pads-idle save. The write pauses the firmware for tens of ms. */
    /* FAULT CLEAR: forgets the saved crash (INFO fault.saved). */
    if (strcmp(cmd, "FAULT") == 0) {
        char *what = strtok(NULL, " ");
        if (what == NULL || strcmp(what, "CLEAR") != 0) {
            reply_err("unknown-key");
        } else if (tiles_fault_clear()) {
            reply_ok();
        } else {
            reply_err("flash");
        }
        return;
    }

    if (strcmp(cmd, "SAVE") == 0) {
        tiles_kv_result_t r = tiles_settings_persist_save_now();
        if (r == TILES_KV_OK) {
            reply_ok();
        } else {
            snprintf(text, sizeof(text), "save-failed-%s", kv_result_name(r));
            reply_err(text);
        }
        return;
    }

    /* REBOOT BOOTSEL | APP: flush "OK" first. BOOTSEL enters the ROM
     * bootloader; APP warm-restarts as from power-on. Never return to the main
     * loop (it feeds the watchdog, which kept the reboot from firing). */
    if (strcmp(cmd, "REBOOT") == 0) {
        char *what = strtok(NULL, " ");
        bool bootsel = what != NULL && strcmp(what, "BOOTSEL") == 0;
        if (bootsel || (what != NULL && strcmp(what, "APP") == 0)) {
            reply_ok();
            uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + 50u;
            while (to_ms_since_boot(get_absolute_time()) < deadline_ms) {
                tiles_usb_task();
                pump_out();
            }
            if (bootsel) {
                reset_usb_boot(0, 0);
            }
            watchdog_reboot(0, 0, 1u);
            while (true) {
                tight_loop_contents(); /* not fed: resets within 1 ms */
            }
        } else {
            reply_err(what == NULL ? "missing-key" : "unknown-key");
        }
        return;
    }

    char *key = strtok(NULL, " ");
    if (key == NULL) {
        reply_err("missing-key");
        return;
    }

    if (strcmp(cmd, "GET") == 0) {
        if (tiles_settings_get_text(key, text, sizeof(text))) {
            reply(text);
        } else {
            reply_err("unknown-key");
        }
        return;
    }

    if (strcmp(cmd, "SET") == 0) {
        char *value = strtok(NULL, " ");
        switch (tiles_settings_set_text(key, value)) {
        case TILES_SETTINGS_OK:
            reply_ok();
            break;
        case TILES_SETTINGS_UNKNOWN_KEY:
            reply_err("unknown-key");
            break;
        case TILES_SETTINGS_OUT_OF_RANGE:
            reply_err("out-of-range");
            break;
        case TILES_SETTINGS_BAD_VALUE:
        default:
            reply_err("bad-value");
            break;
        }
        return;
    }

    /* RESET <key> restores one default; RESET ALL restores all. Applied at
     * once and saved like SET. */
    if (strcmp(cmd, "RESET") == 0) {
        if (tiles_settings_reset(strcmp(key, "ALL") == 0 ? NULL : key)) {
            reply_ok();
        } else {
            reply_err("unknown-key");
        }
        return;
    }

    reply_err("unknown-command");
}

void tiles_usb_vendor_init(void) {
    s_listing = LISTING_NONE;
    s_tag[0] = '\0';
    s_rx_len = 0u;
    s_out_len = 0u;
    s_out_sent = 0u;
}

void tiles_usb_vendor_scan(void) {
    if (!tud_vendor_mounted()) {
        s_out_len = 0u; /* host gone: drop the reply rather than hand it to the next host */
        s_out_sent = 0u;
        s_listing = LISTING_NONE;
        s_rx_len = 0u;
        if (s_raw_left > 0u) {
            tiles_sample_store_abort();
            s_raw_left = 0u;
        }
        return;
    }
    pump_out();
    if (s_out_len == 0u && s_listing != LISTING_NONE) {
        fill_listing(); /* buffer drained: the listing's next rows */
        pump_out();
    }
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (s_out_len > 0u || s_listing != LISTING_NONE) {
        s_rx_idle_since_ms = now_ms; /* not reading: no partial-line timeout */
        if (s_raw_left > 0u) {
            s_raw_last_ms = now_ms;
        }
        return; /* still sending the previous reply: wait before the next command */
    }
    if (s_raw_left > 0u) {
        receive_raw(now_ms);
        return;
    }
    while (tud_vendor_available()) {
        uint8_t byte;
        if (tud_vendor_read(&byte, 1) == 0) {
            break;
        }
        s_rx_idle_since_ms = now_ms;
        if (byte == '\n' || byte == '\r') {
            if (s_rx_len > 0u) {
                s_rx_line[s_rx_len] = '\0';
                handle_line(s_rx_line);
                s_rx_len = 0u;
                pump_out();
                return; /* one command per scan; the rest waits in TinyUSB's FIFO */
            }
            continue;
        }
        if (s_rx_len < USB_VENDOR_LINE_MAX - 1u) {
            s_rx_line[s_rx_len++] = (char)byte;
        }
        /* Line longer than USB_VENDOR_LINE_MAX: drop bytes until the next
         * newline. No real command comes close, so it is malformed anyway. */
    }
    if (s_rx_len > 0u && now_ms - s_rx_idle_since_ms > USB_VENDOR_PARTIAL_LINE_MS) {
        s_rx_len = 0u;
    }
}
