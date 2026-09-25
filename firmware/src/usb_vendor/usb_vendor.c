#include "usb_vendor.h"

#include "settings.h"
#include "settings_persist.h"

#include "tusb.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define USB_VENDOR_LINE_MAX 128u

/* Responses are queued here and drained into the USB IN FIFO as it has room
 * (see pump_out()). Before the settings table, every reply was written straight
 * to a 64-byte TinyUSB FIFO with nothing draining it between lines, so any
 * response longer than one packet could silently lose its tail -- fine for one
 * short line, not for LIST/SCHEMA/INFO, which are dozens of lines. A whole
 * response (SCHEMA is the largest, ~3 KB) always fits: the next command is only
 * read once the previous response has been fully sent. */
#define USB_VENDOR_OUT_MAX 4096u

static char s_rx_line[USB_VENDOR_LINE_MAX];
static size_t s_rx_len;

static char s_out[USB_VENDOR_OUT_MAX];
static size_t s_out_len;  /* bytes queued */
static size_t s_out_sent; /* of those, bytes already handed to TinyUSB */

static void out_append(const char *text) {
    size_t n = strlen(text);
    if (s_out_len + n + 1u > sizeof(s_out)) {
        return; /* can't happen for any response this file builds; never overruns if it ever did */
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

static void handle_line(char *line) {
    char *cmd = strtok(line, " ");
    if (cmd == NULL) {
        return; /* blank line -- no response, matches a plain terminal's own "empty Enter does nothing" */
    }
    char text[USB_VENDOR_LINE_MAX];

    if (strcmp(cmd, "LIST") == 0) {
        for (size_t i = 0; i < tiles_settings_count(); i++) {
            const tiles_setting_def_t *def = tiles_settings_at(i);
            char value[48];
            tiles_settings_format(def, def->get(), value, sizeof(value));
            snprintf(text, sizeof(text), "%s=%s", def->key, value);
            reply(text);
        }
        reply_ok();
        return;
    }

    /* One line per setting: id, key, type, range/values, default -- everything a
     * UI needs to build a control for it without hard-coding the list. */
    if (strcmp(cmd, "SCHEMA") == 0) {
        for (size_t i = 0; i < tiles_settings_count(); i++) {
            tiles_settings_describe(tiles_settings_at(i), text, sizeof(text));
            reply(text);
        }
        reply_ok();
        return;
    }

    /* Flash-store status, for diagnosing "did it save?". */
    if (strcmp(cmd, "INFO") == 0) {
        tiles_settings_persist_info_t info = tiles_settings_persist_get_info();
        snprintf(text, sizeof(text), "settings=%u", (unsigned)tiles_settings_count());
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
        reply_ok();
        return;
    }

    /* Writes any unsaved change now instead of waiting for the automatic,
     * debounced, hands-off-the-pads save. (A flash write pauses the firmware
     * for tens of milliseconds -- don't send this mid-phrase.) */
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

    /* RESET <key> restores one setting's default; RESET ALL restores every one.
     * Like SET, it applies immediately and is saved automatically. */
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
    s_rx_len = 0u;
    s_out_len = 0u;
    s_out_sent = 0u;
}

void tiles_usb_vendor_scan(void) {
    if (!tud_vendor_mounted()) {
        s_out_len = 0u; /* nobody to send it to; don't hand a stale response to the next host */
        s_out_sent = 0u;
        return;
    }
    pump_out();
    if (s_out_len > 0u) {
        return; /* still sending the previous response: don't start the next command yet */
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
                return; /* one command per scan; any further bytes wait in TinyUSB's own FIFO */
            }
            continue;
        }
        if (s_rx_len < USB_VENDOR_LINE_MAX - 1u) {
            s_rx_line[s_rx_len++] = (char)byte;
        }
        /* Line too long for USB_VENDOR_LINE_MAX -- silently drops the
         * overflow bytes until the next newline rather than growing
         * the buffer or wrapping; no key or value this protocol
         * defines comes anywhere close to this limit, so a line this
         * long is already malformed input, not a real command that
         * got unluckily truncated. */
    }
}
