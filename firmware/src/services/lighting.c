#include "lighting.h"

#include "board_pins.h"
#include "crash_indicator.h"
#include "debug_mode.h"
#include "note_map.h"
#include "op_mode.h"
#include "pad_config.h"
#include "power.h"

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

/* Melodic mode's plain pad: a natural key that isn't root, fifth or
 * sounding. Kept separate from the idle baseline so only this pad dims.
 * Set below root and fifth (40) so the landmarks and the echo stand out
 * (tried 30, then "dim 30% the white": 21).
 *
 * Brightness order, keep it: pressed (100% white) > echoing (100% of its
 * color, flash to white) > root and fifth (40) > plain pad (this). Root,
 * fifth, pressed and echo must not be dimmed by changes to this. */
#define TILES_LIGHTING_NATURAL_BASELINE_PERCENT 21u

/* Idle melodic coloring by note role: root = Sentia magenta (#FF00FF) at
 * this level, sharps/black keys dark (see pad_desired_rgb()). A touched
 * pad is always plain white. Raised with the other levels (6 -> 20 -> 40). */
#define TILES_LIGHTING_ROOT_BASELINE_PERCENT 40u

/* Perfect fifth: violet, i.e. blue with some red mixed in
 * (TILES_LIGHTING_FIFTH_RED_TINT), nearer the root's magenta than pure
 * blue but still distinct (hue ~261 deg vs root 300, blue 240). Raise the
 * tint to pull it toward magenta. (A third-degree landmark was tried and
 * removed: root and fifth only.) Unmeasured on the real diffusers. */
#define TILES_LIGHTING_FIFTH_BASELINE_PERCENT 40u
#define TILES_LIGHTING_FIFTH_RED_TINT 0.35f

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

/* The constants above are the DEFAULTS of the look.* settings. s_look[] is
 * what pad_desired_rgb() reads; settings can change it live over USB and
 * save it to flash. Stored in whole percent (tints: 0.35 -> 35). The power
 * ceiling is applied after all of this and is not a setting, so no setting
 * can raise LED current. */
#define LOOK_PCT(fraction) ((uint16_t)((fraction) * 100.0f + 0.5f))
static uint16_t s_look[TILES_LOOK_COUNT] = {
    [TILES_LOOK_IDLE_BASELINE_PERCENT] = TILES_LIGHTING_IDLE_BASELINE_PERCENT,
    [TILES_LOOK_NATURAL_PERCENT] = TILES_LIGHTING_NATURAL_BASELINE_PERCENT,
    [TILES_LOOK_ROOT_PERCENT] = TILES_LIGHTING_ROOT_BASELINE_PERCENT,
    [TILES_LOOK_FIFTH_PERCENT] = TILES_LIGHTING_FIFTH_BASELINE_PERCENT,
    [TILES_LOOK_FIFTH_RED_TINT_PERCENT] = LOOK_PCT(TILES_LIGHTING_FIFTH_RED_TINT),
    [TILES_LOOK_ECHO_SUSTAIN_TINT_PERCENT] = LOOK_PCT(TILES_LIGHTING_ECHO_SUSTAIN_TINT),
    [TILES_LOOK_ECHO_SECONDARY_G_PERCENT] = LOOK_PCT(TILES_LIGHTING_ECHO_SECONDARY_G),
    [TILES_LOOK_ECHO_SECONDARY_B_PERCENT] = LOOK_PCT(TILES_LIGHTING_ECHO_SECONDARY_B),
    [TILES_LOOK_ECHO_FLASH_MS] = TILES_LIGHTING_ECHO_FLASH_MS,
};

#define TILES_LOOK_MAX_FLASH_MS 2000u

static float look_fraction(tiles_look_param_t param) {
    return (float)s_look[param] / 100.0f;
}

uint16_t tiles_lighting_get_look(tiles_look_param_t param) {
    return param < TILES_LOOK_COUNT ? s_look[param] : 0u;
}

