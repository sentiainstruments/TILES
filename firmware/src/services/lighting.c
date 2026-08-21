#include "lighting.h"

#include "board_pins.h"
#include "pad_config.h"

#include "sk6805.h"
#include "tca9554.h"

/* Conservative USB-only ceiling per docs/architecture/defaults-and-safeguards.md
 * "LED color and brightness" -- 35-40% documented range, using the
 * midpoint. Hardcoded because there is no power-profile/GP22 governor
 * yet; replace with a real lookup once profiles/ exists. Never raise
 * this without that governor in place -- see the power safeguards in
 * the same doc. */
#define TILES_LIGHTING_CEILING_PERCENT_USB 37u
#define TILES_LIGHTING_IDLE_BASELINE_PERCENT 10u

static tiles_sk6805_chain_t s_underglow_chain;
static tiles_sk6805_chain_t s_pad_chain;
static tiles_tca9554_t s_led_mux;
static float s_pad_press[TILES_NUM_PADS];
static uint8_t s_service_cursor;
static bool s_initialized;

static uint8_t ceiling_level(void) {
    return (uint8_t)((255u * TILES_LIGHTING_CEILING_PERCENT_USB) / 100u);
}

static uint8_t idle_baseline_level(void) {
    return (uint8_t)(((uint32_t)ceiling_level() * TILES_LIGHTING_IDLE_BASELINE_PERCENT) / 100u);
}

static uint8_t pad_level_for_press(float press_0_to_1) {
    if (press_0_to_1 < 0.0f) {
        press_0_to_1 = 0.0f;
    }
    if (press_0_to_1 > 1.0f) {
        press_0_to_1 = 1.0f;
    }
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

bool tiles_lighting_init(void) {
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pad_press[i] = 0.0f;
    }
    s_service_cursor = 0;
    s_initialized = false;

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

    uint8_t lvl = idle_baseline_level();
    uint32_t underglow_pixel = tiles_sk6805_pack_rgb(lvl, lvl, lvl);
    uint32_t underglow_pixels[4] = {underglow_pixel, underglow_pixel, underglow_pixel, underglow_pixel};
    tiles_sk6805_write(&s_underglow_chain, underglow_pixels, 4);

    s_initialized = true;

    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        write_pad(i);
    }

    return true;
}

void tiles_lighting_set_pad_press(uint8_t logical_pad, float press_0_to_1) {
    if (logical_pad < 1u || logical_pad > TILES_NUM_PADS) {
        return;
    }
    s_pad_press[logical_pad - 1u] = press_0_to_1;
}

void tiles_lighting_service(void) {
    if (!s_initialized) {
        return;
    }

    write_pad(s_service_cursor);
    s_service_cursor = (uint8_t)((s_service_cursor + 1u) % TILES_NUM_PADS);
}
