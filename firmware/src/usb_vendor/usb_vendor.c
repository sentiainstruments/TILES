#include "usb_vendor.h"

#include "board/unit_id.h"
#include "content.h"
#include "drum_seq.h"
#include "haptics.h"
#include "lighting.h"
#include "note_map.h"
#include "power.h"
#include "product_identity.h"
#include "settings.h"
#include "settings_persist.h"

#include "tusb.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"

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

static char s_out[USB_VENDOR_OUT_MAX];
static size_t s_out_len;  /* bytes queued */
static size_t s_out_sent; /* of those, bytes already handed to TinyUSB */

/* The listing being streamed, and the next row/slot to produce. */
typedef enum { LISTING_NONE = 0, LISTING_SETTINGS, LISTING_SCHEMA, LISTING_SCALES, LISTING_CONTENT } listing_t;
static listing_t s_listing;
static size_t s_listing_next;

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
    out_append(text);
    out_append("\n");
}

static void reply_ok(void) {
    reply("OK");
}

static void reply_err(const char *reason) {
    char buf[64];
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
    case LISTING_NONE:
    default:
        return false;
    }
}

/* Adds listing rows while there's room for another full line, then "OK"
 * when the listing runs out. */
static void fill_listing(void) {
    char text[USB_VENDOR_LINE_MAX];
    while (s_listing != LISTING_NONE && sizeof(s_out) - s_out_len > USB_VENDOR_LINE_MAX + 4u) {
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

static void handle_line(char *line) {
    char *cmd = strtok(line, " ");
    if (cmd == NULL) {
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
    if (strcmp(cmd, "INFO") == 0) {
        tiles_settings_persist_info_t info = tiles_settings_persist_get_info();
        snprintf(text, sizeof(text), "unit=%u/%u", (unsigned)TILES_UNIT_NUMBER, (unsigned)TILES_UNIT_COUNT);
        reply(text);
        snprintf(text, sizeof(text), "firmware=%u.%u.%u", (unsigned)TILES_FW_VERSION_MAJOR,
                 (unsigned)TILES_FW_VERSION_MINOR, (unsigned)TILES_FW_VERSION_PATCH);
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
        tiles_drum_seq_store_info_t drums = tiles_drum_seq_get_store_info();
        snprintf(text, sizeof(text), "drums.storage=%s", drums.storage ? "ok" : "none");
        reply(text);
        snprintf(text, sizeof(text), "drums.saved_bytes=%u", (unsigned)drums.saved_bytes);
        reply(text);
        snprintf(text, sizeof(text), "drums.saves=%lu", (unsigned long)drums.saves);
        reply(text);
        snprintf(text, sizeof(text), "drums.pending=%d", drums.pending ? 1 : 0);
        reply(text);
        reply_ok();
        return;
    }

    /* Bench tests for measuring current (tools/README.md "Current tests"):
     *   TEST LEDS <0-100>         every LED white at that % of full scale
     *   TEST MOTORS <n> [duty%]   pads 1..n's motors (capped at the power
     *                             mode's voices) for 8 s; duty default 100
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
        if (what != NULL && strcmp(what, "OFF") == 0) {
            tiles_haptics_test_stop();
            tiles_lighting_set_test_white(0u);
            reply_ok();
            return;
        }
        reply_err("usage-TEST-LEDS-n|MOTORS-n-[duty]|OFF");
        return;
    }

    /* Saves any pending change now instead of waiting for the debounced,
     * pads-idle save. The write pauses the firmware for tens of ms. */
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

    /* REBOOT BOOTSEL | APP: the scriptable path. (picotool's -f uses the
     * standard USB reset interface instead; see tusb_config.h.)
     *
     * BOOTSEL enters the ROM USB bootloader. reset_usb_boot() never returns,
     * so the "OK" is flushed by hand first, with a bounded wait (pump_out()
     * alone only reaches the peripheral FIFO, not the host). No activity LED
     * is wired, hence the 0 arguments.
     *
     * APP is a warm restart into this firmware (to test a fresh boot, or to
     * unstick a non-USB subsystem). watchdog_reboot() returns right away, so
     * the reply goes out during the delay like any other. */
    if (strcmp(cmd, "REBOOT") == 0) {
        char *what = strtok(NULL, " ");
        if (what != NULL && strcmp(what, "BOOTSEL") == 0) {
            reply_ok();
            uint32_t deadline_ms = to_ms_since_boot(get_absolute_time()) + 50u;
            while (to_ms_since_boot(get_absolute_time()) < deadline_ms) {
                tud_task();
                pump_out();
            }
            reset_usb_boot(0, 0);
        } else if (what != NULL && strcmp(what, "APP") == 0) {
            watchdog_reboot(0, 0, 100u);
            reply_ok();
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
    s_rx_len = 0u;
    s_out_len = 0u;
    s_out_sent = 0u;
}

void tiles_usb_vendor_scan(void) {
    if (!tud_vendor_mounted()) {
        s_out_len = 0u; /* host gone: drop the reply rather than hand it to the next host */
        s_out_sent = 0u;
        s_listing = LISTING_NONE;
        return;
    }
    pump_out();
    if (s_out_len == 0u && s_listing != LISTING_NONE) {
        fill_listing(); /* buffer drained: the listing's next rows */
        pump_out();
    }
    if (s_out_len > 0u || s_listing != LISTING_NONE) {
        return; /* still sending the previous reply: wait before the next command */
    }
    while (tud_vendor_available()) {
        uint8_t byte;
        if (tud_vendor_read(&byte, 1) == 0) {
            break;
        }
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
}