void tiles_lighting_set_look(tiles_look_param_t param, uint16_t value) {
    if (param >= TILES_LOOK_COUNT) {
        return;
    }
    uint16_t max = (param == TILES_LOOK_ECHO_FLASH_MS) ? TILES_LOOK_MAX_FLASH_MS : 100u;
    s_look[param] = value > max ? max : value;
}

/* Underglow's own fixed brightness (out of 255), NOT scaled by the power
 * ceiling: at 37% of the USB ceiling it barely glowed. 4 LEDs are a small
 * share of the budget (included in the accounting below), and a steady
 * halo doesn't compete with pad feedback. */
#define TILES_LIGHTING_UNDERGLOW_LEVEL 230u

#define TILES_LIGHTING_NUM_UNDERGLOW_PIXELS 4u

/* ---- Pad brightness ceiling: static, deliberately -------------------------
 * One flat ceiling from the power mode, never recomputed from how many
 * pads are lit (a load-aware ceiling made the whole board's brightness
 * shift with playing, which looks like a brownout).
 *
 * Budget (worst case):
 *   - LEDs: 16 mA/pixel at full white incl. ~1 mA controller overhead
 *     (board map current_model) x 28 pixels = 448 mA; ~28 mA idle floor.
 *   - MCU + sensors + ICs + button LEDs (datasheet estimates): RP2350
 *     ~60 mA, 24 x TMAG5273 ~72 mA, MPR121s ~4 mA, muxes/expander ~2 mA,
 *     PCA9685 ICs ~2 mA, 6 button LEDs ~80 mA: ~220 mA.
 *   - Haptic motors: UNMEASURED. ~60-100 mA each while spinning, so
 *     300-400 mA for the USB voice limit: possibly the largest term.
 * USB-only (500 mA) leaves no confirmed room above 37%. External (2500 mA)
 * keeps ~1.8 A of margin, so its ceiling is 90% (power.c). Measure motor
 * current before changing either. */
static uint8_t static_ceiling_level(void) {
    uint8_t ceiling_percent = tiles_power_get_state().led_brightness_ceiling_percent;
    return (uint8_t)((255u * ceiling_percent) / 100u);
}

typedef struct {
    float r;
    float g;
    float b;
} tiles_rgb01_t;

