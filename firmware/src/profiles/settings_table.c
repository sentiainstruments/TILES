#include "settings_table.h"

#include "settings.h"
#include "settings_persist.h"

#include "board_pins.h"
#include "cv_gate.h"
#include "din_midi.h"
#include "expression.h"
#include "lighting.h"
#include "pedal.h"
#include "storage_flash.h"
#include "touch.h"

#include "pico/time.h"

#include <stdio.h>

/* ---- helpers to build a value ---- */
static tiles_setting_value_t U(uint32_t v) {
    tiles_setting_value_t x;
    x.u = v;
    return x;
}
static tiles_setting_value_t I(int32_t v) {
    tiles_setting_value_t x;
    x.i = v;
    return x;
}
static tiles_setting_value_t F(float v) {
    tiles_setting_value_t x;
    x.f = v;
    return x;
}

/* ---- pedal ---- */
static tiles_setting_value_t get_pedal_mode(void) { return U((uint32_t)tiles_pedal_get_mode()); }
static void set_pedal_mode(tiles_setting_value_t v) { tiles_pedal_set_mode((tiles_pedal_mode_t)v.u); }
static tiles_setting_value_t get_pedal_polarity(void) { return U((uint32_t)tiles_pedal_get_polarity()); }
static void set_pedal_polarity(tiles_setting_value_t v) { tiles_pedal_set_polarity((tiles_pedal_polarity_t)v.u); }

/* ---- expression ---- */
static tiles_setting_value_t get_mpe(void) { return U(tiles_expression_is_mpe_enabled() ? 1u : 0u); }
static void set_mpe(tiles_setting_value_t v) { tiles_expression_set_mpe_enabled(v.u != 0u); }
static tiles_setting_value_t get_bend(void) { return F(tiles_expression_get_pitch_bend_sensitivity()); }
static void set_bend(tiles_setting_value_t v) { tiles_expression_set_pitch_bend_sensitivity(v.f); }
static tiles_setting_value_t get_aftertouch(void) { return U(tiles_expression_get_aftertouch_sensitivity()); }
static void set_aftertouch(tiles_setting_value_t v) { tiles_expression_set_aftertouch_sensitivity((uint16_t)v.u); }
static tiles_setting_value_t get_harmonics(void) { return U(tiles_expression_is_melodic_harmonics_enabled() ? 1u : 0u); }
static void set_harmonics(tiles_setting_value_t v) { tiles_expression_set_melodic_harmonics_enabled(v.u != 0u); }

/* ---- CV / gate ---- */
static tiles_setting_value_t get_cv_enabled(void) { return U(tiles_cv_gate_is_enabled() ? 1u : 0u); }
static void set_cv_enabled(tiles_setting_value_t v) { tiles_cv_gate_set_enabled(v.u != 0u); }

/* Each calibration field is read-modify-write on its struct. */
#define CV_PITCH_FIELD(name, field, CTOR, member)                                                \
    static tiles_setting_value_t get_##name(void) {                                               \
        return CTOR(tiles_cv_gate_get_pitch_calibration().field);                                 \
    }                                                                                             \
    static void set_##name(tiles_setting_value_t v) {                                             \
        tiles_cv_pitch_calibration_t c = tiles_cv_gate_get_pitch_calibration();                   \
        c.field = (__typeof__(c.field))v.member;                                                  \
        tiles_cv_gate_set_pitch_calibration(c);                                                   \
    }
#define CV_PRESSURE_FIELD(name, field, CTOR, member)                                             \
    static tiles_setting_value_t get_##name(void) {                                               \
        return CTOR(tiles_cv_gate_get_pressure_calibration().field);                              \
    }                                                                                             \
    static void set_##name(tiles_setting_value_t v) {                                             \
        tiles_cv_pressure_calibration_t c = tiles_cv_gate_get_pressure_calibration();             \
        c.field = (__typeof__(c.field))v.member;                                                  \
        tiles_cv_gate_set_pressure_calibration(c);                                                \
    }
