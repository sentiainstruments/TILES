#pragma once

/*
 * The settings table -- one registry of every user-tunable value, and the
 * single thing both the USB control interface and flash persistence talk to.
 *
 * Real feedback: "yes start with the settings table and flash saving." Before
 * this, usb_vendor.c held a hand-written strcmp() chain (one branch to read
 * and one to write, per key, plus a third list for LIST), nothing survived a
 * reboot, and the values worth tuning by ear -- LED levels, DIN polarity, the
 * melodic-harmonics switch -- were compile-time constants that needed a
 * rebuild and reflash to change. Now a setting is ONE row in
 * profiles/settings_table.c (id, key, type, range, how to read it, how to
 * apply it) and gets, for free: GET/SET/LIST/SCHEMA/RESET over USB, range
 * checking, and saving to flash. Adding a setting never touches the protocol.
 *
 * Design choices worth knowing:
 *  - The MODULES stay the source of truth. Each row's get()/set() call the
 *    module that owns the value (pedal, expression, lighting, ...). The table
 *    doesn't hold a second copy that could drift from the real one, and a
 *    change made on the device itself is seen exactly like one made by the app.
 *  - A setting's DEFAULT is captured from the module at boot (before any saved
 *    value is applied), not written a second time in the table. There is
 *    nowhere for a "default" in the table to disagree with what the module
 *    really boots with, and the schema reports the true default.
 *  - Persistence is SPARSE: only values that differ from their default are
 *    saved. A blank store means "all defaults", and a setting the user never
 *    touched follows the firmware's default if a later build changes it. (The
 *    cost: a user who explicitly set a value equal to today's default will
 *    follow it if it changes -- deliberate, and the safer of the two.)
 *  - IDs are permanent. The numeric id is what is written to flash and what a
 *    future binary protocol will use; the text key is for humans and scripts.
 *    Never reuse or renumber an id; retire it by leaving a gap.
 *
 * This file and settings.c are pure logic (no Pico SDK), tested natively --
 * firmware/test/test_settings.c. settings_table.c is the hardware-side
 * binding; settings_persist.c is the debounced save/load on top of storage/.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TILES_SETTING_BOOL = 0, /* 0 or 1; value in .u */
    TILES_SETTING_UINT,     /* .u, inclusive [min.u, max.u] */
    TILES_SETTING_INT,      /* .i, inclusive [min.i, max.i] */
    TILES_SETTING_FLOAT,    /* .f, inclusive [min.f, max.f], never NaN */
    TILES_SETTING_ENUM,     /* .u indexes enum_names[0..max.u] */
} tiles_setting_type_t;

typedef union {
    uint32_t u;
    int32_t i;
    float f;
} tiles_setting_value_t;

/* Setting flags. */
#define TILES_SETTING_VOLATILE 0x01u /* never saved to or restored from flash: boots at its default every time */

typedef struct {
    uint16_t id;
    const char *key; /* "group.name", lowercase, no spaces */
    tiles_setting_type_t type;
    tiles_setting_value_t min;
    tiles_setting_value_t max;
    const char *const *enum_names; /* ENUM only: max.u + 1 names, no spaces */
    tiles_setting_value_t (*get)(void);
    void (*set)(tiles_setting_value_t value); /* only ever called with an in-range value */
    uint8_t flags;                            /* TILES_SETTING_* -- omit for the usual "saved" behaviour */
} tiles_setting_def_t;

/* Table-row constructors (a row is a plain aggregate; these just spell out the
 * trailing flags so no compiler warns about a missing initializer):
 *   TILES_SETTING(id, key, type, min, max, enum_names, get, set)        -- saved to flash
 *   TILES_SETTING_V(...same...)                                         -- never saved (volatile) */
#define TILES_SETTING(id, key, type, min, max, names, get, set) {id, key, type, min, max, names, get, set, 0u}
#define TILES_SETTING_V(id, key, type, min, max, names, get, set) \
    {id, key, type, min, max, names, get, set, TILES_SETTING_VOLATILE}

typedef enum {
    TILES_SETTINGS_OK = 0,
    TILES_SETTINGS_UNKNOWN_KEY,
    TILES_SETTINGS_BAD_VALUE,    /* not parseable as this setting's type */
    TILES_SETTINGS_OUT_OF_RANGE, /* parsed, but outside [min, max] (or an unknown enum name) */
} tiles_settings_result_t;

/* ---- registry ---- */

/* Registers the table. `defs` must outlive the program (a static const array). */
void tiles_settings_init(const tiles_setting_def_t *defs, size_t count);

/* Records every setting's CURRENT value as its default. Call once at boot after
 * every module has initialized and BEFORE any saved values are applied. */
void tiles_settings_capture_defaults(void);

size_t tiles_settings_count(void);
const tiles_setting_def_t *tiles_settings_at(size_t index);
const tiles_setting_def_t *tiles_settings_find_key(const char *key);
const tiles_setting_def_t *tiles_settings_find_id(uint16_t id);
tiles_setting_value_t tiles_settings_default_of(const tiles_setting_def_t *def);

/* ---- text (the USB shell) ---- */

void tiles_settings_format(const tiles_setting_def_t *def, tiles_setting_value_t value, char *out, size_t cap);
bool tiles_settings_get_text(const char *key, char *out, size_t cap);

/* Parses `text` for `def` and range-checks it. */
tiles_settings_result_t tiles_settings_parse(const tiles_setting_def_t *def, const char *text,
                                              tiles_setting_value_t *out);
/* Parses, range-checks and APPLIES (calls the row's set()). */
tiles_settings_result_t tiles_settings_set_text(const char *key, const char *text);

/* Applies a setting's default. `key` NULL resets every setting. Returns false for an unknown key. */
bool tiles_settings_reset(const char *key);

/* One line describing a setting for the app to build a UI from, e.g.
 *   id=513 key=expression.pitch_bend_sensitivity type=float min=0.001 max=1 default=0.065
 *   id=256 key=pedal.mode type=enum values=sustain|expression default=sustain
 * `out` should be at least 160 bytes. */
void tiles_settings_describe(const tiles_setting_def_t *def, char *out, size_t cap);

/* ---- persistence format (used by settings_persist.c; exposed for tests) ---- */

/* Version of the blob layout below, stored with it. */
#define TILES_SETTINGS_BLOB_VERSION 1u

/* Serializes every setting whose current value differs from its default (except
 * TILES_SETTING_VOLATILE ones, which are never saved):
 *   repeated { u16 id (LE), u8 type, u32 value (LE) }   -- 7 bytes each
 * in table order (deterministic, so two snapshots can be compared byte for
 * byte). Returns the length, or 0 if `cap` is too small. An all-default state
 * is a legitimate zero-length blob, so "too small" is reported via *overflow. */
size_t tiles_settings_serialize(uint8_t *out, size_t cap, bool *overflow);

/* Applies a blob produced by tiles_settings_serialize(): unknown ids, a changed
 * type, an out-of-range value, or a truncated tail are skipped, never fatal.
 * Returns how many settings were applied. */
size_t tiles_settings_apply_blob(const uint8_t *blob, size_t len);
