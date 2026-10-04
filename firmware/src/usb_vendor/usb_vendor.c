#include "usb_vendor.h"

#include "board/unit_id.h"
#include "product_identity.h"
#include "settings.h"
#include "settings_persist.h"

#include "tusb.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define USB_VENDOR_LINE_MAX 128u

/* Replies are queued here and drained into the 64-byte TinyUSB FIFO as it
 * has room (pump_out()), so multi-line replies (LIST/SCHEMA/INFO) never
 * lose their tail. SCHEMA, the largest (~3 KB), fits whole: the next
 * command is read only once the previous reply is fully sent. */
#define USB_VENDOR_OUT_MAX 4096u

static char s_rx_line[USB_VENDOR_LINE_MAX];
static size_t s_rx_len;

static char s_out[USB_VENDOR_OUT_MAX];
static size_t s_out_len;  /* bytes queued */
static size_t s_out_sent; /* of those, bytes already handed to TinyUSB */

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

static void handle_line(char *line) {
    char *cmd = strtok(line, " ");
    if (cmd == NULL) {
        return; /* blank line: no reply, like a terminal */
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

    /* One line per setting (id, key, type, range/values, default): enough for
     * a UI to build a control without a hard-coded list. */
    if (strcmp(cmd, "SCHEMA") == 0) {
        for (size_t i = 0; i < tiles_settings_count(); i++) {
            tiles_settings_describe(tiles_settings_at(i), text, sizeof(text));
            reply(text);
        }
        reply_ok();
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
    s_rx_len = 0u;
    s_out_len = 0u;
    s_out_sent = 0u;
}

void tiles_usb_vendor_scan(void) {
    if (!tud_vendor_mounted()) {
        s_out_len = 0u; /* host gone: drop the reply rather than hand it to the next host */
        s_out_sent = 0u;
        return;
    }
    pump_out();
    if (s_out_len > 0u) {
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