CV_PITCH_FIELD(cv_pitch_vps, volts_per_semitone, F, f)
CV_PITCH_FIELD(cv_pitch_ref, reference_note, I, i)
CV_PITCH_FIELD(cv_pitch_zero, zero_trim_volts, F, f)
CV_PITCH_FIELD(cv_pitch_gain, gain_trim, F, f)
CV_PRESSURE_FIELD(cv_press_full, full_scale_volts, F, f)
CV_PRESSURE_FIELD(cv_press_zero, zero_trim_volts, F, f)
CV_PRESSURE_FIELD(cv_press_gain, gain_trim, F, f)

/* ---- look (LED levels; lighting.h) ---- */
#define LOOK_ENTRY(name, param)                                                       \
    static tiles_setting_value_t get_look_##name(void) { return U(tiles_lighting_get_look(param)); } \
    static void set_look_##name(tiles_setting_value_t v) { tiles_lighting_set_look(param, (uint16_t)v.u); }
LOOK_ENTRY(idle, TILES_LOOK_IDLE_BASELINE_PERCENT)
LOOK_ENTRY(natural, TILES_LOOK_NATURAL_PERCENT)
LOOK_ENTRY(root, TILES_LOOK_ROOT_PERCENT)
LOOK_ENTRY(fifth, TILES_LOOK_FIFTH_PERCENT)
LOOK_ENTRY(fifth_red, TILES_LOOK_FIFTH_RED_TINT_PERCENT)
LOOK_ENTRY(echo_tint, TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT)
LOOK_ENTRY(echo_g, TILES_LOOK_ECHO_SECONDARY_G_PERCENT)
LOOK_ENTRY(echo_b, TILES_LOOK_ECHO_SECONDARY_B_PERCENT)
LOOK_ENTRY(echo_flash, TILES_LOOK_ECHO_FLASH_MS)

/* ---- MIDI ---- */
static tiles_setting_value_t get_din_type(void) { return U((uint32_t)tiles_din_midi_get_trs_type()); }
static void set_din_type(tiles_setting_value_t v) { tiles_din_midi_set_trs_type((tiles_din_midi_trs_type_t)v.u); }

static const char *const PEDAL_MODE_NAMES[] = {"sustain", "expression"};
static const char *const PEDAL_POLARITY_NAMES[] = {"normally_open", "normally_closed"};
static const char *const DIN_TYPE_NAMES[] = {"a", "b"};

/* THE TABLE. One row per setting. Rules:
 *  - `id` is permanent (it is what is written to flash and what a binary protocol will use): never
 *    reuse or renumber one -- retire it by leaving a gap. Ids are grouped by hundreds:
 *    0x01xx pedal, 0x02xx expression, 0x03xx CV/gate, 0x04xx LED look, 0x05xx MIDI, 0x06xx features.
 *  - `key` is the human/script name.
 *  - The range is what the setting will accept -- wide enough for any value that is actually useful,
 *    narrow enough to refuse something nonsensical (or unsafe for the CV output).
 *  - The DEFAULT is not written here: it is captured from the module at boot (settings.h).
 * Values are {min, max} in the order below; `.u` for bool/uint/enum, `.i` for int, `.f` for float. */
