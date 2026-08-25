#include "lighting.h"

#include "board_pins.h"
#include "pad_config.h"
#include "power.h"

#include "sk6805.h"
#include "tca9554.h"

#define TILES_LIGHTING_IDLE_BASELINE_PERCENT 10u

/* Underglow's own fixed brightness, out of 255 -- deliberately NOT
 * scaled by ceiling_level()/the power state. It used to be a percentage
 * of the active ceiling (65% of ceiling_level()), which meant it rode
 * down with the USB-only ceiling (37%) to ~24% of full and read as
 * "basically not glowing" on real hardware. Only 4 LEDs are on this
 * chain vs 24 on the pad grid -- even at full raw brightness the
 * current draw is a small fraction of the ~448mA full-grid estimate in
 * docs/architecture/defaults-and-safeguards.md, so there's no power
 * budget reason to hold it down the way the 24-pad grid needs to be.
 * It's a fixed ambient halo, not a per-pad state indicator, so running
 * it bright doesn't compete with touch/press feedback the way raising
 * every pad's baseline would. */
#define TILES_LIGHTING_UNDERGLOW_LEVEL 230u

#define TILES_LIGHTING_NUM_UNDERGLOW_PIXELS 4u

static tiles_sk6805_chain_t s_underglow_chain;
static tiles_sk6805_chain_t s_pad_chain;
static tiles_tca9554_t s_led_mux;
static float s_pad_press[TILES_NUM_PADS];
static float s_underglow_press[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
static uint8_t s_service_cursor;
static bool s_initialized;
static bool s_standby_active;

/* Live read (not cached) so a power-state change -- e.g. external 12V
 * gets plugged in mid-session -- is reflected the very next time any
 * pad is written, with no extra wiring here. tiles_power_get_state()
 * is a cheap struct copy, safe to call this often. */
static uint8_t ceiling_level(void) {
    uint8_t ceiling_percent = tiles_power_get_state().led_brightness_ceiling_percent;
    return (uint8_t)((255u * ceiling_percent) / 100u);
}

static uint8_t idle_baseline_level(void) {
    return (uint8_t)(((uint32_t)ceiling_level() * TILES_LIGHTING_IDLE_BASELINE_PERCENT) / 100u);
}

static float clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* level_0_to_1 is a fraction of TILES_LIGHTING_UNDERGLOW_LEVEL, not of
 * ceiling_level() -- see that constant's header comment for why
 * underglow doesn't share the pad grid's power-derived ceiling. Normal
 * (non-standby) operation holds every pixel at 1.0. */
static uint8_t underglow_pixel_level(uint8_t index) {
    return (uint8_t)((float)TILES_LIGHTING_UNDERGLOW_LEVEL * clamp01(s_underglow_press[index]));
}

static uint8_t pad_level_for_press(float press_0_to_1) {
    press_0_to_1 = clamp01(press_0_to_1);
    uint8_t baseline = idle_baseline_level();
    uint8_t ceiling = ceiling_level();
    return (uint8_t)(baseline + (float)(ceiling - baseline) * press_0_to_1);
}

static void write_pad(uint8_t pad_index /* 0-23 */) {
    const tiles_pad_config_t *cfg = board_pad_config((uint8_t)(pad_index + 1u));
    if (cfg == NULL) {
        return;
    }

    uint8_t level = pad_level_for_press(s_pad_press[pad_index]);
    uint32_t pixel = tiles_sk6805_pack_rgb(level, level, level);

    tiles_tca9554_disable_all_muxes(&s_led_mux);
    tiles_tca9554_set_select(&s_led_mux, cfg->led.mux_channel);
    tiles_tca9554_enable_mux(&s_led_mux, cfg->led.mux_index);
    tiles_sk6805_write(&s_pad_chain, &pixel, 1);
    tiles_tca9554_disable_all_muxes(&s_led_mux);
}

static void write_underglow(void) {
    uint32_t pixels[TILES_LIGHTING_NUM_UNDERGLOW_PIXELS];
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        uint8_t level = underglow_pixel_level(i);
        pixels[i] = tiles_sk6805_pack_rgb(level, level, level);
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
    }
    for (uint8_t i = 0; i < TILES_LIGHTING_NUM_UNDERGLOW_PIXELS; i++) {
        s_underglow_press[i] = 1.0f;
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
            s_underglow_press[i] = 1.0f;
        }
        if (s_initialized) {
            write_underglow();
        }
    }
}

void tiles_lighting_set_standby_pad(uint8_t logical_pad, float level_0_to_1) {
    set_pad_press_internal(logical_pad, level_0_to_1);
}

void tiles_lighting_set_standby_underglow(uint8_t pixel_index, float level_0_to_1) {
    if (!s_standby_active || pixel_index >= TILES_LIGHTING_NUM_UNDERGLOW_PIXELS) {
        return;
    }
    float clamped = clamp01(level_0_to_1);
    if (s_underglow_press[pixel_index] == clamped) {
        return;
    }
    s_underglow_press[pixel_index] = clamped;
    if (s_initialized) {
        write_underglow();
    }
}
