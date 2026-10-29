#include "lighting.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "crash_indicator.h"
#include "debug_mode.h"
#include "expression.h"
#include "expression_control.h"
#include "game_mode.h"
#include "hall.h"
#include "light_effects.h"
#include "note_map.h"
#include "op_mode.h"
#include "pad_config.h"
#include "power.h"
#include "standby.h"
#include "touch.h"

#include "pico/time.h"

#include "sk6805.h"
#include "tca9554.h"

#include <math.h>
#include <stdio.h>

/* Shared "visible but not the active thing" level: a pressed pad's floor,
 * bass frets, the chord strip. 50 matches the menus' available-item level
 * (OP_SCALE_AVAILABLE_LEVEL), so resting brightness is one standard
 * (raised 10 -> 25 -> 50 over "too dim" feedback). A fraction of the power
 * ceiling, like every level here. */
#define TILES_LIGHTING_IDLE_BASELINE_PERCENT 50u

/* Default colour scheme: the original melodic look, as colours.
 *   - root: Sentia magenta (#FF00FF) at 40% -> 0x660066;
 *   - fifth: violet, blue with 35% red (hue ~261 deg vs root 300, blue
 *     240), at 40% -> 0x240066;
 *   - other notes: white at 21% ("dim 30% the white", after 30) ->
 *     0x363636, below root and fifth so the landmarks and the echo stand out;
 *   - accidentals (sharps/black keys) dark: the one exception to "pads never
 *     go fully dark";
 *   - third: no highlight (a third landmark was tried and dropped; the
 *     app can turn it on).
 * Brightness order to keep: pressed (100% white) > echo (100% of its
 * colour, flash to white) > root and fifth > other notes. */
#define TILES_LIGHTING_SCHEME_ROOT 0x660066u
#define TILES_LIGHTING_SCHEME_THIRD TILES_LIGHTING_COLOR_NONE
#define TILES_LIGHTING_SCHEME_FIFTH 0x240066u
#define TILES_LIGHTING_SCHEME_NOTE 0x363636u
#define TILES_LIGHTING_SCHEME_ACCIDENTAL 0x000000u

/* Melodic echo (op_mode.c "live echo of an incoming melody"). The color is
 * already full-scale, and the power ceiling isn't raised, so it gets
 * brighter within the ceiling instead:
 *   - a little red and blue mixed into the green
 *     (TILES_LIGHTING_ECHO_SUSTAIN_TINT) uses all three dies: a paler,
 *     brighter green, still clearly not a pressed pad's white;
 *   - an onset flash from full white easing to that green over
 *     TILES_LIGHTING_ECHO_FLASH_MS, so every note lands visibly. White at
 *     the ceiling is what a pressed pad shows, so it's within budget.
 * Unmeasured. */
#define TILES_LIGHTING_ECHO_SUSTAIN_TINT 0.35f
#define TILES_LIGHTING_ECHO_FLASH_MS 180u

/* Second TILES DISPLAY (echo layer 1): the same onset flash, settling on a
 * soft warm red, R full with small G and B. Blue stays below green so it
 * reads coral, not pink (pink is the root's color); too much green drifts
 * orange (Song capture's color). Unmeasured; these two are the knobs. */
#define TILES_LIGHTING_ECHO_SECONDARY_G 0.18f
#define TILES_LIGHTING_ECHO_SECONDARY_B 0.12f

/* ---- Pressure hue and bend dimming, on every touched pad ------------------
 * The last step before a pad is written, in every mode (not games, the
 * sleep animation or the bench test), so a pad answers the same way
 * whatever colour it shows:
 *   - Pressure turns the pad's colour toward Sentia blue: its hue moves
 *     toward PRESSURE_TARGET_HUE_DEG by up to
 *     TILES_LIGHTING_PRESSURE_HUE_DEGREES at full pressure, the short way
 *     round and never past it, with saturation and brightness (HSV value)
 *     kept, so nothing gets brighter or draws more current. White and greys
 *     have no hue to turn; they take on a tint of that blue instead, up to
 *     PRESSURE_NEUTRAL_SAT saturation (partway: "not fully"), blended in as
 *     a colour's own saturation falls below PRESSURE_NEUTRAL_BELOW_SAT (no
 *     jump between the two). Pressed white used to go toward Sentia
 *     magenta; blue reads better against the magenta landmarks.
 *     Pressure is the pad's Hall depth past the strike threshold, scaled to
 *     expression.aftertouch_sensitivity like MIDI pressure, so the light
 *     follows what the note does. Smoothed a little (PRESSURE_SMOOTHING).
 *     MPE only: with MPE off, pressure is one channel-wide value, not the
 *     pad's own, so the pads don't show it.
 *   - A bending note's pad swells darker as it bends: brightness falls
 *     with the bend (expression.c's amount, 0..1 of the pad's full bend)
 *     to 1 - TILES_LIGHTING_BEND_DIM at full bend, eased over
 *     BEND_SWELL_MS each way so it swells rather than steps. A flicker
 *     (6-24 Hz) was tried first; a smooth dim reads better. It never goes
 *     dark.
 * Pads showing either are rewritten every PAD_LIVE_REFRESH_MS. They take
 * the pass's one pad write from the round-robin, which waits: a pad write
 * is ~0.3 ms (two mux writes on I2C plus the 120 us latch; unmeasured). All first guesses:
 * "slight, but visible". The amounts are settings
 * (look.pressure_hue_degrees, look.bend_dim_percent); 0 turns either off. */
#define TILES_LIGHTING_PRESSURE_HUE_DEGREES 40u
#define TILES_LIGHTING_BEND_DIM 0.5f
/* Sentia blue: the brand periwinkle #94ABF9 (quick start guide, web
 * player), hue ~226 degrees. */
#define PRESSURE_TARGET_HUE_DEG 226.0f
/* Neutral tint saturation at full pressure, for the default hue amount
 * (scaled with look.pressure_hue_degrees, so one setting moves both). */
#define PRESSURE_NEUTRAL_SAT 0.45f
#define PRESSURE_NEUTRAL_BELOW_SAT 0.15f
#define PRESSURE_SMOOTHING 0.3f
#define BEND_SWELL_MS 60.0f
#define PAD_LIVE_REFRESH_MS 12u
#define PAD_FX_EPSILON 0.005f

/* The constants above are the DEFAULTS of the look.* settings. s_look[] is
 * what pad_desired_rgb() reads; settings can change it live over USB and
 * save it to flash. Stored in whole percent (tints: 0.35 -> 35). The power
 * ceiling is applied after all of this and is not a setting, so no setting
 * can raise LED current. */