static const tiles_setting_def_t TABLE[] = {
    /* pedal */
    TILES_SETTING(0x0100, "pedal.mode", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, PEDAL_MODE_NAMES, get_pedal_mode,
                  set_pedal_mode),
    TILES_SETTING(0x0101, "pedal.polarity", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, PEDAL_POLARITY_NAMES,
                  get_pedal_polarity, set_pedal_polarity),
    /* expression */
    TILES_SETTING(0x0200, "expression.mpe_enabled", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_mpe, set_mpe),
    TILES_SETTING(0x0201, "expression.pitch_bend_sensitivity", TILES_SETTING_FLOAT, {.f = 0.001f}, {.f = 1.0f}, NULL,
                  get_bend, set_bend),
    TILES_SETTING(0x0202, "expression.aftertouch_sensitivity", TILES_SETTING_UINT, {.u = 1}, {.u = 65535}, NULL,
                  get_aftertouch, set_aftertouch),
    /* CV / gate (calibration values limited to what the jack can actually use).
     * The enable switch is volatile on purpose: CV/gate stays an explicit, per-session switch that boots OFF ("keep cv
     * gate implemented but off rn"; services/cv_gate.h) -- the calibration below IS saved, this switch is not. */
    TILES_SETTING_V(0x0300, "cv_gate.enabled", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_cv_enabled,
                    set_cv_enabled),
    TILES_SETTING(0x0301, "cv_gate.pitch.volts_per_semitone", TILES_SETTING_FLOAT, {.f = 0.001f}, {.f = 1.0f}, NULL,
                  get_cv_pitch_vps, set_cv_pitch_vps),
    TILES_SETTING(0x0302, "cv_gate.pitch.reference_note", TILES_SETTING_INT, {.i = 0}, {.i = 127}, NULL,
                  get_cv_pitch_ref, set_cv_pitch_ref),
    TILES_SETTING(0x0303, "cv_gate.pitch.zero_trim_volts", TILES_SETTING_FLOAT, {.f = -2.5f}, {.f = 2.5f}, NULL,
                  get_cv_pitch_zero, set_cv_pitch_zero),
    TILES_SETTING(0x0304, "cv_gate.pitch.gain_trim", TILES_SETTING_FLOAT, {.f = 0.5f}, {.f = 2.0f}, NULL,
                  get_cv_pitch_gain, set_cv_pitch_gain),
    TILES_SETTING(0x0305, "cv_gate.pressure.full_scale_volts", TILES_SETTING_FLOAT, {.f = 0.1f}, {.f = 10.0f}, NULL,
                  get_cv_press_full, set_cv_press_full),
    TILES_SETTING(0x0306, "cv_gate.pressure.zero_trim_volts", TILES_SETTING_FLOAT, {.f = -2.5f}, {.f = 2.5f}, NULL,
                  get_cv_press_zero, set_cv_press_zero),
    TILES_SETTING(0x0307, "cv_gate.pressure.gain_trim", TILES_SETTING_FLOAT, {.f = 0.5f}, {.f = 2.0f}, NULL,
                  get_cv_press_gain, set_cv_press_gain),
    /* LED look: whole percent of the (fixed) brightness ceiling; see lighting.h */
    TILES_SETTING(0x0400, "look.idle_baseline_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL, get_look_idle,
                  set_look_idle),
    TILES_SETTING(0x0401, "look.natural_pad_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_natural, set_look_natural),
    TILES_SETTING(0x0402, "look.root_pad_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL, get_look_root,
                  set_look_root),
    TILES_SETTING(0x0403, "look.fifth_pad_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL, get_look_fifth,
                  set_look_fifth),
    TILES_SETTING(0x0404, "look.fifth_red_tint_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_fifth_red, set_look_fifth_red),
    TILES_SETTING(0x0405, "look.echo_sustain_tint_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_echo_tint, set_look_echo_tint),
    TILES_SETTING(0x0406, "look.echo_secondary_g_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_echo_g, set_look_echo_g),
    TILES_SETTING(0x0407, "look.echo_secondary_b_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_echo_b, set_look_echo_b),
    TILES_SETTING(0x0408, "look.echo_flash_ms", TILES_SETTING_UINT, {.u = 0}, {.u = 2000}, NULL, get_look_echo_flash,
                  set_look_echo_flash),
    /* MIDI */
    TILES_SETTING(0x0500, "midi.din_trs_type", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, DIN_TYPE_NAMES, get_din_type,
                  set_din_type),
    /* features */
    TILES_SETTING(0x0600, "features.melodic_harmonics", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_harmonics,
                  set_harmonics),
};
#define TABLE_COUNT (sizeof(TABLE) / sizeof(TABLE[0]))

/* "Hands off the pads" -- a flash erase stops the whole firmware for tens of milliseconds. */
static bool pads_idle(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            return false;
        }
    }
    return true;
}

void tiles_settings_boot(void) {
    tiles_settings_init(TABLE, TABLE_COUNT);
    tiles_settings_capture_defaults();
    if (!tiles_storage_settings_region_safe()) {
        printf("[settings] application image overlaps the settings flash region -- changes will NOT be saved\n");
        return;
    }
    tiles_settings_persist_init(tiles_storage_settings_ops(), pads_idle);
    tiles_settings_persist_info_t info = tiles_settings_persist_get_info();
    printf("[settings] %u settings; %s\n", (unsigned)TABLE_COUNT,
           info.loaded ? "saved snapshot restored" : "no saved snapshot (defaults)");
}

void tiles_settings_scan(void) {
    tiles_settings_persist_service(to_ms_since_boot(get_absolute_time()));
}
