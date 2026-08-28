#include "lighting.h"

#include "board_pins.h"
#include "note_map.h"
#include "pad_config.h"
#include "power.h"

#include "sk6805.h"
#include "tca9554.h"

/* Real feedback: "make all led brighter its hard to see" -- raised from
 * 10. Still a fraction of the active brightness ceiling (a power-derived
 * safety cap from tiles_power_get_state(), untouched by this change --
 * see this file's "Pad brightness ceiling" section further below for how
 * that ceiling is picked), so this only spends more of whatever headroom
 * that ceiling already allows on the resting/idle state, not a change to
 * the underlying power budget. */
#define TILES_LIGHTING_IDLE_BASELINE_PERCENT 25u

/* Idle (untouched) chromatic-play pad coloring by note role -- real
 * feedback: "root should be blue and black keys shouldnt have led this
 * in rest non pressed moment... push should be regular white illumination"
 * (unchanged, see pad_desired_rgb() below, still used whenever a pad
 * IS touched, root or not), then, after a first hardware pass: "make the
 * blue sentia purple for root notes but dim it a bit more than standard
 * non pressed pads." Root now uses Sentia Instruments' own brand
 * magenta/purple (#FF00FF -- the same color services/expression_control.c
 * uses for its sub-menu's selected-pad indicator and
 * services/boot_sequence.c uses for its final pulse phase) instead of
 * plain blue, at TILES_LIGHTING_ROOT_BASELINE_PERCENT -- deliberately
 * LOWER than TILES_LIGHTING_IDLE_BASELINE_PERCENT above (the natural-key
 * baseline), a reversal of this feature's first pass, which had root
 * brighter than naturals to stand out as a landmark; real feedback
 * called for the opposite, a subtler root indicator that reads as dimmer
 * than the surrounding white keys rather than a bright highlight.
 * Unmeasured -- a first attempt at "visibly dimmer than a natural key,
 * not so dim it disappears," not calibrated against real LED
 * brightness/diffusion. Sharp/black keys get no baseline floor at all
 * when idle (true black, see write_pad() below) -- unlike every other
 * idle pad in this file, which is deliberately never allowed to go
 * fully dark (see tiles_lighting_set_pad_press()'s header); this is a
 * narrow, deliberate exception specifically for the natural/sharp
 * readability distinction real feedback asked for.
 * Raised from 6, real feedback: "make root note led also brighter" (part
 * of a broader "make all led brighter its hard to see" -- see
 * TILES_LIGHTING_IDLE_BASELINE_PERCENT above). Kept below that constant's
 * new value so root stays visibly dimmer than a natural key at rest, per
 * the same real feedback that made it dimmer in the first place -- just
 * a less extreme gap now that both are brighter in absolute terms. */
#define TILES_LIGHTING_ROOT_BASELINE_PERCENT 15u

/* Underglow's own fixed brightness, out of 255 -- deliberately NOT
 * scaled by the active brightness ceiling/the power state. It used to be
 * a percentage of the active ceiling (65%), which meant it rode down
 * with the USB-only ceiling (37%) to ~24% of full and read as
 * "basically not glowing" on real hardware. Only 4 LEDs are on this
 * chain vs 24 on the pad grid -- even at full raw brightness the
 * current draw is a small fraction of the ~448mA full-grid estimate in
 * docs/architecture/defaults-and-safeguards.md, so there's no power
 * budget reason to hold it down the way the 24-pad grid needs to be.
 * It's a fixed ambient halo, not a per-pad state indicator, so running
 * it bright doesn't compete with touch/press feedback the way raising
 * every pad's baseline would. Its own current draw is still accounted
 * for in the fuller budget breakdown in this file's "Pad brightness
 * ceiling" section below, just not by any code here -- it stays a fixed
 * output regardless. */
#define TILES_LIGHTING_UNDERGLOW_LEVEL 230u

#define TILES_LIGHTING_NUM_UNDERGLOW_PIXELS 4u

