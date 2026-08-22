#include "buttons.h"

#include "board_pins.h"
#include "pca9685.h"

#include "hardware/gpio.h"
#include "pico/time.h"

#define NUM_BUTTONS 6u
#define DEBOUNCE_MS 10u /* docs/hardware/.../scheduler_defaults: button_debounce_ms */

typedef struct {
    uint gpio;
    uint8_t pca9685_addr;
    uint8_t pca9685_channel;
} button_route_t;

/* Physical order left-to-right: capsule, capsule, triangle, diamond,
 * square, circle -- per docs/hardware/.../buttons table. */
static const button_route_t s_button_routes[NUM_BUTTONS] = {
    {TILES_GPIO_SW1_LEFT_CAPSULE, TILES_I2C1_ADDR_HAPTIC_PCA9685_1, 0u},
    {TILES_GPIO_SW2_RIGHT_CAPSULE, TILES_I2C1_ADDR_HAPTIC_PCA9685_1, 1u},
    {TILES_GPIO_SW3_TRIANGLE, TILES_I2C1_ADDR_HAPTIC_PCA9685_2, 2u},
    {TILES_GPIO_SW4_DIAMOND, TILES_I2C1_ADDR_HAPTIC_PCA9685_2, 3u},
    {TILES_GPIO_SW5_SQUARE, TILES_I2C1_ADDR_HAPTIC_PCA9685_2, 4u},
    {TILES_GPIO_SW6_CIRCLE, TILES_I2C1_ADDR_HAPTIC_PCA9685_2, 5u},
};

static tiles_pca9685_t s_pca1; /* TILES_I2C1_ADDR_HAPTIC_PCA9685_1 */
static tiles_pca9685_t s_pca2; /* TILES_I2C1_ADDR_HAPTIC_PCA9685_2 */
static bool s_raw_pressed[NUM_BUTTONS];
static bool s_debounced[NUM_BUTTONS];
static uint32_t s_last_change_ms[NUM_BUTTONS];

static tiles_pca9685_t *pca_for_addr(uint8_t addr) {
    if (addr == TILES_I2C1_ADDR_HAPTIC_PCA9685_1) {
        return &s_pca1;
    }
    if (addr == TILES_I2C1_ADDR_HAPTIC_PCA9685_2) {
        return &s_pca2;
    }
    return NULL;
}

static void set_button_led(uint8_t index, bool lit) {
    tiles_pca9685_t *pca = pca_for_addr(s_button_routes[index].pca9685_addr);
    if (pca == NULL) {
        return;
    }
    /* Active-low wiring: lit = pin driven low = PCA9685 "full off"
     * internal state; dark = pin high = "full on" internal state. */
    tiles_pca9685_set_channel_full(pca, s_button_routes[index].pca9685_channel, !lit);
}

bool tiles_buttons_init(void) {
    bool ok = tiles_pca9685_init(&s_pca1, i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_1);
    ok = tiles_pca9685_init(&s_pca2, i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_2) && ok;

    /* tiles_pca9685_init() just forced every channel on both chips to
     * "full off" (pin low), which lights these active-low-wired button
     * LEDs rather than darkening them -- correct that immediately. */
    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        set_button_led(i, false);
        s_raw_pressed[i] = false;
        s_debounced[i] = false;
        s_last_change_ms[i] = 0;
    }

    return ok;
}

void tiles_buttons_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        bool raw_pressed = !gpio_get(s_button_routes[i].gpio); /* active low */

        if (raw_pressed != s_raw_pressed[i]) {
            s_raw_pressed[i] = raw_pressed;
            s_last_change_ms[i] = now_ms;
        } else if (raw_pressed != s_debounced[i] && (now_ms - s_last_change_ms[i]) >= DEBOUNCE_MS) {
            s_debounced[i] = raw_pressed;
            set_button_led(i, s_debounced[i]);
        }
    }
}

bool tiles_button_is_pressed(uint8_t button_id) {
    if (button_id < 1u || button_id > NUM_BUTTONS) {
        return false;
    }
    return s_debounced[button_id - 1u];
}
