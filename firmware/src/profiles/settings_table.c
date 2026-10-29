#include "settings_table.h"

#include "settings.h"
#include "settings_persist.h"

#include "board_pins.h"
#include "cv_gate.h"
#include "din_midi.h"
#include "expression.h"
#include "light_effects.h"
#include "lighting.h"
#include "midi_clock.h"
#include "midi_out.h"
#include "note_map.h"
#include "op_mode.h"
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

/* ---- light effect, layout ---- */
static tiles_setting_value_t get_play_effect(void) { return U((uint32_t)tiles_fx_get()); }
static void set_play_effect(tiles_setting_value_t v) { tiles_fx_set((tiles_fx_t)v.u); }
static tiles_setting_value_t get_layout(void) { return U((uint32_t)tiles_note_map_get_layout()); }
static void set_layout(tiles_setting_value_t v) { tiles_note_map_set_layout((tiles_melodic_layout_t)v.u); }
/* ---- MIDI clock ---- */
static tiles_setting_value_t get_clock_lead(void) { return U(tiles_midi_clock_get_lead_ms()); }
static void set_clock_lead(tiles_setting_value_t v) { tiles_midi_clock_set_lead_ms((uint16_t)v.u); }
/* 0 = parts on their own USB port (midi/midi_ports.h), 1 = on the MIDI port. */
static tiles_setting_value_t get_parts_port(void) { return U(tiles_midi_out_get_parts_separate() ? 0u : 1u); }
static void set_parts_port(tiles_setting_value_t v) { tiles_midi_out_set_parts_separate(v.u == 0u); }
static tiles_setting_value_t get_seq_lock(void) { return U(tiles_op_mode_get_phase_lock() ? 1u : 0u); }
static void set_seq_lock(tiles_setting_value_t v) { tiles_op_mode_set_phase_lock(v.u != 0u); }
/* ---- pedal ---- */
static tiles_setting_value_t get_pedal_mode(void) { return U((uint32_t)tiles_pedal_get_mode()); }
static void set_pedal_mode(tiles_setting_value_t v) { tiles_pedal_set_mode((tiles_pedal_mode_t)v.u); }
static tiles_setting_value_t get_pedal_polarity(void) { return U((uint32_t)tiles_pedal_get_polarity()); }
static void set_pedal_polarity(tiles_setting_value_t v) { tiles_pedal_set_polarity((tiles_pedal_polarity_t)v.u); }
static tiles_setting_value_t get_pedal_sustain_style(void) { return U((uint32_t)tiles_pedal_get_sustain_style()); }
static void set_pedal_sustain_style(tiles_setting_value_t v) {
    tiles_pedal_set_sustain_style((tiles_pedal_sustain_style_t)v.u);
}