/* ---- Pad brightness ceiling: back to static, deliberately -------------
 * Real feedback: "could we push the led celing a bit more safely?" led to
 * a first attempt (pad_dynamic_scale(), recomputing the ceiling every
 * frame from real projected current draw) -- then, after real hardware
 * feedback: "this led solution might look glitchy like we have unstable
 * power. lets find a solution that doesnt include shifting brightness."
 * Correct call: a ceiling that continuously reacts to how many OTHER
 * pads happen to be lit means the WHOLE board's brightness visibly
 * shifts as notes are struck/released or an animation frame's lit-pixel
 * count changes -- exactly what a real brownout looks like, even though
 * the underlying math was current-safe. Removed entirely; back to a
 * single flat ceiling per pad, chosen once (power mode changes, not
 * every frame) so a given pad's brightness at a given state is always
 * the same fixed value, never drifting with unrelated activity.
 *
 * Real feedback then asked to "calculate the safe range again to make
 * sure, acountign for ics lights and haptics and sensors" -- a fuller
 * accounting than the original ~448mA-LEDs-only estimate in
 * docs/architecture/defaults-and-safeguards.md:
 *   - LEDs: solid. 16mA/pixel at full white including ~1mA controller
 *     overhead (board map's own current_model), 28 pixels (24 pad + 4
 *     underglow) -> 448mA worst case, ~28mA idle floor even at zero
 *     brightness. This is the number the existing ceiling was built on.
 *   - MCU + sensors + I2C ICs + function-button LEDs: not measured for
 *     this board, but reasonably estimable from typical datasheet
 *     figures -- RP2350 active (~60mA) + 24x TMAG5273 Hall sensors
 *     (~3mA each, ~72mA) + 2x MPR121 (~4mA) + TCA9554/TCA9548A (~2mA) +
 *     2x PCA9685 IC overhead, not the loads they switch (~2mA) + 6
 *     function-button LEDs at worst case all lit, 150-ohm-from-5V per
 *     the board map (~80mA) -- roughly 220mA of overhead this file's
 *     own ceiling math never subtracted before.
 *   - Haptic motors: genuinely UNMEASURED -- both hardware docs flag
 *     this explicitly ("measure one motor's running and stall/start
 *     current" before trusting the higher-voice profiles). Small ERM
 *     motors typically run ~60-100mA each while spinning; USB-only
 *     allows up to 5 simultaneous voices, so a real worst case could be
 *     300-400mA from haptics ALONE -- potentially the single largest
 *     term in the whole budget, not LEDs.
 * On USB-only (500mA total), 220mA overhead + a genuinely uncertain
 * 300-400mA haptics worst case leaves little to no headroom confirmed
 * safe for LEDs beyond the existing ceiling -- raising it further isn't
 * something this fuller accounting actually supports, so power.c's
 * USB_ONLY/FAULT led_brightness_ceiling_percent stays at 37%, not
 * increased. External power (2500mA) keeps a large margin
 * (~1.8A) even under the same pessimistic haptics assumption, so that
 * ceiling (power.c's led_brightness_ceiling_percent for
 * EXTERNAL_ONLY/USB_AND_EXTERNAL) was raised 75 -> 90 there instead --
 * see power.c's own comment. Haptic motor current is the actual
 * highest-priority unknown to measure here, not anything in this file. */
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
static float s_pad_press[TILES_NUM_PADS]; /* touch-driven, white, baseline-floored -- normal operation only */
static tiles_rgb01_t s_pad_standby_rgb[TILES_NUM_PADS]; /* standby animation color, no baseline floor */
static tiles_rgb01_t s_underglow_rgb[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
static uint8_t s_service_cursor;
static bool s_initialized;
static bool s_standby_active;

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* Fraction of TILES_LIGHTING_UNDERGLOW_LEVEL -- see that constant's
 * header comment for why underglow doesn't share the pad grid's
 * power-derived ceiling. No baseline floor: unlike pad press (below),
 * underglow (and standby pad color, also below) are allowed to go to
 * true 0 -- there's no "never fully dark" requirement for either of
 * those, and standby animations specifically need real black for
 * contrast. */
static uint8_t underglow_channel_level(float channel_0_to_1) {
    return (uint8_t)((float)TILES_LIGHTING_UNDERGLOW_LEVEL * clamp01(channel_0_to_1));
}

/* What a pad's r/g/b wants (0.0-1.0 each), independent of the ceiling --
 * the ceiling is applied once, afterward, in write_pad() below. Kept as
 * its own function (rather than inlined into write_pad()) as the single
 * source of truth for "what does this pad look like" in every state. */
static tiles_rgb01_t pad_desired_rgb(uint8_t pad_index) {
    if (s_standby_active) {
        return s_pad_standby_rgb[pad_index];
    }
    if (s_pad_press[pad_index] > 0.0f) {
        /* Baseline-floored: 0.0 maps to the idle baseline fraction, not
         * true black -- this is normal (non-standby) touch-driven
         * operation's "pads never go fully dark in V1" requirement (see
         * tiles_lighting_set_pad_press's header). */
        float baseline = (float)TILES_LIGHTING_IDLE_BASELINE_PERCENT / 100.0f;
        float level = baseline + (1.0f - baseline) * clamp01(s_pad_press[pad_index]);
        return (tiles_rgb01_t){level, level, level};
    }
    /* Idle (untouched), normal chromatic play: color by note role -- real
     * feedback: "root should be blue [later: purple] and black keys
     * shouldnt have led this in rest non pressed moment." Root checked
     * first since a root pad can itself be a sharp/black key depending on
     * the current key offset (see tiles_note_map_is_root_pad()'s own
     * comment) -- root's color always wins over that. */
    uint8_t logical_pad = (uint8_t)(pad_index + 1u);
    if (tiles_note_map_is_root_pad(logical_pad)) {
        /* Sentia Instruments Magenta (#FF00FF) -- R and B channels only,
         * G stays 0 -- see TILES_LIGHTING_ROOT_BASELINE_PERCENT's own
         * comment for the color and brightness reasoning. */
        float level = (float)TILES_LIGHTING_ROOT_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){level, 0.0f, level};
    }
    if (tiles_note_map_is_natural_pad(logical_pad)) {
        float level = (float)TILES_LIGHTING_IDLE_BASELINE_PERCENT / 100.0f;
        return (tiles_rgb01_t){level, level, level};
    }
    /* Sharp/black key, idle -- true black, deliberately bypassing this
     * file's usual "pads never go fully dark" floor (see
     * TILES_LIGHTING_ROOT_BASELINE_PERCENT's own comment for why this
     * specific exception exists). */
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

    tiles_tca9554_disable_all_muxes(&s_led_mux);
    tiles_tca9554_set_select(&s_led_mux, cfg->led.mux_channel);
    tiles_tca9554_enable_mux(&s_led_mux, cfg->led.mux_index);
    tiles_sk6805_write(&s_pad_chain, &pixel, 1);
    tiles_tca9554_disable_all_muxes(&s_led_mux);
}

