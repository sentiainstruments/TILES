#include "power.h"

#include "board_pins.h"

#include "hardware/gpio.h"
#include "pico/time.h"
#include "tusb.h"

#include <stddef.h>

/* Debounce for the combined (GP22, mounted) state. GP22 is a push-pull
 * output and doesn't bounce, but the mux can read ambiguously mid-switch;
 * 50 ms is a margin, not a measured settling time. */
#define TILES_POWER_DEBOUNCE_MS 50u

static tiles_power_mode_t raw_mode_from_pins(void) {
    bool external_selected = !gpio_get(TILES_GPIO_POWER_SOURCE_STATUS);
    bool usb_mounted = tud_mounted();

    /* The truth table from SENTIA_TILES_FIRMWARE_HANDOFF.md "Power/connection
     * states": all four combinations are meaningful. */
    if (external_selected) {
        return usb_mounted ? TILES_POWER_MODE_USB_AND_EXTERNAL : TILES_POWER_MODE_EXTERNAL_ONLY;
    }
    return usb_mounted ? TILES_POWER_MODE_USB_ONLY : TILES_POWER_MODE_FAULT;
}

/* Budgets from the handoff's profile table and
 * docs/architecture/defaults-and-safeguards.md. USB-only mirrors
 * USB_DEMO_SAFE (500 mA); external mirrors FULL_DEMO_EXTERNAL (2.5 A,
 * CV/gate allowed). USB_DEMO_VALIDATED_1P5A is a manual override, not
 * derived here.
 *
 * External LED ceiling is 90%: worst case ~448 mA LEDs + ~220 mA MCU/ICs +
 * a pessimistic 300-400 mA haptics still leaves ~1.8 A of margin (full
 * breakdown in services/lighting.c). USB-only stays at 37%: the same
 * accounting leaves no confirmed margin in 500 mA. Motor current is the
 * biggest unmeasured number. */
static tiles_power_state_t state_for_mode(tiles_power_mode_t mode) {
    tiles_power_state_t s = {0};
    s.mode = mode;

    switch (mode) {
    case TILES_POWER_MODE_EXTERNAL_ONLY:
        s.usb_operating_budget_ma = 0u;
        s.main_5v_budget_ma = 2500u;
        s.max_haptic_voices = 12u;
        s.led_brightness_ceiling_percent = 90u;
        s.cv_gate_permitted = true;
        break;

    case TILES_POWER_MODE_USB_AND_EXTERNAL:
        s.usb_operating_budget_ma = 500u;
        s.main_5v_budget_ma = 2500u;
        s.max_haptic_voices = 12u;
        s.led_brightness_ceiling_percent = 90u;
        s.cv_gate_permitted = true;
        break;

    case TILES_POWER_MODE_USB_ONLY:
        s.usb_operating_budget_ma = 500u;
        s.main_5v_budget_ma = 500u;
        /* 4 voices: at an estimated 80-100 mA per small ERM motor, 5 voices alone
         * could take the whole 500 mA budget. Conservative until motor current is
         * measured (the handoff expects a current governor on top of this). */
        s.max_haptic_voices = 4u;
        s.led_brightness_ceiling_percent = 37u;
        s.cv_gate_permitted = false;
        break;

    case TILES_POWER_MODE_FAULT:
    default:
        /* "Fail outputs off and report a power fault": 0 haptic voices and no
         * CV/gate. LEDs stay at the USB-safe ceiling rather than going dark; a
         * fault is most likely a brief enumeration transient. */
        s.usb_operating_budget_ma = 500u;
        s.main_5v_budget_ma = 500u;
        s.max_haptic_voices = 0u;
        s.led_brightness_ceiling_percent = 37u;
        s.cv_gate_permitted = false;
        break;
    }

    return s;
}

static tiles_power_state_t s_state;

static bool s_pending_valid;
static tiles_power_mode_t s_pending_mode;
static uint32_t s_pending_since_ms;

static tiles_power_change_callback_t s_callbacks[TILES_POWER_MAX_CALLBACKS];
static size_t s_callback_count;

void tiles_power_init(void) {
    /* Seed from one immediate read, so the state is right from the first
     * frame (lighting reads it during init). */
    s_state = state_for_mode(raw_mode_from_pins());
    s_pending_valid = false;
    s_callback_count = 0;
    for (size_t i = 0; i < TILES_POWER_MAX_CALLBACKS; i++) {
        s_callbacks[i] = NULL;
    }
}

void tiles_power_scan(void) {
    tiles_power_mode_t raw = raw_mode_from_pins();

    if (raw == s_state.mode) {
        s_pending_valid = false;
        return;
    }

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    if (!s_pending_valid || s_pending_mode != raw) {
        s_pending_mode = raw;
        s_pending_since_ms = now_ms;
        s_pending_valid = true;
        return;
    }

    if (now_ms - s_pending_since_ms < TILES_POWER_DEBOUNCE_MS) {
        return;
    }

    s_state = state_for_mode(raw);
    s_pending_valid = false;

    for (size_t i = 0; i < s_callback_count; i++) {
        s_callbacks[i](s_state);
    }
}

tiles_power_state_t tiles_power_get_state(void) {
    return s_state;
}

tiles_power_mode_t tiles_power_get_mode(void) {
    return s_state.mode;
}

bool tiles_power_register_callback(tiles_power_change_callback_t callback) {
    if (callback == NULL || s_callback_count >= TILES_POWER_MAX_CALLBACKS) {
        return false;
    }
    s_callbacks[s_callback_count] = callback;
    s_callback_count++;
    return true;
}
