#include "settings.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_MAX 64u
#define BLOB_ENTRY_SIZE 7u

static const tiles_setting_def_t *s_defs;
static size_t s_count;
static tiles_setting_value_t s_defaults[SETTINGS_MAX];

void tiles_settings_init(const tiles_setting_def_t *defs, size_t count) {
    s_defs = defs;
    s_count = count > SETTINGS_MAX ? SETTINGS_MAX : count;
    for (size_t i = 0; i < SETTINGS_MAX; i++) {
        s_defaults[i].u = 0u;
    }
}

void tiles_settings_capture_defaults(void) {
    for (size_t i = 0; i < s_count; i++) {
        s_defaults[i] = s_defs[i].get();
    }
}

size_t tiles_settings_count(void) {
    return s_count;
}

const tiles_setting_def_t *tiles_settings_at(size_t index) {
    return index < s_count ? &s_defs[index] : NULL;
}

const tiles_setting_def_t *tiles_settings_find_key(const char *key) {
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_defs[i].key, key) == 0) {
            return &s_defs[i];
        }
    }
    return NULL;
}

const tiles_setting_def_t *tiles_settings_find_id(uint16_t id) {
    for (size_t i = 0; i < s_count; i++) {
        if (s_defs[i].id == id) {
            return &s_defs[i];
        }
    }
    return NULL;
}

static size_t index_of(const tiles_setting_def_t *def) {
    return (size_t)(def - s_defs);
}

tiles_setting_value_t tiles_settings_default_of(const tiles_setting_def_t *def) {
    return s_defaults[index_of(def)];
}

/* ---- text ---- */

void tiles_settings_format(const tiles_setting_def_t *def, tiles_setting_value_t value, char *out, size_t cap) {
    switch (def->type) {
    case TILES_SETTING_BOOL:
        snprintf(out, cap, "%u", (unsigned)(value.u ? 1u : 0u));
        break;
    case TILES_SETTING_UINT:
        snprintf(out, cap, "%lu", (unsigned long)value.u);
        break;
    case TILES_SETTING_INT:
        snprintf(out, cap, "%ld", (long)value.i);
        break;
    case TILES_SETTING_FLOAT:
        snprintf(out, cap, "%.6f", (double)value.f);
        break;
    case TILES_SETTING_ENUM:
        snprintf(out, cap, "%s", value.u <= def->max.u ? def->enum_names[value.u] : "?");
        break;
    }
}

bool tiles_settings_get_text(const char *key, char *out, size_t cap) {
    const tiles_setting_def_t *def = tiles_settings_find_key(key);
    if (def == NULL) {
        return false;
    }
    tiles_settings_format(def, def->get(), out, cap);
    return true;
}

/* strtol()/strtof() leave *end at the first unconverted character; anything
 * left over (or nothing consumed) is garbage, not "0". */
tiles_settings_result_t tiles_settings_parse(const tiles_setting_def_t *def, const char *text,
                                              tiles_setting_value_t *out) {
    if (text == NULL || *text == '\0') {
        return TILES_SETTINGS_BAD_VALUE;
    }
    char *end;
    switch (def->type) {
    case TILES_SETTING_BOOL:
        if (strcmp(text, "1") == 0) {
            out->u = 1u;
        } else if (strcmp(text, "0") == 0) {
            out->u = 0u;
        } else {
            return TILES_SETTINGS_BAD_VALUE;
        }
        return TILES_SETTINGS_OK;
    case TILES_SETTING_UINT: {
        if (*text == '-') {
            return TILES_SETTINGS_BAD_VALUE;
        }
        unsigned long v = strtoul(text, &end, 10);
        if (*end != '\0') {
            return TILES_SETTINGS_BAD_VALUE;
        }
        if (v < def->min.u || v > def->max.u) {
            return TILES_SETTINGS_OUT_OF_RANGE;
        }
        out->u = (uint32_t)v;
        return TILES_SETTINGS_OK;
    }
    case TILES_SETTING_INT: {
        long v = strtol(text, &end, 10);
        if (*end != '\0') {
            return TILES_SETTINGS_BAD_VALUE;
        }
        if (v < def->min.i || v > def->max.i) {
            return TILES_SETTINGS_OUT_OF_RANGE;
        }
        out->i = (int32_t)v;
        return TILES_SETTINGS_OK;
    }
    case TILES_SETTING_FLOAT: {
        float v = strtof(text, &end);
        if (*end != '\0' || isnan(v)) {
            return TILES_SETTINGS_BAD_VALUE;
        }
        if (v < def->min.f || v > def->max.f) {
            return TILES_SETTINGS_OUT_OF_RANGE;
        }
        out->f = v;
        return TILES_SETTINGS_OK;
    }
    case TILES_SETTING_ENUM:
        for (uint32_t i = 0u; i <= def->max.u; i++) {
            if (strcmp(text, def->enum_names[i]) == 0) {
                out->u = i;
                return TILES_SETTINGS_OK;
            }
        }
        return TILES_SETTINGS_OUT_OF_RANGE;
    }
    return TILES_SETTINGS_BAD_VALUE;
}