#define LOOK_PCT(fraction) ((uint16_t)((fraction) * 100.0f + 0.5f))
static uint16_t s_look[TILES_LOOK_COUNT] = {
    [TILES_LOOK_IDLE_BASELINE_PERCENT] = TILES_LIGHTING_IDLE_BASELINE_PERCENT,
    [TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT] = LOOK_PCT(TILES_LIGHTING_ECHO_SUSTAIN_TINT),
    [TILES_LOOK_ECHO_SECONDARY_G_PERCENT] = LOOK_PCT(TILES_LIGHTING_ECHO_SECONDARY_G),
    [TILES_LOOK_ECHO_SECONDARY_B_PERCENT] = LOOK_PCT(TILES_LIGHTING_ECHO_SECONDARY_B),
    [TILES_LOOK_ECHO_FLASH_MS] = TILES_LIGHTING_ECHO_FLASH_MS,
    [TILES_LOOK_PRESSURE_HUE_DEGREES] = TILES_LIGHTING_PRESSURE_HUE_DEGREES,
    [TILES_LOOK_BEND_DIM_PERCENT] = LOOK_PCT(TILES_LIGHTING_BEND_DIM),
    [TILES_LOOK_LED_INTENSITY_PERCENT] = 100u,
};

#define TILES_LOOK_MAX_FLASH_MS 2000u

static float look_fraction(tiles_look_param_t param) {
    return (float)s_look[param] / 100.0f;
}

static uint32_t s_scheme[TILES_SCHEME_COUNT] = {
    [TILES_SCHEME_ROOT] = TILES_LIGHTING_SCHEME_ROOT,
    [TILES_SCHEME_THIRD] = TILES_LIGHTING_SCHEME_THIRD,
    [TILES_SCHEME_FIFTH] = TILES_LIGHTING_SCHEME_FIFTH,
    [TILES_SCHEME_NOTE] = TILES_LIGHTING_SCHEME_NOTE,
    [TILES_SCHEME_ACCIDENTAL] = TILES_LIGHTING_SCHEME_ACCIDENTAL,
};
static bool s_custom_pads_enabled;
static uint32_t s_pad_color[TILES_NUM_PADS]; /* TILES_LIGHTING_COLOR_NONE = use the scheme */
static bool s_pad_colors_initialized;

static bool color_valid(uint32_t c) {
    return c <= 0xFFFFFFu || c == TILES_LIGHTING_COLOR_NONE;
}

static void init_pad_colors(void) {
    if (!s_pad_colors_initialized) {
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            s_pad_color[i] = TILES_LIGHTING_COLOR_NONE;
        }
        s_pad_colors_initialized = true;
    }
}

void tiles_lighting_set_scheme_color(tiles_scheme_role_t role, uint32_t rgb_or_none) {
    if (role < TILES_SCHEME_COUNT && color_valid(rgb_or_none)) {
        s_scheme[role] = rgb_or_none;
    }
}

uint32_t tiles_lighting_get_scheme_color(tiles_scheme_role_t role) {
    return role < TILES_SCHEME_COUNT ? s_scheme[role] : TILES_LIGHTING_COLOR_NONE;
}

void tiles_lighting_set_custom_pads_enabled(bool enabled) {
    s_custom_pads_enabled = enabled;
}

bool tiles_lighting_get_custom_pads_enabled(void) {
    return s_custom_pads_enabled;
}

void tiles_lighting_set_pad_color(uint8_t logical_pad, uint32_t rgb_or_none) {
    init_pad_colors();
    if (logical_pad >= 1u && logical_pad <= TILES_NUM_PADS && color_valid(rgb_or_none)) {
        s_pad_color[logical_pad - 1u] = rgb_or_none;
    }
}

uint32_t tiles_lighting_get_pad_color(uint8_t logical_pad) {
    init_pad_colors();
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return TILES_LIGHTING_COLOR_NONE;
    }
    return s_pad_color[logical_pad - 1u];
}

/* The scheme colour for a melodic pad, by role priority; NONE skips a role. */
static uint32_t scheme_color_for_pad(uint8_t logical_pad) {
    if (s_scheme[TILES_SCHEME_ROOT] != TILES_LIGHTING_COLOR_NONE && tiles_note_map_is_root_pad(logical_pad)) {
        return s_scheme[TILES_SCHEME_ROOT]; /* first: a root can be a sharp in some keys, and root wins */
    }
    if (s_scheme[TILES_SCHEME_FIFTH] != TILES_LIGHTING_COLOR_NONE && tiles_note_map_is_fifth_pad(logical_pad)) {
        return s_scheme[TILES_SCHEME_FIFTH];
    }
    if (s_scheme[TILES_SCHEME_THIRD] != TILES_LIGHTING_COLOR_NONE && tiles_note_map_is_third_pad(logical_pad)) {
        return s_scheme[TILES_SCHEME_THIRD];
    }
    if (s_scheme[TILES_SCHEME_ACCIDENTAL] != TILES_LIGHTING_COLOR_NONE && !tiles_note_map_is_natural_pad(logical_pad)) {
        return s_scheme[TILES_SCHEME_ACCIDENTAL];
    }
    return s_scheme[TILES_SCHEME_NOTE] != TILES_LIGHTING_COLOR_NONE ? s_scheme[TILES_SCHEME_NOTE] : 0x000000u;
}

void tiles_lighting_melodic_rest_rgb(uint8_t logical_pad, float rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (!tiles_note_map_pad_has_note(logical_pad)) {
        return;
    }
    if (tiles_note_map_get_pad_cents(logical_pad) != 0u) {
        float level = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT) * 0.35f;
        rgb[0] = 0.58f * level;
        rgb[1] = 0.67f * level;
        rgb[2] = 0.98f * level;
        return;
    }
    uint32_t c = scheme_color_for_pad(logical_pad);
    rgb[0] = (float)((c >> 16) & 0xFFu) / 255.0f;
    rgb[1] = (float)((c >> 8) & 0xFFu) / 255.0f;
    rgb[2] = (float)(c & 0xFFu) / 255.0f;
}

uint16_t tiles_lighting_get_look(tiles_look_param_t param) {
    return param < TILES_LOOK_COUNT ? s_look[param] : 0u;
}

static void write_underglow(void);
static bool s_initialized;

void tiles_lighting_set_look(tiles_look_param_t param, uint16_t value) {
    if (param >= TILES_LOOK_COUNT) {
        return;
    }
    uint16_t max = param == TILES_LOOK_ECHO_FLASH_MS          ? TILES_LOOK_MAX_FLASH_MS
                   : param == TILES_LOOK_PRESSURE_HUE_DEGREES ? 90u
                                                               : 100u;
    uint16_t old = s_look[param];
    s_look[param] = value > max ? max : value;
    /* Pads pick a new intensity up on the round-robin; the underglow is only
     * written on change, so push it now. */
    if (param == TILES_LOOK_LED_INTENSITY_PERCENT && s_look[param] != old && s_initialized) {
        write_underglow();
    }
}

/* LED intensity (Feel menu row 4) as a factor, 1 while the Feel menu is
 * open so it stays readable at "off". Applies to the pads, the normal
 * underglow and the button LEDs (handed to buttons.c each pass), not to
 * the underglow indicators (crash, debug, captures). */