static void write_underglow(void) {
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        const tiles_rgb01_t *c = &s_underglow_rgb[i];
        pixels[i] = tiles_sk6805_pack_rgb(underglow_channel_level(c->r), underglow_channel_level(c->g),
                                           underglow_channel_level(c->b));
    }
    tiles_sk6805_write(&s_underglow_chain, pixels, TILES_LIGHTING_NUM_UNDERGLOW_PIXELS);
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

    /* Write immediately rather than waiting for tiles_lighting_service()'s
     * round-robin to reach this pad -- with 24 pads serviced one per
     * main-loop iteration, a touch change could otherwise take up to
     * ~24 loop iterations to actually reach the LED, which reads as
     * sluggish. The round-robin still runs continuously as a background
     * "keep everything current" sweep, this just short-circuits the
     * common case (touch/release) to feel immediate. */
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

    write_pad(s_service_cursor);
    s_service_cursor = (uint8_t)((s_service_cursor + 1u) % TILES_NUM_PADS);
}

void tiles_lighting_set_standby_active(bool active) {
    s_standby_active = active;

    if (!active) {
        /* Pads: no explicit restore needed -- touch.c calls
         * tiles_lighting_set_pad_press() every main-loop iteration
         * regardless of standby, so the very next scan (now unguarded)
         * writes each pad's real state. Underglow has no other
         * continuous driver, so restore it here explicitly. */
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

    /* Same immediate-write reasoning as set_pad_press_internal() above --
     * an animation frame should reach the LED right away, not wait for
     * the round-robin. */
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