static tiles_sk6805_chain_t s_underglow_chain;
static tiles_sk6805_chain_t s_pad_chain;
static tiles_tca9554_t s_led_mux;
static float s_pad_press[TILES_NUM_PADS]; /* touch-driven, white, floored at the idle level */
static tiles_rgb01_t s_pad_standby_rgb[TILES_NUM_PADS]; /* standby color, no floor */
static tiles_rgb01_t s_underglow_rgb[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
static uint8_t s_service_cursor;
static bool s_initialized;
static bool s_standby_active;
/* Whether a crash/debug/pattern/capture override owned the underglow last
 * frame (restore on release; see tiles_lighting_service()). */
static bool s_underglow_override_was_active;

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

/* What a pad's r/g/b wants (0.0-1.0) in every state, before the ceiling
 * (applied in write_pad()). The single source of truth for a pad's look. */
static tiles_rgb01_t pad_desired_rgb(uint8_t pad_index) {
    if (s_standby_active) {
        return s_pad_standby_rgb[pad_index];
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
     * daw-integration/README.md) light green, a color nothing else here uses.
     * After the live-touch check, before the idle looks. */
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

    /* Bass guitar mode: neck-style coloring. Unmarked frets are amber at the
     * idle level; inlay frets brighter; octave (double-dot) frets brightest. */
    if (tiles_note_map_is_guitar_mode_active()) {
        bool is_octave = false;
        if (tiles_note_map_is_guitar_fret_marker_pad(logical_pad, &is_octave)) {
            float level = is_octave ? 1.0f : 0.75f;
            return (tiles_rgb01_t){level, level * 0.5f, 0.0f};
        }
        float level = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT);
        return (tiles_rgb01_t){level, level * 0.5f, 0.0f};
    }

    /* Chord mode's chord strip: one solid blue for the whole region. The
     * melody grid needs no branch (the root/natural checks are chord-aware). */
    if (tiles_note_map_is_chord_region_pad(logical_pad)) {
        float level = look_fraction(TILES_LOOK_IDLE_BASELINE_PERCENT);
        return (tiles_rgb01_t){0.0f, 0.0f, level};
    }

    /* Idle melodic coloring by note role. Root first: a root pad can be a
     * sharp in some keys, and root wins. */
    if (tiles_note_map_is_root_pad(logical_pad)) {
        /* Sentia magenta: R and B only. */
        float level = look_fraction(TILES_LOOK_ROOT_PERCENT);
        return (tiles_rgb01_t){level, 0.0f, level};
    }
    if (tiles_note_map_is_fifth_pad(logical_pad)) {
        /* Violet: blue with a little red (see TILES_LIGHTING_FIFTH_RED_TINT). */
        float level = look_fraction(TILES_LOOK_FIFTH_PERCENT);
        return (tiles_rgb01_t){level * look_fraction(TILES_LOOK_FIFTH_RED_TINT_PERCENT), 0.0f, level};
    }
    if (tiles_note_map_is_natural_pad(logical_pad)) {
        float level = look_fraction(TILES_LOOK_NATURAL_PERCENT);
        return (tiles_rgb01_t){level, level, level};
    }
    /* Sharp/black key at rest: true black, the one exception to "pads never
     * go fully dark". */
    return (tiles_rgb01_t){0.0f, 0.0f, 0.0f};
}

static void write_pad(uint8_t pad_index /* 0-23 */) {
    const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(pad_index + 1u));
    if (cfg == NULL) {
        return;
    }

    tiles_rgb01_t desired = pad_desired_rgb(pad_index);
    uint8_t ceiling = static_ceiling_level();
    uint8_t r = (uint8_t)((float)ceiling * clamp01(desired.r));
    uint8_t g = (uint8_t)((float)ceiling * clamp01(desired.g));
    uint8_t b = (uint8_t)((float)ceiling * clamp01(desired.b));
    uint32_t pixel = tiles_sk6805_pack_rgb(r, g, b);

    /* Crash-recorder marks ('i' before the mux, 'w'/'x' around the pixel
     * write) so a report shows which blocking step a hang was in. Both steps
     * are timeout-bounded. */
    tiles_debug_trace('i');
    tiles_tca9554_disable_all_muxes(&s_led_mux);
    tiles_tca9554_set_select(&s_led_mux, cfg->led.mux_channel);
    tiles_tca9554_enable_mux(&s_led_mux, cfg->led.mux_index);
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
        pixels[i] = tiles_sk6805_pack_rgb(underglow_channel_level(c->r), underglow_channel_level(c->g),
                                           underglow_channel_level(c->b));
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
        s_underglow_rgb[i] = (tiles_rgb01_t){1.0f, 1.0f, 1.0f};
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

void tiles_lighting_service(void) {
    if (!s_initialized) {
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

    write_pad(s_service_cursor);
    s_service_cursor = (uint8_t)((s_service_cursor + 1u) % TILES_NUM_PADS);
}

void tiles_lighting_set_standby_active(bool active) {
    s_standby_active = active;

    if (!active) {
        /* Pads repaint on their own (touch calls set_pad_press every scan); the
         * underglow has no other driver, so restore it here. */
        for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
            s_underglow_rgb[i] = (tiles_rgb01_t){1.0f, 1.0f, 1.0f};
        }
        if (s_initialized) {
            write_underglow();
        }
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
    *stored = c;

    /* Immediate write, as in set_pad_press_internal(). */
    if (s_initialized) {
        write_pad(pad_index);
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