static float led_intensity(void) {
    if (tiles_expression_control_owns_pad_grid()) {
        return 1.0f;
    }
    return look_fraction(TILES_LOOK_LED_INTENSITY_PERCENT);
}

/* Underglow's own fixed brightness (out of 255), NOT scaled by the power
 * ceiling: at the USB ceiling it barely glowed. 4 LEDs are a small
 * share of the budget (included in the accounting below), and a steady
 * halo doesn't compete with pad feedback. */
#define TILES_LIGHTING_UNDERGLOW_LEVEL 230u

#define TILES_LIGHTING_NUM_UNDERGLOW_PIXELS 4u

/* ---- Pad brightness ceiling: static, deliberately -------------------------
 * One flat ceiling from the power mode, never recomputed from how many
 * pads are lit (a load-aware ceiling made the whole board's brightness
 * shift with playing, which looks like a brownout).
 *
 * Budget, measured on unit 4 over USB with an inline meter (2026-10-06,
 * docs/hardware/current-measurements.md; tools/current_test.py):
 *   - Board without LEDs or motors (MCU, sensors, ICs, button LEDs):
 *     ~80 mA (the datasheet estimate was ~220 mA).
 *   - LEDs: ~4.1 mA per percent with all 28 white, ~0.41 A at 100%
 *     (~14.6 mA per pixel; the board map's model said 16).
 *   - Motors: ~28 mA each running at full duty, ~0.11 A for 4 (estimated
 *     60-100 mA each). Start-up surges are short and not captured.
 * Worst case at the USB ceiling (50%): 80 + 205 (pads) + ~24 (underglow at
 * its fixed level) + 110 (4 motors) = ~0.42 A of 500 mA. External (2500
 * mA) at 90%: ~0.6 A. Normal playing measured 0.15-0.35 A on USB (37%). */
static uint8_t static_ceiling_level(void) {
    uint8_t ceiling_percent = tiles_power_get_state().led_brightness_ceiling_percent;
    return (uint8_t)((255u * ceiling_percent) / 100u);
}

typedef struct {
    float r;
    float g;
    float b;
} tiles_rgb01_t;

static tiles_rgb01_t rgb01_from_color(uint32_t c) {
    return (tiles_rgb01_t){(float)((c >> 16) & 0xFFu) / 255.0f, (float)((c >> 8) & 0xFFu) / 255.0f,
                           (float)(c & 0xFFu) / 255.0f};
}

static tiles_sk6805_chain_t s_underglow_chain;
static tiles_sk6805_chain_t s_pad_chain;
static tiles_tca9554_t s_led_mux;
static float s_pad_press[TILES_NUM_PADS]; /* touch-driven, white, floored at the idle level */
static tiles_rgb01_t s_pad_standby_rgb[TILES_NUM_PADS]; /* standby color, no floor */
/* What each pad last showed of it (valid = written since standby began):
 * standby colours go out STANDBY_WRITES_PER_PASS pads a pass, biggest
 * change first, so no screen change (a menu opening, a game frame, the
 * screensaver) sends 24 pads at once and holds the loop (and the
 * playback ticks) ~7 ms. */
static tiles_rgb01_t s_standby_written[TILES_NUM_PADS];
static bool s_standby_written_valid[TILES_NUM_PADS];
#define STANDBY_WRITES_PER_PASS 3u
static tiles_rgb01_t s_underglow_rgb[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
/* The underglow outside standby: the active mode's colour (white until set). */
static tiles_rgb01_t s_mode_underglow = {1.0f, 1.0f, 1.0f};
static uint8_t s_service_cursor;
/* Bench test white level (0-255), 0 = off. See tiles_lighting_set_test_white(). */
static uint8_t s_test_white_level;
static uint8_t s_test_white_percent;
static bool s_standby_active;
/* Whether a crash/debug/pattern/capture override owned the underglow last
 * frame (restore on release; see tiles_lighting_service()). */
static bool s_underglow_override_was_active;
/* Pressure hue / bend dimming state per pad (see "Pressure hue" above). */
static float s_pad_pressure[TILES_NUM_PADS];     /* 0..1, smoothed */
static float s_pad_bend[TILES_NUM_PADS];         /* 0..1, eased (BEND_SWELL_MS) */
static bool s_pad_fx_shown[TILES_NUM_PADS];      /* last write carried an effect */
static uint32_t s_pad_written_ms[TILES_NUM_PADS];
static uint32_t s_fx_updated_ms;
static uint8_t s_live_cursor;
/* The playing light effect (light_effects.h) as each pad was last written,
 * so a pass sends the pads whose effect changed most. */
static float s_play_fx_written[TILES_NUM_PADS][3];
#define PLAY_FX_WRITES_PER_PASS 2u /* a pad write is ~0.3 ms */
#define PLAY_FX_CHANGE_MIN 0.02f
/* The standby frame is a step grid: the effect draws over it
 * (tiles_lighting_set_grid_effects()). */
static bool s_grid_fx;

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* Fraction of TILES_LIGHTING_UNDERGLOW_LEVEL. No floor: underglow and
 * standby colors may be true black (animations need it). */
static uint8_t underglow_channel_level(float channel_0_to_1) {
    return (uint8_t)((float)TILES_LIGHTING_UNDERGLOW_LEVEL * clamp01(channel_0_to_1));
}

static void rgb_to_hsv(tiles_rgb01_t c, float *h, float *s, float *v) {
    float mx = fmaxf(c.r, fmaxf(c.g, c.b));
    float d = mx - fminf(c.r, fminf(c.g, c.b));
    *v = mx;
    *s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        *h = 0.0f;
    } else if (mx == c.r) {
        *h = 60.0f * fmodf((c.g - c.b) / d + 6.0f, 6.0f);
    } else if (mx == c.g) {
        *h = 60.0f * ((c.b - c.r) / d + 2.0f);
    } else {
        *h = 60.0f * ((c.r - c.g) / d + 4.0f);
    }
}

static tiles_rgb01_t hsv_to_rgb(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    tiles_rgb01_t out;
    switch ((int)(h / 60.0f)) {
    case 0:
        out = (tiles_rgb01_t){c, x, 0.0f};
        break;
    case 1:
        out = (tiles_rgb01_t){x, c, 0.0f};
        break;
    case 2:
        out = (tiles_rgb01_t){0.0f, c, x};
        break;
    case 3:
        out = (tiles_rgb01_t){0.0f, x, c};
        break;
    case 4:
        out = (tiles_rgb01_t){x, 0.0f, c};
        break;
    default:
        out = (tiles_rgb01_t){c, 0.0f, x};
        break;
    }
    return (tiles_rgb01_t){out.r + m, out.g + m, out.b + m};
}

/* Pressure 0..1 from Hall depth: past the strike threshold, full at the
 * aftertouch full scale (as MIDI pressure). */
