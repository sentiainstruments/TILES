#include "usb_vendor.h"

#include "cv_gate.h"
#include "expression.h"
#include "pedal.h"

#include "tusb.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_VENDOR_LINE_MAX 128u

static char s_rx_line[USB_VENDOR_LINE_MAX];
static size_t s_rx_len;

static void reply(const char *text) {
    if (!tud_vendor_mounted()) {
        return;
    }
    tud_vendor_write(text, strlen(text));
    tud_vendor_write("\n", 1);
    tud_vendor_flush();
}

static void reply_ok(void) {
    reply("OK");
}

static void reply_err(const char *reason) {
    char buf[64];
    snprintf(buf, sizeof(buf), "ERR %s", reason);
    reply(buf);
}

/* strtof()/strtol() both leave *endptr pointing at the first
 * unconverted character -- an empty or fully-unparsed string leaves it
 * pointing at the start, and any leftover non-whitespace after a
 * partial parse (e.g. "1.5x") also fails this check. Rejects garbage
 * input with ERR bad-value instead of silently treating it as 0, the
 * same "a hard compile/parse error beats a silent wrong value"
 * reasoning this codebase already applies to on-flash data. */
static bool parse_float(const char *s, float *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end;
    float value = strtof(s, &end);
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_int(const char *s, long *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end;
    long value = strtol(s, &end, 10);
    if (*end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_bool(const char *s, bool *out) {
    if (s == NULL) {
        return false;
    }
    if (strcmp(s, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

/* GET side of every key below -- writes the current value into `out`
 * (sized USB_VENDOR_LINE_MAX, always enough for one number or short
 * word) and returns true, or returns false for an unrecognized key. */
static bool get_value(const char *key, char *out, size_t out_size) {
    if (strcmp(key, "pedal.mode") == 0) {
        snprintf(out, out_size, "%s", tiles_pedal_get_mode() == TILES_PEDAL_MODE_SUSTAIN ? "sustain" : "expression");
    } else if (strcmp(key, "pedal.polarity") == 0) {
        snprintf(out, out_size, "%s",
                  tiles_pedal_get_polarity() == TILES_PEDAL_POLARITY_NORMALLY_OPEN ? "normally_open"
                                                                                    : "normally_closed");
    } else if (strcmp(key, "expression.mpe_enabled") == 0) {
        snprintf(out, out_size, "%d", tiles_expression_is_mpe_enabled() ? 1 : 0);
    } else if (strcmp(key, "expression.pitch_bend_sensitivity") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_expression_get_pitch_bend_sensitivity());
    } else if (strcmp(key, "expression.aftertouch_sensitivity") == 0) {
        snprintf(out, out_size, "%u", (unsigned)tiles_expression_get_aftertouch_sensitivity());
    } else if (strcmp(key, "cv_gate.enabled") == 0) {
        snprintf(out, out_size, "%d", tiles_cv_gate_is_enabled() ? 1 : 0);
    } else if (strcmp(key, "cv_gate.pitch.volts_per_semitone") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_cv_gate_get_pitch_calibration().volts_per_semitone);
    } else if (strcmp(key, "cv_gate.pitch.reference_note") == 0) {
        snprintf(out, out_size, "%d", (int)tiles_cv_gate_get_pitch_calibration().reference_note);
    } else if (strcmp(key, "cv_gate.pitch.zero_trim_volts") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_cv_gate_get_pitch_calibration().zero_trim_volts);
    } else if (strcmp(key, "cv_gate.pitch.gain_trim") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_cv_gate_get_pitch_calibration().gain_trim);
    } else if (strcmp(key, "cv_gate.pressure.full_scale_volts") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_cv_gate_get_pressure_calibration().full_scale_volts);
    } else if (strcmp(key, "cv_gate.pressure.zero_trim_volts") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_cv_gate_get_pressure_calibration().zero_trim_volts);
    } else if (strcmp(key, "cv_gate.pressure.gain_trim") == 0) {
        snprintf(out, out_size, "%.6f", (double)tiles_cv_gate_get_pressure_calibration().gain_trim);
    } else {
        return false;
    }
    return true;
}

/* SET side of every key above -- same key list, opposite direction.
 * Returns false for an unrecognized key; *out_parse_error is set true
 * (regardless of return value's meaning) only when the key WAS
 * recognized but `value` couldn't be parsed into the type it needs,
 * so the caller can tell "unknown key" apart from "known key, bad
 * value" and report the right ERR reason for each. */