/* ---- expression ---- */
static tiles_setting_value_t get_mpe(void) { return U(tiles_expression_is_mpe_enabled() ? 1u : 0u); }
static void set_mpe(tiles_setting_value_t v) { tiles_expression_set_mpe_enabled(v.u != 0u); }
static tiles_setting_value_t get_bend(void) { return F(tiles_expression_get_pitch_bend_sensitivity()); }
static void set_bend(tiles_setting_value_t v) { tiles_expression_set_pitch_bend_sensitivity(v.f); }
static tiles_setting_value_t get_aftertouch(void) { return U(tiles_expression_get_aftertouch_sensitivity()); }
static void set_aftertouch(tiles_setting_value_t v) { tiles_expression_set_aftertouch_sensitivity((uint16_t)v.u); }
static tiles_setting_value_t get_harmonics(void) { return U(tiles_expression_is_melodic_harmonics_enabled() ? 1u : 0u); }
static void set_harmonics(tiles_setting_value_t v) { tiles_expression_set_melodic_harmonics_enabled(v.u != 0u); }
static tiles_setting_value_t get_harm_arm(void) { return U(tiles_expression_get_harmonics_arm_ms()); }
static void set_harm_arm(tiles_setting_value_t v) { tiles_expression_set_harmonics_arm_ms((uint16_t)v.u); }
static tiles_setting_value_t get_harm_confirm(void) { return U(tiles_expression_get_harmonics_confirm_ms()); }
static void set_harm_confirm(tiles_setting_value_t v) { tiles_expression_set_harmonics_confirm_ms((uint16_t)v.u); }
static tiles_setting_value_t get_harm_press(void) { return U(tiles_expression_get_harmonics_press_depth()); }
static void set_harm_press(tiles_setting_value_t v) { tiles_expression_set_harmonics_press_depth((uint16_t)v.u); }

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
LOOK_ENTRY(echo_tint, TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT)
LOOK_ENTRY(echo_g, TILES_LOOK_ECHO_SECONDARY_G_PERCENT)
LOOK_ENTRY(echo_b, TILES_LOOK_ECHO_SECONDARY_B_PERCENT)
LOOK_ENTRY(echo_flash, TILES_LOOK_ECHO_FLASH_MS)
LOOK_ENTRY(pressure_hue, TILES_LOOK_PRESSURE_HUE_DEGREES)
LOOK_ENTRY(bend_dim, TILES_LOOK_BEND_DIM_PERCENT)
LOOK_ENTRY(led_intensity, TILES_LOOK_LED_INTENSITY_PERCENT)

/* ---- mode menu entries (op_mode.h) ---- */
#define MODES_ENTRY(name, entry)                                                                       \
    static tiles_setting_value_t get_modes_##name(void) {                                              \
        return U(tiles_op_mode_get_menu_entry_enabled(entry) ? 1u : 0u);                               \
    }                                                                                                  \
    static void set_modes_##name(tiles_setting_value_t v) { tiles_op_mode_set_menu_entry_enabled(entry, v.u != 0u); }
MODES_ENTRY(duet, TILES_MENU_DUET)
MODES_ENTRY(sequencer, TILES_MENU_SEQUENCER)
MODES_ENTRY(drums, TILES_MENU_DRUMS)
MODES_ENTRY(song, TILES_MENU_SONG)
MODES_ENTRY(prism, TILES_MENU_PRISM)
MODES_ENTRY(bass, TILES_MENU_BASS)
MODES_ENTRY(ableton, TILES_MENU_ABLETON)
MODES_ENTRY(volca, TILES_MENU_VOLCA)
MODES_ENTRY(bounce, TILES_MENU_BOUNCE)
MODES_ENTRY(cradle, TILES_MENU_CRADLE)
MODES_ENTRY(sand, TILES_MENU_SAND)
MODES_ENTRY(ripples, TILES_MENU_RIPPLES)
MODES_ENTRY(orbits, TILES_MENU_ORBITS)

/* ---- colour scheme and custom pad colours (lighting.h) ---- */
_Static_assert(TILES_LIGHTING_COLOR_NONE == TILES_COLOR_NONE, "lighting and settings must agree on 'none'");
#define SCHEME_ENTRY(name, role)                                                                    \
    static tiles_setting_value_t get_color_##name(void) { return U(tiles_lighting_get_scheme_color(role)); } \
    static void set_color_##name(tiles_setting_value_t v) { tiles_lighting_set_scheme_color(role, v.u); }
SCHEME_ENTRY(root, TILES_SCHEME_ROOT)
SCHEME_ENTRY(third, TILES_SCHEME_THIRD)
SCHEME_ENTRY(fifth, TILES_SCHEME_FIFTH)
SCHEME_ENTRY(note, TILES_SCHEME_NOTE)
SCHEME_ENTRY(accidental, TILES_SCHEME_ACCIDENTAL)
static tiles_setting_value_t get_custom_pads(void) { return U(tiles_lighting_get_custom_pads_enabled() ? 1u : 0u); }
static void set_custom_pads(tiles_setting_value_t v) { tiles_lighting_set_custom_pads_enabled(v.u != 0u); }
/* One getter/setter pair and one row per pad; n is the logical pad, text
 * its two-digit key suffix ("color.pad.01".."color.pad.24"). */