static float pressure_from_depth(uint16_t depth) {
    float floor = TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA;
    float full = (float)tiles_expression_get_aftertouch_sensitivity();
    if (full < floor + 1.0f) {
        full = floor + 1.0f;
    }
    return clamp01(((float)depth - floor) / (full - floor));
}

static bool pad_fx_allowed(void) {
    return s_test_white_level == 0u && !tiles_game_mode_is_active() && !tiles_standby_is_active() &&
           !tiles_standby_is_deep_sleep();
}

static bool pad_fx_active(uint8_t pad_index) {
    return pad_fx_allowed() && (s_pad_pressure[pad_index] > PAD_FX_EPSILON || s_pad_bend[pad_index] > 0.0f);
}

/* Once per pass: each pad's pressure (touched pads only) and eased bend. */
static void update_pad_fx(uint32_t now_ms) {
    float dt_s = (float)(now_ms - s_fx_updated_ms) / 1000.0f;
    s_fx_updated_ms = now_ms;
    if (dt_s > 0.1f) {
        dt_s = 0.1f;
    }
    float swell = 1.0f - expf(-dt_s * 1000.0f / BEND_SWELL_MS); /* per-pass easing */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        uint8_t pad = (uint8_t)(i + 1u);
        if (tiles_touch_is_touched(pad) && tiles_expression_is_mpe_enabled()) {
            float target = pressure_from_depth(tiles_hall_get_depth(pad));
            s_pad_pressure[i] += PRESSURE_SMOOTHING * (target - s_pad_pressure[i]);
        } else {
            s_pad_pressure[i] = 0.0f;
        }
        float bend = tiles_expression_get_pad_bend_amount(pad);
        s_pad_bend[i] += swell * (bend - s_pad_bend[i]);
        if (bend == 0.0f && s_pad_bend[i] < 0.002f) {
            s_pad_bend[i] = 0.0f;
        }
    }
}

/* The pressure hue and bend dimming on top of a pad's colour. */
static tiles_rgb01_t apply_pad_fx(uint8_t pad_index, tiles_rgb01_t c) {
    if (!pad_fx_active(pad_index)) {
        return c;
    }
    float hue_degrees = (float)s_look[TILES_LOOK_PRESSURE_HUE_DEGREES];
    float p = s_pad_pressure[pad_index];
    if (p > PAD_FX_EPSILON && hue_degrees > 0.0f) {
        float h, s, v;
        rgb_to_hsv((tiles_rgb01_t){clamp01(c.r), clamp01(c.g), clamp01(c.b)}, &h, &s, &v);
        /* Toward the target hue, the short way, never past it. */
        float gap = fmodf(PRESSURE_TARGET_HUE_DEG - h + 540.0f, 360.0f) - 180.0f; /* -180..180 */
        float turn = fminf(fabsf(gap), p * hue_degrees);
        tiles_rgb01_t turned = hsv_to_rgb(h + (gap < 0.0f ? -turn : turn), s, v);
        float neutral = 1.0f - clamp01(s / PRESSURE_NEUTRAL_BELOW_SAT);
        if (neutral > 0.0f) {
            float tint_sat = clamp01(p * PRESSURE_NEUTRAL_SAT * hue_degrees / (float)TILES_LIGHTING_PRESSURE_HUE_DEGREES);
            tiles_rgb01_t tint = hsv_to_rgb(PRESSURE_TARGET_HUE_DEG, tint_sat, v);
            turned.r += neutral * (tint.r - turned.r);
            turned.g += neutral * (tint.g - turned.g);
            turned.b += neutral * (tint.b - turned.b);
        }
        c = turned;
    }
    float dim = look_fraction(TILES_LOOK_BEND_DIM_PERCENT) * s_pad_bend[pad_index];
    if (dim > 0.0f) {
        float level = 1.0f - dim;
        c.r *= level;
        c.g *= level;
        c.b *= level;
    }
    return c;
}

static bool grid_fx_on(void) {
    return s_grid_fx && s_standby_active && tiles_fx_get() != TILES_FX_OFF &&
           !tiles_expression_control_owns_pad_grid();
}

/* Under standby: the frame's colour, with a step grid's light effect
 * screened over it. */
static tiles_rgb01_t standby_pad_rgb(uint8_t pad_index, uint32_t now_ms) {
    tiles_rgb01_t c = s_pad_standby_rgb[pad_index];
    float fx[3];
    if (grid_fx_on() && tiles_fx_pad_light((uint8_t)(pad_index + 1u), now_ms, fx)) {
        c.r = 1.0f - (1.0f - clamp01(c.r)) * (1.0f - fx[0]);
        c.g = 1.0f - (1.0f - clamp01(c.g)) * (1.0f - fx[1]);
        c.b = 1.0f - (1.0f - clamp01(c.b)) * (1.0f - fx[2]);
    }
    return c;
}

/* A pad's resting look (no touch, echo or capture): bass frets, the chord
 * strip, layout gaps and quarter tones, custom colours, the scheme. */
static tiles_rgb01_t pad_idle_rgb(uint8_t logical_pad) {
    /* Bass guitar mode: neck-style coloring in the mode's lilac
     * (TILES_LIGHTING_BASS_*). Unmarked frets at the idle level; inlay frets
     * brighter; octave (double-dot) frets brightest. */
    if (tiles_note_map_is_guitar_mode_active()) {
        bool is_octave = false;
        float level = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT);
        if (tiles_note_map_is_guitar_fret_marker_pad(logical_pad, &is_octave)) {
            level = is_octave ? 1.0f : 0.75f;
        }
        return (tiles_rgb01_t){level * TILES_LIGHTING_BASS_R, level * TILES_LIGHTING_BASS_G,
                               level * TILES_LIGHTING_BASS_B};
    }

    /* Chord mode's chord strip: one solid blue for the whole region. The
     * melody grid needs no branch (the root/natural checks are chord-aware). */
    if (tiles_note_map_is_chord_region_pad(logical_pad)) {
        float level = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT);
        return (tiles_rgb01_t){0.0f, 0.0f, level};
    }

    /* Layouts (note_map.h): a pad with no note (Keys' gaps) is dark; a
     * quarter tone a dim Sentia blue, apart from the semitones. */
    if (!tiles_note_map_pad_has_note(logical_pad)) {
        return (tiles_rgb01_t){0.0f, 0.0f, 0.0f};
    }
    if (tiles_note_map_get_pad_cents(logical_pad) != 0u) {
        float level = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT) * 0.35f;
        return (tiles_rgb01_t){0.58f * level, 0.67f * level, 0.98f * level};
    }

    /* Custom pad colours (advanced), melodic mode only: the map is drawn for
     * melodic mode's layout, so chord mode's melody grid keeps the scheme. */
    if (s_custom_pads_enabled && !tiles_note_map_is_chord_mode_active()) {
        uint32_t own = tiles_lighting_get_pad_color(logical_pad);
        if (own != TILES_LIGHTING_COLOR_NONE) {
            return rgb01_from_color(own);
        }
    }

    /* Idle melodic colouring by note role: the colour scheme. */
    return rgb01_from_color(scheme_color_for_pad(logical_pad));
}