static bool set_value(const char *key, const char *value, bool *out_parse_error) {
    *out_parse_error = false;
    float f;
    long i;
    bool b;

    if (strcmp(key, "pedal.mode") == 0) {
        if (strcmp(value, "sustain") == 0) {
            tiles_pedal_set_mode(TILES_PEDAL_MODE_SUSTAIN);
        } else if (strcmp(value, "expression") == 0) {
            tiles_pedal_set_mode(TILES_PEDAL_MODE_EXPRESSION);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "pedal.polarity") == 0) {
        if (strcmp(value, "normally_open") == 0) {
            tiles_pedal_set_polarity(TILES_PEDAL_POLARITY_NORMALLY_OPEN);
        } else if (strcmp(value, "normally_closed") == 0) {
            tiles_pedal_set_polarity(TILES_PEDAL_POLARITY_NORMALLY_CLOSED);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "expression.mpe_enabled") == 0) {
        if (parse_bool(value, &b)) {
            tiles_expression_set_mpe_enabled(b);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "expression.pitch_bend_sensitivity") == 0) {
        if (parse_float(value, &f)) {
            tiles_expression_set_pitch_bend_sensitivity(f);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "expression.aftertouch_sensitivity") == 0) {
        if (parse_int(value, &i) && i > 0) {
            tiles_expression_set_aftertouch_sensitivity((uint16_t)i);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.enabled") == 0) {
        if (parse_bool(value, &b)) {
            tiles_cv_gate_set_enabled(b);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pitch.volts_per_semitone") == 0) {
        if (parse_float(value, &f)) {
            tiles_cv_pitch_calibration_t cal = tiles_cv_gate_get_pitch_calibration();
            cal.volts_per_semitone = f;
            tiles_cv_gate_set_pitch_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pitch.reference_note") == 0) {
        if (parse_int(value, &i)) {
            tiles_cv_pitch_calibration_t cal = tiles_cv_gate_get_pitch_calibration();
            cal.reference_note = (int16_t)i;
            tiles_cv_gate_set_pitch_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pitch.zero_trim_volts") == 0) {
        if (parse_float(value, &f)) {
            tiles_cv_pitch_calibration_t cal = tiles_cv_gate_get_pitch_calibration();
            cal.zero_trim_volts = f;
            tiles_cv_gate_set_pitch_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pitch.gain_trim") == 0) {
        if (parse_float(value, &f)) {
            tiles_cv_pitch_calibration_t cal = tiles_cv_gate_get_pitch_calibration();
            cal.gain_trim = f;
            tiles_cv_gate_set_pitch_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pressure.full_scale_volts") == 0) {
        if (parse_float(value, &f)) {
            tiles_cv_pressure_calibration_t cal = tiles_cv_gate_get_pressure_calibration();
            cal.full_scale_volts = f;
            tiles_cv_gate_set_pressure_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pressure.zero_trim_volts") == 0) {
        if (parse_float(value, &f)) {
            tiles_cv_pressure_calibration_t cal = tiles_cv_gate_get_pressure_calibration();
            cal.zero_trim_volts = f;
            tiles_cv_gate_set_pressure_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else if (strcmp(key, "cv_gate.pressure.gain_trim") == 0) {
        if (parse_float(value, &f)) {
            tiles_cv_pressure_calibration_t cal = tiles_cv_gate_get_pressure_calibration();
            cal.gain_trim = f;
            tiles_cv_gate_set_pressure_calibration(cal);
        } else {
            *out_parse_error = true;
        }
    } else {
        return false;
    }
    return true;
}

/* Every key get_value()/set_value() know about -- LIST's own single
 * source of truth, so a key added to one but forgotten in this array
 * just doesn't show up in LIST rather than silently disagreeing with
 * it. */
static const char *const ALL_KEYS[] = {
    "pedal.mode",
    "pedal.polarity",
    "expression.mpe_enabled",
    "expression.pitch_bend_sensitivity",
    "expression.aftertouch_sensitivity",
    "cv_gate.enabled",
    "cv_gate.pitch.volts_per_semitone",
    "cv_gate.pitch.reference_note",
    "cv_gate.pitch.zero_trim_volts",
    "cv_gate.pitch.gain_trim",
    "cv_gate.pressure.full_scale_volts",
    "cv_gate.pressure.zero_trim_volts",
    "cv_gate.pressure.gain_trim",
};
#define NUM_ALL_KEYS (sizeof(ALL_KEYS) / sizeof(ALL_KEYS[0]))

static void handle_line(char *line) {
    char *cmd = strtok(line, " ");
    if (cmd == NULL) {
        return; /* blank line -- no response, matches a plain terminal's own "empty Enter does nothing" */
    }

    if (strcmp(cmd, "LIST") == 0) {
        char value[USB_VENDOR_LINE_MAX];
        char out[USB_VENDOR_LINE_MAX];
        for (size_t i = 0; i < NUM_ALL_KEYS; i++) {
            get_value(ALL_KEYS[i], value, sizeof(value));
            snprintf(out, sizeof(out), "%s=%s", ALL_KEYS[i], value);
            reply(out);
        }
        reply_ok();
        return;
    }

    char *key = strtok(NULL, " ");
    if (key == NULL) {
        reply_err("missing-key");
        return;
    }

    if (strcmp(cmd, "GET") == 0) {
        char value[USB_VENDOR_LINE_MAX];
        if (get_value(key, value, sizeof(value))) {
            reply(value);
        } else {
            reply_err("unknown-key");
        }
        return;
    }

    if (strcmp(cmd, "SET") == 0) {
        char *value = strtok(NULL, " ");
        bool parse_error;
        if (!set_value(key, value, &parse_error)) {
            reply_err("unknown-key");
        } else if (parse_error) {
            reply_err("bad-value");
        } else {
            reply_ok();
        }
        return;
    }

    reply_err("unknown-command");
}

void tiles_usb_vendor_init(void) {
    s_rx_len = 0u;
}

void tiles_usb_vendor_scan(void) {
    if (!tud_vendor_mounted()) {
        return;
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