#define PAD_COLOR_ENTRY(n, text)                                                                    \
    static tiles_setting_value_t get_pad_##n(void) { return U(tiles_lighting_get_pad_color(n##u)); } \
    static void set_pad_##n(tiles_setting_value_t v) { tiles_lighting_set_pad_color(n##u, v.u); }
#define PAD_COLOR_ROW(n, text)                                                                      \
    TILES_SETTING(0x0710 + (n) - 1, "color.pad." text, TILES_SETTING_COLOR, {.u = 0}, {.u = 0}, NULL, get_pad_##n, \
                  set_pad_##n)
#define PAD_COLOR_LIST(X)                                                                           \
    X(1, "01") X(2, "02") X(3, "03") X(4, "04") X(5, "05") X(6, "06") X(7, "07") X(8, "08")          \
    X(9, "09") X(10, "10") X(11, "11") X(12, "12") X(13, "13") X(14, "14") X(15, "15") X(16, "16")   \
    X(17, "17") X(18, "18") X(19, "19") X(20, "20") X(21, "21") X(22, "22") X(23, "23") X(24, "24")
PAD_COLOR_LIST(PAD_COLOR_ENTRY)
#define PAD_COLOR_ROW_COMMA(n, text) PAD_COLOR_ROW(n, text),

/* ---- MIDI ---- */
static tiles_setting_value_t get_din_type(void) { return U((uint32_t)tiles_din_midi_get_trs_type()); }
static void set_din_type(tiles_setting_value_t v) { tiles_din_midi_set_trs_type((tiles_din_midi_trs_type_t)v.u); }

static const char *const PEDAL_MODE_NAMES[] = {"sustain", "expression"};
static const char *const PEDAL_POLARITY_NAMES[] = {"normally_open", "normally_closed"};
/* services/pedal.h's tiles_pedal_sustain_style_t: who holds released notes. */
static const char *const PEDAL_SUSTAIN_STYLE_NAMES[] = {"synth", "hold"};
static const char *const DIN_TYPE_NAMES[] = {"a", "b"};
static const char *const PARTS_PORT_NAMES[] = {"parts", "main"};
static const char *const SEQ_LOCK_NAMES[] = {"off", "bar"};
/* In light_effects.h order (the Light effects menu's pads 1-12). */
static const char *const PLAY_EFFECT_NAMES[] = {"off",          "afterglow",    "ripple",  "cross",
                                                "halo",         "warmth",       "prism_ripple", "touch_colour",
                                                "heatmap",      "fireworks",    "rise",    "ink"};
static const char *const LAYOUT_NAMES[] = {"rows", "columns", "fourths", "keys", "fifths", "tonnetz", "quarter_tones"};

/* THE TABLE. One row per setting. Rules:
 *  - `id` is permanent (it is what is written to flash and what a binary protocol will use): never
 *    reuse or renumber one -- retire it by leaving a gap. Ids are grouped by hundreds:
 *    0x01xx pedal, 0x02xx expression, 0x03xx CV/gate, 0x04xx LED look, 0x05xx MIDI, 0x06xx features,
 *    0x07xx colours, 0x08xx the mode menu's entries.
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
    TILES_SETTING(0x0102, "pedal.sustain_style", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, PEDAL_SUSTAIN_STYLE_NAMES,
                  get_pedal_sustain_style, set_pedal_sustain_style),
    /* expression */
    TILES_SETTING(0x0200, "expression.mpe_enabled", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_mpe, set_mpe),
    TILES_SETTING(0x0201, "expression.pitch_bend_sensitivity", TILES_SETTING_FLOAT, {.f = 0.001f}, {.f = 1.0f}, NULL,
                  get_bend, set_bend),
    TILES_SETTING(0x0202, "expression.aftertouch_sensitivity", TILES_SETTING_UINT, {.u = 1}, {.u = 65535}, NULL,
                  get_aftertouch, set_aftertouch),
    /* CV / gate (calibration limited to what the jack can use). The enable
     * switch is volatile: CV/gate boots OFF every time (services/cv_gate.h);
     * the calibration is saved. */
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
    /* 0x0401-0x0404 retired (natural/root/fifth levels and the fifth's red
     * tint): replaced by the colour scheme, color.* below. */
    TILES_SETTING(0x0405, "look.echo_sustain_tint_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_echo_tint, set_look_echo_tint),
    TILES_SETTING(0x0406, "look.echo_secondary_g_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_echo_g, set_look_echo_g),
    TILES_SETTING(0x0407, "look.echo_secondary_b_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_echo_b, set_look_echo_b),
    TILES_SETTING(0x0408, "look.echo_flash_ms", TILES_SETTING_UINT, {.u = 0}, {.u = 2000}, NULL, get_look_echo_flash,
                  set_look_echo_flash),
    /* Touched pads: hue turn at full pressure (degrees) and brightness taken
     * off at full bend (percent); 0 = off. */
    TILES_SETTING(0x0409, "look.pressure_hue_degrees", TILES_SETTING_UINT, {.u = 0}, {.u = 90}, NULL,
                  get_look_pressure_hue, set_look_pressure_hue),
    TILES_SETTING(0x040A, "look.bend_dim_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 90}, NULL,
                  get_look_bend_dim, set_look_bend_dim),
    /* Pads, underglow and button LEDs, percent of full (Feel menu row 4: 0,
     * 4, 16, 36, 64, 100); 0 = off. */
    TILES_SETTING(0x040B, "look.led_intensity_percent", TILES_SETTING_UINT, {.u = 0}, {.u = 100}, NULL,
                  get_look_led_intensity, set_look_led_intensity),
    /* The light effect while playing (Feel menu, circle + square). */
    TILES_SETTING(0x040C, "look.play_effect", TILES_SETTING_ENUM, {.u = 0}, {.u = TILES_FX_COUNT - 1u}, PLAY_EFFECT_NAMES,
                  get_play_effect, set_play_effect),
    /* MIDI */
    TILES_SETTING(0x0500, "midi.din_trs_type", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, DIN_TYPE_NAMES, get_din_type,
                  set_din_type),
    /* Following a DAW's clock, how many ms early pulses come through so the
     * notes land on its grid (services/midi_clock.h); 0 = off. */
    TILES_SETTING(0x0501, "midi.clock_lead_ms", TILES_SETTING_UINT, {.u = 0}, {.u = 50}, NULL, get_clock_lead,
                  set_clock_lead),
    /* Where the parts (sequencer lanes, drums, Volca, Bouncing notes, Song
     * tracks, chords, games) go over USB: their own "Parts" port, so a DAW's
     * MPE track on the MIDI port hears only the MPE zone; or the MIDI port. */
    TILES_SETTING(0x0502, "midi.parts_port", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, PARTS_PORT_NAMES, get_parts_port,
                  set_parts_port),
    /* features */
    TILES_SETTING(0x0600, "features.melodic_harmonics", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_harmonics,
                  set_harmonics),
    /* Chord-vs-harmonic tuning (services/expression.c HARMONIC_ARM_MS_DEFAULT):
     * ms after a strike before other touches may pluck; ms a touch waits before
     * plucking; raw Hall depth (rest 0, strike 150) that makes it a real press. */
    TILES_SETTING(0x0601, "features.harmonics.arm_ms", TILES_SETTING_UINT, {.u = 0}, {.u = 1000}, NULL, get_harm_arm,
                  set_harm_arm),
    TILES_SETTING(0x0602, "features.harmonics.confirm_ms", TILES_SETTING_UINT, {.u = 0}, {.u = 500}, NULL,
                  get_harm_confirm, set_harm_confirm),
    TILES_SETTING(0x0603, "features.harmonics.press_depth", TILES_SETTING_UINT, {.u = 1}, {.u = 1000}, NULL,
                  get_harm_press, set_harm_press),
    /* colour scheme (0xRRGGBB before the power ceiling, or "none"; lighting.h) */
    TILES_SETTING(0x0700, "color.root", TILES_SETTING_COLOR, {.u = 0}, {.u = 0}, NULL, get_color_root, set_color_root),
    TILES_SETTING(0x0701, "color.third", TILES_SETTING_COLOR, {.u = 0}, {.u = 0}, NULL, get_color_third,
                  set_color_third),
    TILES_SETTING(0x0702, "color.fifth", TILES_SETTING_COLOR, {.u = 0}, {.u = 0}, NULL, get_color_fifth,
                  set_color_fifth),
    TILES_SETTING(0x0703, "color.note", TILES_SETTING_COLOR, {.u = 0}, {.u = 0}, NULL, get_color_note, set_color_note),
    TILES_SETTING(0x0704, "color.accidental", TILES_SETTING_COLOR, {.u = 0}, {.u = 0}, NULL, get_color_accidental,
                  set_color_accidental),
    /* custom pad colours (advanced; melodic mode only) */
    /* Mode menu: which modes are on it (melodic always is). */
    TILES_SETTING(0x0800, "modes.duet", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_duet, set_modes_duet),
    TILES_SETTING(0x0801, "modes.sequencer", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_sequencer,
                  set_modes_sequencer),
    TILES_SETTING(0x0802, "modes.drums", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_drums, set_modes_drums),
    TILES_SETTING(0x0803, "modes.song", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_song, set_modes_song),
    TILES_SETTING(0x0804, "modes.prism", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_prism,
                  set_modes_prism),
    TILES_SETTING(0x0805, "modes.bass", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_bass, set_modes_bass),
    TILES_SETTING(0x0806, "modes.ableton", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_ableton,
                  set_modes_ableton),
    TILES_SETTING(0x0807, "modes.volca", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_volca, set_modes_volca),
    TILES_SETTING(0x0808, "modes.bounce", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_bounce,
                  set_modes_bounce),
    TILES_SETTING(0x0809, "modes.cradle", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_cradle,
                  set_modes_cradle),
    TILES_SETTING(0x080A, "modes.sand", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_sand, set_modes_sand),
    TILES_SETTING(0x080B, "modes.ripples", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_ripples,
                  set_modes_ripples),
    TILES_SETTING(0x080C, "modes.orbits", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_modes_orbits,
                  set_modes_orbits),
    /* Melodic mode's layout (Feel menu, circle + triangle; note_map.h). */
    TILES_SETTING(0x0900, "layout.melodic", TILES_SETTING_ENUM, {.u = 0}, {.u = TILES_NUM_LAYOUTS - 1u}, LAYOUT_NAMES,
                  get_layout, set_layout),
    /* Phase lock between sequencer lanes and the drums (op_mode.h); no
     * hardware gesture. */
    TILES_SETTING(0x0A00, "seq.lock", TILES_SETTING_ENUM, {.u = 0}, {.u = 1}, SEQ_LOCK_NAMES, get_seq_lock,
                  set_seq_lock),
    TILES_SETTING(0x0705, "color.custom_pads", TILES_SETTING_BOOL, {.u = 0}, {.u = 1}, NULL, get_custom_pads,
                  set_custom_pads),
    PAD_COLOR_LIST(PAD_COLOR_ROW_COMMA)
};
#define TABLE_COUNT (sizeof(TABLE) / sizeof(TABLE[0]))

/* "Hands off the pads": a flash erase stalls the firmware for tens of ms. */
/* Settings save (an erase: tens of ms of everything stalled) with the pads
 * idle and the clock stopped, so playing never hiccups; a change made
 * mid-song lands once it stops. */
static bool pads_idle(void) {
    if (tiles_midi_clock_is_running()) {
        return false;
    }
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