/* What a pad's r/g/b wants (0.0-1.0) in every state, before the ceiling
 * (applied in write_pad()) and the pressure hue / bend dimming
 * (apply_pad_fx()). The single source of truth for a pad's look. */
static tiles_rgb01_t pad_look_rgb(uint8_t pad_index) {
    if (s_standby_active) {
        return standby_pad_rgb(pad_index, to_ms_since_boot(get_absolute_time()));
    }
    if (s_pad_press[pad_index] > 0.0f) {
        /* Touched pad: floored at the idle level, never dark. */
        float baseline = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT);
        float level = baseline + (1.0f - baseline) * clamp01(s_pad_press[pad_index]);
        return (tiles_rgb01_t){level, level, level};
    }
    uint8_t logical_pad = (uint8_t)(pad_index + 1u);

    /* Song capture running: flash the pad the currently sounding captured note
     * lives on, over whatever mode is showing. After the live-touch check
     * (a real touch wins), before the idle looks. Not for chord-strip pads,
     * whose get_note() isn't a real note. (There is no "current step" marker
     * for Song's 128 steps yet; see op_mode.h.) */
    if (tiles_op_mode_song_capture_is_active() && !tiles_note_map_is_chord_region_pad(logical_pad) &&
        tiles_op_mode_song_capture_is_note_sounding(tiles_note_map_get_note(logical_pad))) {
        return (tiles_rgb01_t){1.0f, 0.6f, 0.0f};
    }

    /* Melodic echo: incoming notes (from TILES DISPLAY; see
     * daw-integration/README.md) light a pale green, flashing in from white
     * (the bass frets' green is deeper and never flashes). After the
     * live-touch check, before the idle looks. */
    uint8_t echo_note = tiles_note_map_get_note(logical_pad);
    bool echo_primary = tiles_op_mode_incoming_note_is_sounding(0, echo_note);
    bool echo_secondary = tiles_op_mode_incoming_note_is_sounding(1, echo_note);
    if (echo_primary || echo_secondary) {
        /* Both layers on the same note (rare): show the most recent hit so its
         * flash isn't hidden. */
        bool secondary = echo_secondary && (!echo_primary || tiles_op_mode_incoming_note_age_ms(1, echo_note) <
                                                              tiles_op_mode_incoming_note_age_ms(0, echo_note));
        /* White at onset easing to pale green (primary) or soft red (secondary):
         * the layer's own channel stays full, the others ease from full down to
         * their settled tint. */
        uint8_t layer = secondary ? 1u : 0u;
        uint32_t age_ms = tiles_op_mode_incoming_note_age_ms(layer, echo_note);
        float settle = 1.0f; /* 0 = just hit, 1 = settled */
        uint32_t flash_ms = s_look[TILES_LOOK_ECHO_FLASH_MS];
        if (age_ms < flash_ms) { /* flash_ms == 0 disables the flash (and guards the divide) */
            settle = (float)age_ms / (float)flash_ms;
        }
        if (secondary) {
            return (tiles_rgb01_t){1.0f, 1.0f + settle * (look_fraction(TILES_LOOK_ECHO_SECONDARY_G_PERCENT) - 1.0f),
                                   1.0f + settle * (look_fraction(TILES_LOOK_ECHO_SECONDARY_B_PERCENT) - 1.0f)};
        }
        float mix = 1.0f + settle * (look_fraction(TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT) - 1.0f);
        return (tiles_rgb01_t){mix, 1.0f, mix};
    }

    /* Under a light effect the resting look dims, so the effect's light
     * has room above it. */
    tiles_rgb01_t idle = pad_idle_rgb(logical_pad);
    if (tiles_fx_get() != TILES_FX_OFF) {
        idle.r *= TILES_LIGHTING_FX_REST_LEVEL;
        idle.g *= TILES_LIGHTING_FX_REST_LEVEL;
        idle.b *= TILES_LIGHTING_FX_REST_LEVEL;
    }
    return idle;
}

/* The effect's light on a pad now: none under standby (menus and the modes
 * that draw the whole grid) or with no effect picked. */
static bool play_fx_light(uint8_t pad_index, uint32_t now_ms, float rgb[3]) {
    if (s_standby_active || tiles_fx_get() == TILES_FX_OFF) {
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        return false;
    }
    return tiles_fx_pad_light((uint8_t)(pad_index + 1u), now_ms, rgb);
}

/* The look with the playing light effect over it. A held pad stays white
 * (Touch colour tints it); the rest get the effect's light blended over
 * (screen: light over light). Records what was drawn. */
static tiles_rgb01_t pad_desired_rgb(uint8_t pad_index) {
    float fx[3];
    bool lit = play_fx_light(pad_index, to_ms_since_boot(get_absolute_time()), fx);
    for (uint8_t c = 0; c < 3u; c++) {
        s_play_fx_written[pad_index][c] = fx[c];
    }
    tiles_rgb01_t look = pad_look_rgb(pad_index);
    if (s_standby_active || tiles_fx_get() == TILES_FX_OFF) {
        return look;
    }
    float tint[3];
    if (s_pad_press[pad_index] > 0.0f) {
        if (tiles_fx_pressed_colour((uint8_t)(pad_index + 1u), tint)) {
            return (tiles_rgb01_t){look.r * tint[0], look.g * tint[1], look.b * tint[2]};
        }
        return look;
    }
    if (lit) {
        look.r = 1.0f - (1.0f - clamp01(look.r)) * (1.0f - fx[0]);
        look.g = 1.0f - (1.0f - clamp01(look.g)) * (1.0f - fx[1]);
        look.b = 1.0f - (1.0f - clamp01(look.b)) * (1.0f - fx[2]);
    }
    return look;
}

static void write_pad(uint8_t pad_index);

/* Under standby: up to STANDBY_WRITES_PER_PASS pads whose standby colour
 * (with a step grid's effect) differs most from what they show (one never
 * written since standby began goes first). True if it wrote any. */
static bool service_standby_frame(uint32_t now_ms) {
    if (grid_fx_on()) {
        tiles_fx_step(now_ms);
    }
    tiles_rgb01_t target[TILES_NUM_PADS];
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        target[i] = standby_pad_rgb(i, now_ms);
    }
    bool any = false;
    for (uint8_t n = 0; n < STANDBY_WRITES_PER_PASS; n++) {
        uint8_t best = TILES_NUM_PADS;
        float best_change = 0.004f;
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            const tiles_rgb01_t *want = &target[i], *shown = &s_standby_written[i];
            float change = !s_standby_written_valid[i]
                               ? 2.0f
                               : fmaxf(fabsf(want->r - shown->r), fmaxf(fabsf(want->g - shown->g), fabsf(want->b - shown->b)));
            if (change > best_change) {
                best_change = change;
                best = i;
            }
        }
        if (best == TILES_NUM_PADS) {
            break;
        }
        write_pad(best);
        any = true;
    }
    return any;
}