tiles_settings_result_t tiles_settings_set_text(const char *key, const char *text) {
    const tiles_setting_def_t *def = tiles_settings_find_key(key);
    if (def == NULL) {
        return TILES_SETTINGS_UNKNOWN_KEY;
    }
    tiles_setting_value_t v;
    tiles_settings_result_t r = tiles_settings_parse(def, text, &v);
    if (r == TILES_SETTINGS_OK) {
        def->set(v);
    }
    return r;
}

bool tiles_settings_reset(const char *key) {
    if (key == NULL) {
        for (size_t i = 0; i < s_count; i++) {
            s_defs[i].set(s_defaults[i]);
        }
        return true;
    }
    const tiles_setting_def_t *def = tiles_settings_find_key(key);
    if (def == NULL) {
        return false;
    }
    def->set(s_defaults[index_of(def)]);
    return true;
}

/* Appends formatted text at out[*len], never past cap. */
static void append(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *len += (size_t)n;
        if (*len >= cap) {
            *len = cap - 1u; /* truncated: keep the terminator */
        }
    }
}

void tiles_settings_describe(const tiles_setting_def_t *def, char *out, size_t cap) {
    static const char *const TYPE_NAMES[] = {"bool", "uint", "int", "float", "enum"};
    char def_text[32];
    tiles_settings_format(def, tiles_settings_default_of(def), def_text, sizeof(def_text));
    size_t len = 0u;
    out[0] = '\0';
    append(out, cap, &len, "id=%u key=%s type=%s", (unsigned)def->id, def->key, TYPE_NAMES[def->type]);
    switch (def->type) {
    case TILES_SETTING_BOOL:
        break;
    case TILES_SETTING_UINT:
        append(out, cap, &len, " min=%lu max=%lu", (unsigned long)def->min.u, (unsigned long)def->max.u);
        break;
    case TILES_SETTING_INT:
        append(out, cap, &len, " min=%ld max=%ld", (long)def->min.i, (long)def->max.i);
        break;
    case TILES_SETTING_FLOAT:
        append(out, cap, &len, " min=%g max=%g", (double)def->min.f, (double)def->max.f);
        break;
    case TILES_SETTING_ENUM:
        append(out, cap, &len, " values=");
        for (uint32_t i = 0u; i <= def->max.u; i++) {
            append(out, cap, &len, "%s%s", i == 0u ? "" : "|", def->enum_names[i]);
        }
        break;
    }
    append(out, cap, &len, " default=%s", def_text);
    if (def->flags & TILES_SETTING_VOLATILE) {
        append(out, cap, &len, " persist=0");
    }
}

/* ---- persistence blob ---- */

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t tiles_settings_serialize(uint8_t *out, size_t cap, bool *overflow) {
    size_t len = 0u;
    *overflow = false;
    for (size_t i = 0; i < s_count; i++) {
        if (s_defs[i].flags & TILES_SETTING_VOLATILE) {
            continue;
        }
        tiles_setting_value_t v = s_defs[i].get();
        if (v.u == s_defaults[i].u) { /* bit-exact: floats included */
            continue;
        }
        if (len + BLOB_ENTRY_SIZE > cap) {
            *overflow = true;
            return 0u;
        }
        put_u16(&out[len], s_defs[i].id);
        out[len + 2u] = (uint8_t)s_defs[i].type;
        put_u32(&out[len + 3u], v.u);
        len += BLOB_ENTRY_SIZE;
    }
    return len;
}

/* Range check for a raw stored value (the same rules tiles_settings_parse()
 * applies to text). */
static bool value_in_range(const tiles_setting_def_t *def, tiles_setting_value_t v) {
    switch (def->type) {
    case TILES_SETTING_BOOL:
        return v.u <= 1u;
    case TILES_SETTING_UINT:
    case TILES_SETTING_ENUM:
        return v.u >= def->min.u && v.u <= def->max.u;
    case TILES_SETTING_INT:
        return v.i >= def->min.i && v.i <= def->max.i;
    case TILES_SETTING_FLOAT:
        return !isnan(v.f) && v.f >= def->min.f && v.f <= def->max.f;
    }
    return false;
}

size_t tiles_settings_apply_blob(const uint8_t *blob, size_t len) {
    size_t applied = 0u;
    for (size_t pos = 0u; pos + BLOB_ENTRY_SIZE <= len; pos += BLOB_ENTRY_SIZE) {
        const tiles_setting_def_t *def = tiles_settings_find_id(get_u16(&blob[pos]));
        if (def == NULL || (uint8_t)def->type != blob[pos + 2u] || (def->flags & TILES_SETTING_VOLATILE)) {
            continue; /* a setting from a newer/older build, one whose type changed, or one that is never restored */
        }
        tiles_setting_value_t v;
        v.u = get_u32(&blob[pos + 3u]);
        if (!value_in_range(def, v)) {
            continue;
        }
        def->set(v);
        applied++;
    }
    return applied;
}
