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
static bool s_standby_active;
static bool s_override_active[NUM_BUTTONS];

/* Public (see buttons.h) so services/haptics.c can reach the same two
 * already-woken, already-configured chip instances this file owns,
 * without re-running tiles_pca9685_init() itself (which would force
 * every channel -- including any live motor -- back to "full off"). */
tiles_pca9685_t *tiles_buttons_pca9685_for_addr(uint8_t addr) {
    if (addr == TILES_I2C1_ADDR_HAPTIC_PCA9685_1) {
        return &s_pca1;
    }
    if (addr == TILES_I2C1_ADDR_HAPTIC_PCA9685_2) {
        return &s_pca2;
    }
    return NULL;
}

static void set_button_led(uint8_t index, bool lit) {
    tiles_pca9685_t *pca = tiles_buttons_pca9685_for_addr(s_button_routes[index].pca9685_addr);
    if (pca == NULL) {
        return;
    }
    /* Active-low wiring: lit = pin driven low = PCA9685 "full off"
     * internal state; dark = pin high = "full on" internal state. */
    tiles_pca9685_set_channel_full(pca, s_button_routes[index].pca9685_channel, !lit);
}

/* Smooth brightness via the PCA9685's 12-bit PWM -- tiles_pca9685_set_pwm()'s
 * first real use in this codebase (previously only full on/off was
 * needed for buttons; haptics, its other intended caller, isn't built
 * yet). Per that function's own header comment, with on_count=0 the pin
 * goes high at the start of each cycle and off_count is how many of the
 * 4096 ticks it STAYS high before going low -- i.e. off_count is the
 * HIGH duration, not the lit duration. This board's button LEDs are
 * active-low (see set_button_led above), so HIGH = dark: the fraction
 * of the cycle spent lit (low) is (4096-off_count)/4096, which means
 * off_count = (1 - level) * 4095, not level * 4095 -- inverted from
 * what you'd guess without re-reading that comment carefully. The true
 * 0.0/1.0 endpoints go through tiles_pca9685_set_channel_full() instead
 * of off_count=0/4095, matching how it's already used elsewhere in this
 * file and sidestepping the datasheet's documented ambiguity around
 * on_count==off_count without the full-on/full-off bit set. */
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

    uint8_t channel = s_button_routes[index].pca9685_channel;

    if (level_0_to_1 <= 0.0f) {
        /* Active-low, same convention as set_button_led() above: dark =
         * pin high = full_on=true. This was backwards (full_on=false)
         * until now -- a real bug, not just untuned: it meant
         * octave_control.c's "off by default" state (level 0.0) was
         * actually driving the pin low, i.e. LIT, and its "solid"
         * magnitude-1 indicator (level 1.0, the branch below) was
         * actually driving dark. Confirmed against set_button_led()'s
         * own !lit convention two lines up. */
        tiles_pca9685_set_channel_full(pca, channel, true); /* dark */
        return;
    }
    if (level_0_to_1 >= 1.0f) {
        tiles_pca9685_set_channel_full(pca, channel, false); /* fully lit */
        return;
    }

    uint16_t off_count = (uint16_t)((1.0f - level_0_to_1) * 4095.0f);
    if (off_count < 1u) {
        off_count = 1u;
    }
    if (off_count > 4094u) {
        off_count = 4094u;
    }
    tiles_pca9685_set_pwm(pca, channel, 0u, off_count);
}

static void refresh_all_button_leds(void) {
    for (uint8_t i = 0; i < NUM_BUTTONS; i++) {
        if (s_override_active[i]) {
            /* Owned by an override controller (e.g. octave_control.c),
             * not the default press-follow behavior -- leave it alone;
             * that controller's own next scan repaints it correctly. */
            continue;
        }
        set_button_led(i, s_debounced[i]);
    }
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
            /* Debounce/press tracking above always runs -- standby.c
             * needs a live tiles_button_is_pressed() to detect a wake
             * press even while its own animation owns the LEDs, and a
             * button with an active override (see
             * tiles_buttons_set_override_active()) still needs its press
             * state tracked even though this default "LED follows
             * press" behavior is suspended for it. Only the LED write
             * itself is suppressed, by either standby or a per-button
             * override. */
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
        /* Restore default "LED follows press" immediately -- tiles_buttons_scan()
         * only writes an LED on a press/release edge, so simply clearing
         * the flag wouldn't by itself repaint whatever the override left
         * the LED showing. */
        set_button_led(index, s_debounced[index]);
    }
}

void tiles_buttons_set_override_led(uint8_t button_id, float level_0_to_1) {
    if (button_id < 1u || button_id > NUM_BUTTONS || !s_override_active[button_id - 1u]) {
        return;
    }
    /* Transparent no-op during standby -- standby.c's own animation
     * already writes every button (including this one) every frame via
     * tiles_buttons_set_standby_led(), so a controller like
     * octave_control.c can keep calling this unconditionally every scan
     * without needing to know standby exists, exactly like touch.c does
     * for tiles_lighting_set_pad_press(). The override resumes on its
     * own next scan once standby ends. */
    if (s_standby_active) {
        return;
    }
    set_button_led_level((uint8_t)(button_id - 1u), level_0_to_1);
}