/* While a light effect animates: up to PLAY_FX_WRITES_PER_PASS pads whose
 * effect changed most since written. True if it wrote any (the pass's
 * usual write then waits). */
static bool service_play_fx(uint32_t now_ms) {
    if (s_standby_active || tiles_fx_get() == TILES_FX_OFF) {
        return false;
    }
    tiles_fx_step(now_ms);
    bool busy = tiles_fx_busy(now_ms);
    float now_light[TILES_NUM_PADS][3];
    float change[TILES_NUM_PADS];
    bool any = false;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        float shown = s_play_fx_written[i][0] + s_play_fx_written[i][1] + s_play_fx_written[i][2];
        if (!busy && shown < 0.004f) {
            change[i] = 0.0f; /* nothing animating, nothing left to clear */
            continue;
        }
        play_fx_light(i, now_ms, now_light[i]);
        change[i] = fmaxf(fabsf(now_light[i][0] - s_play_fx_written[i][0]),
                          fmaxf(fabsf(now_light[i][1] - s_play_fx_written[i][1]),
                                fabsf(now_light[i][2] - s_play_fx_written[i][2])));
    }
    for (uint8_t n = 0; n < PLAY_FX_WRITES_PER_PASS; n++) {
        uint8_t best = TILES_NUM_PADS;
        float best_change = PLAY_FX_CHANGE_MIN;
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            if (change[i] >= best_change) {
                best_change = change[i];
                best = i;
            }
        }
        if (best == TILES_NUM_PADS) {
            break;
        }
        write_pad(best);
        change[best] = 0.0f;
        any = true;
    }
    return any;
}

static void write_pad(uint8_t pad_index /* 0-23 */) {
    const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(pad_index + 1u));
    if (cfg == NULL) {
        return;
    }

    s_pad_fx_shown[pad_index] = pad_fx_active(pad_index);
    s_pad_written_ms[pad_index] = to_ms_since_boot(get_absolute_time());
    tiles_rgb01_t desired = pad_desired_rgb(pad_index);
    if (s_standby_active) {
        s_standby_written[pad_index] = desired;
        s_standby_written_valid[pad_index] = true;
    }
    uint32_t pixel;
    if (s_test_white_level != 0u) {
        pixel = tiles_sk6805_pack_rgb(s_test_white_level, s_test_white_level, s_test_white_level);
    } else {
        desired = apply_pad_fx(pad_index, desired);
        float scale = (float)static_ceiling_level() * led_intensity();
        uint8_t r = (uint8_t)(scale * clamp01(desired.r));
        uint8_t g = (uint8_t)(scale * clamp01(desired.g));
        uint8_t b = (uint8_t)(scale * clamp01(desired.b));
        pixel = tiles_sk6805_pack_rgb(r, g, b);
    }

    /* Crash-recorder marks ('i' before the mux, 'w'/'x' around the pixel
     * write) so a report shows which blocking step a hang was in. Both steps
     * are timeout-bounded. */
    tiles_debug_trace('i');
    /* Select + enable in one I2C write (the bank is already disabled after
     * the last pixel); two writes a pixel instead of four. */
    tiles_tca9554_select_and_enable(&s_led_mux, cfg->led.mux_channel, cfg->led.mux_index);
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_pad_chain, &pixel, 1);
    /* 'x' = tiles_sk6805_write() returned (it once hung inside despite its
     * timeout; see drivers/sk6805.c). */
    tiles_debug_trace('x');
    tiles_tca9554_disable_all_muxes(&s_led_mux);
}

/* Debug mode's underglow: a Sentia magenta pulse (red is reserved for the
 * crash indicator, so the two can't be confused). Same pulse shape as the
 * menus' selection pulse, as a separate copy. */
#define DEBUG_UNDERGLOW_PULSE_PERIOD_MS 900.0f
#define DEBUG_UNDERGLOW_PULSE_MIN 0.35f
#define DEBUG_UNDERGLOW_PULSE_MAX 1.0f
#define DEBUG_UNDERGLOW_PI 3.14159265358979323846f

static float debug_underglow_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / DEBUG_UNDERGLOW_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * DEBUG_UNDERGLOW_PI * phase);
    return DEBUG_UNDERGLOW_PULSE_MIN + (DEBUG_UNDERGLOW_PULSE_MAX - DEBUG_UNDERGLOW_PULSE_MIN) * raw;
}

/* Writes straight to the LEDs, bypassing s_underglow_rgb[]/standby
 * ownership, so it shows whatever mode or menu owns rendering. When debug
 * mode ends, the normal path takes over again. R and B share the pulse so
 * the color stays magenta. */
static void write_debug_underglow(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float pulse = debug_underglow_pulse_level(now_ms);
    uint8_t level = underglow_channel_level(pulse);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, 0u, level);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    /* Same 'w'/'x' marks as write_pad() (one character for pad and underglow
     * writes, to save trace space). */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

/* Crash indicator: pure red pulse (nothing else uses red). Same shape and
 * bypass as debug mode's, as a separate copy. */
#define CRASH_UNDERGLOW_PULSE_PERIOD_MS 900.0f
#define CRASH_UNDERGLOW_PULSE_MIN 0.35f
#define CRASH_UNDERGLOW_PULSE_MAX 1.0f

static float crash_underglow_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / CRASH_UNDERGLOW_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * DEBUG_UNDERGLOW_PI * phase);
    return CRASH_UNDERGLOW_PULSE_MIN + (CRASH_UNDERGLOW_PULSE_MAX - CRASH_UNDERGLOW_PULSE_MIN) * raw;
}

static void write_crash_underglow(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float pulse = crash_underglow_pulse_level(now_ms);
    uint8_t level = underglow_channel_level(pulse);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, 0u, 0u);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    /* Same 'w'/'x' marks as write_pad(). */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

/* Song capture indicator: an amber pulse, straight to the LEDs like the
 * two above, because the mode's own pad grid stays visible underneath
 * while capturing (melodic/chord/bass don't claim standby), so the
 * underglow is the only place left. Amber is used by nothing else. Song
 * mode's own grid shows a steady yellow instead (op_mode.c
 * render_song_underglow()). */
#define SONG_CAPTURE_UNDERGLOW_PULSE_PERIOD_MS 900.0f
#define SONG_CAPTURE_UNDERGLOW_PULSE_MIN 0.35f
#define SONG_CAPTURE_UNDERGLOW_PULSE_MAX 1.0f

static float song_capture_underglow_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / SONG_CAPTURE_UNDERGLOW_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * DEBUG_UNDERGLOW_PI * phase);
    return SONG_CAPTURE_UNDERGLOW_PULSE_MIN + (SONG_CAPTURE_UNDERGLOW_PULSE_MAX - SONG_CAPTURE_UNDERGLOW_PULSE_MIN) * raw;
}

