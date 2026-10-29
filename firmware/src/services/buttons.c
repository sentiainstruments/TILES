#include "buttons.h"

#include "board_pins.h"
#include "pca9685.h"

#include "hardware/gpio.h"
#include "pico/time.h"

#define NUM_BUTTONS 6u
#define DEBOUNCE_MS 10u /* handoff scheduler_defaults: button_debounce_ms */

typedef struct {
    uint gpio;
    uint8_t pca9685_addr;
    uint8_t pca9685_channel;
} button_route_t;

/* Left to right: "-", "+", triangle, diamond, square, circle (handoff
 * buttons table). */
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
static bool s_standby_active;
static bool s_override_active[NUM_BUTTONS];

/* Shared with services/haptics.c (see buttons.h). */
tiles_pca9685_t *tiles_buttons_pca9685_for_addr(uint8_t addr) {
    if (addr == TILES_I2C1_ADDR_HAPTIC_PCA9685_1) {
        return &s_pca1;
    }
    if (addr == TILES_I2C1_ADDR_HAPTIC_PCA9685_2) {
        return &s_pca2;
    }
    return NULL;
}

/* What each LED's channel was last set to, so an unchanged level costs no
 * I2C: modes redraw every LED every pass, and each write is 4 register
 * transactions on the bus the Hall sensors and haptics share. Codes: PWM
 * off_count 1-4094, or LED_CODE_DARK / LED_CODE_LIT; LED_CODE_UNKNOWN after
 * the PCA9685s are (re)initialised. */
#define LED_CODE_UNKNOWN (-1)
#define LED_CODE_DARK 5000
#define LED_CODE_LIT 5001
static int16_t s_led_code[NUM_BUTTONS];

static void forget_led_codes(void) {
    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        s_led_code[i] = LED_CODE_UNKNOWN;
    }
}

static void set_button_led(uint8_t index, bool lit) {
    tiles_pca9685_t *pca = tiles_buttons_pca9685_for_addr(s_button_routes[index].pca9685_addr);
    int16_t code = lit ? LED_CODE_LIT : LED_CODE_DARK;
    if (pca == NULL || s_led_code[index] == code) {
        return;
    }
    /* Active low: lit = pin low = "full off"; dark = pin high = "full on". */
    if (tiles_pca9685_set_channel_full(pca, s_button_routes[index].pca9685_channel, !lit)) {
        s_led_code[index] = code;
    } else {
        s_led_code[index] = LED_CODE_UNKNOWN;
    }
}

/* Smooth brightness via 12-bit PWM. With on_count=0, off_count is how long
 * the pin stays HIGH, and these LEDs are active low (high = dark), so
 * off_count = (1 - level) * 4095. Exact 0.0/1.0 use the full on/off bits,
 * avoiding the datasheet's on_count==off_count ambiguity. */
static void set_button_led_level(uint8_t index, float level_0_to_1) {
    tiles_pca9685_t *pca = tiles_buttons_pca9685_for_addr(s_button_routes[index].pca9685_addr);
    if (pca == NULL) {
        return;
    }
    if (level_0_to_1 < 0.0f) {
        level_0_to_1 = 0.0f;
    }
    if (level_0_to_1 > 1.0f) {
        level_0_to_1 = 1.0f;
    }

    /* 256 steps: smooth to the eye, and a slow pulse changes the code (and
     * writes) far less often than every pass. */
    level_0_to_1 = (float)(int)(level_0_to_1 * 255.0f + 0.5f) / 255.0f;

    if (level_0_to_1 <= 0.0f) {
        set_button_led(index, false); /* dark */
        return;
    }
    if (level_0_to_1 >= 1.0f) {
        set_button_led(index, true); /* fully lit */
        return;
    }

    uint16_t off_count = (uint16_t)((1.0f - level_0_to_1) * 4095.0f);
    if (off_count < 1u) {
        off_count = 1u;
    }
    if (off_count > 4094u) {
        off_count = 4094u;
    }
    if (s_led_code[index] == (int16_t)off_count) {
        return;
    }
    uint8_t channel = s_button_routes[index].pca9685_channel;
    s_led_code[index] = tiles_pca9685_set_pwm(pca, channel, 0u, off_count) ? (int16_t)off_count : LED_CODE_UNKNOWN;
}

static void refresh_all_button_leds(void) {
    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        if (s_override_active[i]) {
            /* Owned by an override controller; it repaints its own LED. */
            continue;
        }
        set_button_led(i, s_debounced[i]);
    }
}

void tiles_buttons_resync_pca9685(void) {
    /* Return values ignored, as at boot: there is no fallback, and a failure
     * was already reported at init. */
    (void)tiles_pca9685_init(&s_pca1, i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_1);
    (void)tiles_pca9685_init(&s_pca2, i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_2);
    forget_led_codes(); /* init changed every channel behind the cache */
    /* Init set every channel "full off" (lit here): restore the default-mode
     * LEDs now; override/standby LEDs repaint on their owners' next frame. */
    refresh_all_button_leds();
}

bool tiles_buttons_init(void) {
    bool ok = tiles_pca9685_init(&s_pca1, i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_1);
    ok = tiles_pca9685_init(&s_pca2, i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_2) && ok;
    forget_led_codes();

    /* Init set every channel "full off", which lights these active-low LEDs:
     * turn them dark. */
    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        set_button_led(i, false);
        s_raw_pressed[i] = false;
        s_debounced[i] = false;
        s_last_change_ms[i] = 0;
        s_override_active[i] = false;
    }
    s_standby_active = false;

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
            /* Press tracking always runs (standby needs wake presses, overridden
             * buttons still report presses); only the LED write is suppressed. */
            if (!s_standby_active && !s_override_active[i]) {
                set_button_led(i, s_debounced[i]);
            }
        }
    }
}

bool tiles_button_is_pressed(uint8_t button_id) {
    if (button_id < 1u || button_id > NUM_BUTTONS) {
        return false;
    }
    return s_debounced[button_id - 1u];
}

void tiles_buttons_set_standby_active(bool active) {
    s_standby_active = active;
    if (!active) {
        refresh_all_button_leds();
    }
}

void tiles_buttons_set_standby_led(uint8_t button_id, float level_0_to_1) {
    if (!s_standby_active || button_id < 1u || button_id > NUM_BUTTONS) {
        return;
    }
    set_button_led_level((uint8_t)(button_id - 1u), level_0_to_1);
}

void tiles_buttons_set_override_active(uint8_t button_id, bool active) {
    if (button_id < 1u || button_id > NUM_BUTTONS) {
        return;
    }
    uint8_t index = (uint8_t)(button_id - 1u);
    s_override_active[index] = active;
    if (!active) {
        /* Repaint now: scan only writes on edges. */
        set_button_led(index, s_debounced[index]);
    }
}

void tiles_buttons_set_override_led(uint8_t button_id, float level_0_to_1) {
    if (button_id < 1u || button_id > NUM_BUTTONS || !s_override_active[button_id - 1u]) {
        return;
    }
    /* Ignored during standby, which redraws every button each frame; see
     * tiles_buttons_set_override_led() in buttons.h. */
    if (s_standby_active) {
        return;
    }
    set_button_led_level((uint8_t)(button_id - 1u), level_0_to_1);
}