static void write_song_capture_underglow(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float pulse = song_capture_underglow_pulse_level(now_ms);
    uint8_t level = underglow_channel_level(pulse);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, level, 0u);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    /* Same 'w'/'x' marks as write_pad(). */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

/* Pattern save/delete confirmation: the color op_mode.c already resolved
 * for the pad flash (tiles_op_mode_pattern_flash_underglow_color()), so
 * pad and underglow blink together. */
static void write_pattern_flash_underglow(float r, float g, float b) {
    uint8_t level_r = underglow_channel_level(r);
    uint8_t level_g = underglow_channel_level(g);
    uint8_t level_b = underglow_channel_level(b);
    uint32_t pixel = tiles_sk6805_pack_rgb(level_r, level_g, level_b);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

static void write_underglow(void) {
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        const tiles_rgb01_t *c = &s_underglow_rgb[i];
        float k = led_intensity();
        pixels[i] = tiles_sk6805_pack_rgb(underglow_channel_level(c->r * k), underglow_channel_level(c->g * k),
                                           underglow_channel_level(c->b * k));
    }
    /* Same 'w'/'x' marks as write_pad(). */
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

static void set_pad_press_internal(uint8_t logical_pad, float press_0_to_1) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }

    uint8_t pad_index = (uint8_t)(logical_pad - 1u);
    if (s_pad_press[pad_index] == press_0_to_1) {
        return;
    }
    s_pad_press[pad_index] = press_0_to_1;

    /* Write now instead of waiting up to ~24 loop passes for the round-robin,
     * so touch feels immediate. The round-robin keeps running in the
     * background. */
    if (s_initialized) {
        write_pad(pad_index);
    }
}

bool tiles_lighting_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pad_press[i] = 0.0f;
        s_pad_standby_rgb[i] = (tiles_rgb01_t){0.0f, 0.0f, 0.0f};
    }
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        s_underglow_rgb[i] = s_mode_underglow;
    }
    s_service_cursor = 0;
    s_initialized = false;
    s_standby_active = false;
    s_underglow_override_was_active = false;

    if (!tiles_sk6805_init(&s_underglow_chain, pio0, TILES_GPIO_UNDERGLOW_DATA)) {
        return false;
    }
    if (!tiles_sk6805_init(&s_pad_chain, pio0, TILES_GPIO_PAD_LED_DATA)) {
        tiles_sk6805_deinit(&s_underglow_chain);
        return false;
    }
    if (!tiles_tca9554_init(&s_led_mux, i2c1, TILES_I2C1_ADDR_LED_MUX_TCA9554)) {
        tiles_sk6805_deinit(&s_underglow_chain);
        tiles_sk6805_deinit(&s_pad_chain);
        return false;
    }

    write_underglow();

    s_initialized = true;

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        write_pad(i);
    }

    return true;
}

void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1) {
    if (s_standby_active) {
        return;
    }
    set_pad_press_internal(logical_pad, press_0_to_1);
}

static void write_test_underglow(void) {
    uint32_t pixel = tiles_sk6805_pack_rgb(s_test_white_level, s_test_white_level, s_test_white_level);
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        pixels[i] = pixel;
    }
    tiles_debug_trace('w');
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
    tiles_debug_trace('x');
}

void tiles_lighting_set_test_white(uint8_t percent_0_to_100) {
    if (percent_0_to_100 > 100u) {
        percent_0_to_100 = 100u;
    }
    s_test_white_percent = percent_0_to_100;
    s_test_white_level = (uint8_t)((255u * percent_0_to_100) / 100u);
    if (percent_0_to_100 != 0u && s_test_white_level == 0u) {
        s_test_white_level = 1u;
    }
    if (!s_initialized) {
        return;
    }
    /* Apply at once (all 24 pads), so the meter settles without waiting on
     * the round-robin; on the way out, normal colors and the underglow. */
    if (s_test_white_level != 0u) {
        write_test_underglow();
    } else {
        write_underglow();
    }
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        write_pad(i);
    }
}

uint8_t tiles_lighting_get_test_white(void) {
    return s_test_white_percent;
}

void tiles_lighting_service(void) {
    if (!s_initialized) {
        return;
    }

    /* Bench test: hold everything white; nothing else draws. */
    if (s_test_white_level != 0u) {
        write_test_underglow();
        write_pad(s_service_cursor);
        s_service_cursor = (uint8_t)((s_service_cursor + 1u) % TILES_NUM_PADS);
        return;
    }

    /* Underglow override priority: crash > pattern flash > debug > Song
     * capture. Crash first (debug mode can survive a crash reboot, and the
     * crash is the surprise); the brief pattern confirmation beats the ambient
     * debug pulse. */
    bool underglow_override_active = false;
    float pattern_flash_r, pattern_flash_g, pattern_flash_b;
    /* Logs which override owns the underglow, on change only (for debugging
     * the pattern flash). */
    static uint8_t s_debug_last_override_kind = 0xFFu;
    uint8_t debug_override_kind = 0u;
    if (tiles_crash_indicator_is_active()) {
        write_crash_underglow();
        underglow_override_active = true;
        debug_override_kind = 1u;
    } else if (tiles_op_mode_pattern_flash_underglow_color(&pattern_flash_r, &pattern_flash_g, &pattern_flash_b)) {
        /* Above debug mode: a 600 ms confirmation of something the player just did
         * beats an ambient pulse. */
        write_pattern_flash_underglow(pattern_flash_r, pattern_flash_g, pattern_flash_b);
        underglow_override_active = true;
        debug_override_kind = 2u;
    } else if (tiles_debug_mode_is_active()) {
        write_debug_underglow();
        underglow_override_active = true;
        debug_override_kind = 3u;
    } else if (tiles_op_mode_song_capture_is_active()) {
        /* Song capture: lowest (the player turned it on on purpose). */
        write_song_capture_underglow();
        underglow_override_active = true;
        debug_override_kind = 4u;
    }
    if (debug_override_kind != s_debug_last_override_kind) {
        printf("[lighting] underglow override -> %u (0=none 1=crash 2=pattern_flash 3=debug 4=song_capture)\n",
               (unsigned)debug_override_kind);
        s_debug_last_override_kind = debug_override_kind;
    }

    /* Nothing else redraws the underglow every frame (pads have the
     * round-robin), so when an override stops, push s_underglow_rgb[] once;
     * otherwise it stays frozen at the pulse's last level (seen: the crash
     * dismiss left solid red). */
    if (s_underglow_override_was_active && !underglow_override_active) {
        write_underglow();
    }
    s_underglow_override_was_active = underglow_override_active;

    /* Button LEDs follow the LED intensity (no-op unless it changed). */
    tiles_buttons_set_led_intensity(led_intensity());

    /* One pad write per pass: a pad showing pressure hue / bend dimming (or
     * one that just stopped) that's due, taking turns; else the round-robin. */
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    update_pad_fx(now_ms);
    for (uint8_t n = 0; n < TILES_NUM_PADS; n++) {
        uint8_t i = (uint8_t)((s_live_cursor + n) % TILES_NUM_PADS);
        if ((pad_fx_active(i) || s_pad_fx_shown[i]) && now_ms - s_pad_written_ms[i] >= PAD_LIVE_REFRESH_MS) {
            write_pad(i);
            s_live_cursor = (uint8_t)((i + 1u) % TILES_NUM_PADS);
            return;
        }
    }
    /* Then a standby screen's changes, or the playing light effect, before
     * the round-robin. */
    if (s_standby_active ? service_standby_frame(now_ms) : service_play_fx(now_ms)) {
        return;
    }
    write_pad(s_service_cursor);
    s_service_cursor = (uint8_t)((s_service_cursor + 1u) % TILES_NUM_PADS);
}

void tiles_lighting_set_mode_underglow(float r, float g, float b) {
    s_mode_underglow = (tiles_rgb01_t){clamp01(r), clamp01(g), clamp01(b)};
    if (s_standby_active) {
        return; /* the standby owner draws the underglow; restored on release */
    }
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        s_underglow_rgb[i] = s_mode_underglow;
    }
    if (s_initialized) {
        write_underglow();
    }
}

void tiles_lighting_set_grid_effects(bool on) {
    s_grid_fx = on;
    tiles_fx_set_sequencer(on);
}

void tiles_lighting_set_standby_active(bool active) {
    if (active && !s_standby_active) {
        for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
            s_standby_written_valid[i] = false; /* the pads show the normal look */
        }
    }
    s_standby_active = active;

    if (!active) {
        /* Pads repaint on their own (touch calls set_pad_press every scan); the
         * underglow has no other driver, so restore it here: the mode's colour. */
        for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
            s_underglow_rgb[i] = s_mode_underglow;
        }
        if (s_initialized) {
            write_underglow();
        }
    }
}

void tiles_lighting_set_standby_frame(const float rgb[][3], uint8_t max_writes) {
    (void)max_writes; /* service_standby_frame() paces every standby write */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        tiles_lighting_set_standby_pad_rgb((uint8_t)(i + 1u), rgb[i][0], rgb[i][1], rgb[i][2]);
    }
}

void tiles_lighting_set_standby_pad_rgb(uint8_t logical_pad, float r, float g, float b) {
    if (!s_standby_active || logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    uint8_t pad_index = (uint8_t)(logical_pad - 1u);
    tiles_rgb01_t c = {clamp01(r), clamp01(g), clamp01(b)};
    tiles_rgb01_t *stored = &s_pad_standby_rgb[pad_index];
    if (stored->r == c.r && stored->g == c.g && stored->b == c.b) {
        return;
    }
    *stored = c; /* tiles_lighting_service() sends it (service_standby_frame()) */
}

void tiles_lighting_flush_standby(void) {
    if (!s_standby_active || !s_initialized) {
        return;
    }
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        tiles_rgb01_t want = standby_pad_rgb(i, now_ms), shown = s_standby_written[i];
        if (!s_standby_written_valid[i] || fabsf(want.r - shown.r) > 0.004f || fabsf(want.g - shown.g) > 0.004f ||
            fabsf(want.b - shown.b) > 0.004f) {
            write_pad(i);
        }
    }
}

/* Underglow from the pads. The floor keeps the mode's colour faintly at
 * rest; a pad's light reaches an LED with weight exp(-d^2 / 2) for d pads
 * from its anchor (1 right above it, 0.6 next to it, 0.14 two away).
 * Changes under 1/64 aren't sent. First guesses. */
#define GLOW_FLOOR 0.15f
#define GLOW_RELEASE_MS 120.0f
#define GLOW_SEND_INTERVAL_MS 16u
#define GLOW_STEPS 64.0f
static float s_glow_weight[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS][TILES_NUM_PADS];
static bool s_glow_weight_ready;
static float s_glow[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS][3];
static uint32_t s_glow_ms;
static uint32_t s_glow_sent_ms;

void tiles_lighting_set_standby_underglow_from_pads(const float rgb[][3], float base_r, float base_g, float base_b) {
    if (!s_standby_active) {
        return;
    }
    if (!s_glow_weight_ready) {
        for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
            for (uint8_t row = 1u; row <= TILES_GRID_MAX_ROW; row++) {
                for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
                    float dr = (float)row - (float)g_tiles_underglow_anchor[i].row;
                    float dc = (float)col - (float)g_tiles_underglow_anchor[i].col;
                    s_glow_weight[i][board_pad_for_row_col(row, col) - 1u] = expf(-(dr * dr + dc * dc) * 0.5f);
                }
            }
        }
        s_glow_weight_ready = true;
    }
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    float fall = 1.0f - expf(-(float)(now_ms - s_glow_ms) / GLOW_RELEASE_MS);
    s_glow_ms = now_ms;
    const float base[3] = {base_r, base_g, base_b};
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        for (uint8_t ch = 0; ch < 3u; ch++) {
            float want = GLOW_FLOOR * clamp01(base[ch]);
            for (uint8_t p = 0; p < TILES_NUM_PADS; p++) {
                want = fmaxf(want, clamp01(rgb[p][ch]) * s_glow_weight[i][p]);
            }
            float *g = &s_glow[i][ch];
            *g = want > *g ? want : *g + (want - *g) * fall; /* up at once, down slowly */
        }
    }
    if (now_ms - s_glow_sent_ms < GLOW_SEND_INTERVAL_MS) {
        return;
    }
    bool changed = false;
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        tiles_rgb01_t c = {roundf(s_glow[i][0] * GLOW_STEPS) / GLOW_STEPS, roundf(s_glow[i][1] * GLOW_STEPS) / GLOW_STEPS,
                           roundf(s_glow[i][2] * GLOW_STEPS) / GLOW_STEPS};
        tiles_rgb01_t *stored = &s_underglow_rgb[i];
        if (stored->r != c.r || stored->g != c.g || stored->b != c.b) {
            *stored = c;
            changed = true;
        }
    }
    if (changed && s_initialized) {
        write_underglow(); /* one chain write for all four */
        s_glow_sent_ms = now_ms;
    }
}

void tiles_lighting_set_standby_underglow_rgb(uint8_t pixel_index, float r, float g, float b) {
    if (!s_standby_active || pixel_index >= TILES_LIGHTING_NUM_UNDERGLOW_PIXELS) {
        return;
    }
    tiles_rgb01_t c = {clamp01(r), clamp01(g), clamp01(b)};
    tiles_rgb01_t *stored = &s_underglow_rgb[pixel_index];
    if (stored->r == c.r && stored->g == c.g && stored->b == c.b) {
        return;
    }
    *stored = c;
    if (s_initialized) {
        write_underglow();
    }
}
