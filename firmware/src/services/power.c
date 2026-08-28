#include "power.h"

#include "board_pins.h"

#include "hardware/gpio.h"
#include "pico/time.h"
#include "tusb.h"

#include <stddef.h>

/* Debounce window for the combined (GP22, tud_mounted()) raw state.
 * GP22 is a push-pull logic output from the TPS2121, not a mechanical
 * contact -- it won't bounce the way a button does -- but the mux
 * itself needs some settling time during an actual source transition
 * (unplug/replug), during which a reading could be transiently
 * ambiguous. 50ms is a safety margin against acting on that transient,
 * not a measured settling time. */
#define TILES_POWER_DEBOUNCE_MS 50u

static tiles_power_mode_t raw_mode_from_pins(void) {
    bool external_selected = !gpio_get(TILES_GPIO_POWER_SOURCE_STATUS);
    bool usb_mounted = tud_mounted();

    /* Exactly the truth table in
     * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md "Power/connection
     * states" -- all four (GP22, mounted) combinations are meaningful,
     * none left as "shouldn't happen". */
    if (external_selected) {
        return usb_mounted ? TILES_POWER_MODE_USB_AND_EXTERNAL : TILES_POWER_MODE_EXTERNAL_ONLY;
    }
    return usb_mounted ? TILES_POWER_MODE_USB_ONLY : TILES_POWER_MODE_FAULT;
}

/* Budgets/ceilings below are transcribed from the same handoff doc's
 * named-profile table and docs/architecture/defaults-and-safeguards.md
 * "LED color and brightness" -- not invented here. USB-only mirrors
 * USB_DEMO_SAFE (500mA, max 5 haptic voices, 35-40% LED ceiling);
 * external mirrors FULL_DEMO_EXTERNAL (2.5A 5V target, max 12 voices,
 * originally 70-80% LED ceiling, CV/gate permitted). USB_DEMO_VALIDATED_1P5A
 * is a manual-only override with no automatic trigger and isn't derived
 * here -- a future profiles/ module owns that choice, not this one.
 *
 * External's ceiling raised 75 -> 90, real feedback: "calculate the safe
 * range again to make sure, acountign for ics lights and haptics and
 * sensors." Fuller accounting (see services/lighting.c's own "Pad
 * brightness ceiling" section for the full breakdown): ~448mA LED worst
 * case + ~220mA estimated MCU/sensor/IC/button-LED overhead + a
 * pessimistic 300-400mA haptics worst case (motor current itself is
 * genuinely unmeasured, both hardware docs flag this) still leaves
 * roughly 1.8A of margin against the 2500mA budget here -- real headroom
 * to raise, unlike USB-only below. USB-only's 37% is UNCHANGED by that
 * same accounting: 500mA total minus that same ~220mA overhead minus a
 * similarly pessimistic haptics worst case leaves little to no confirmed
 * margin beyond the existing ceiling, so it wasn't raised. Haptic motor
 * current is the actual highest-priority unknown to measure here, not
 * LED brightness on either profile. */
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
        /* Lowered 5 -> 3, real feedback: "we might have gotten to close
         * to max draw in usb mode." Motor current is genuinely
         * unmeasured (see services/lighting.c's own fuller budget
         * breakdown), but a rough typical-small-ERM-motor estimate
         * (~80-100mA running each) puts 5 simultaneous voices alone at
         * 400-500mA -- potentially the ENTIRE USB budget before the
         * ~220mA of estimated MCU/sensor/IC/button-LED overhead or any
         * LED brightness is even counted. 3 voices at that same estimate
         * (~240-300mA) leaves real margin for that overhead instead of
         * assuming it away. Still a conservative estimate pending real
         * motor-current measurement, not a precise number -- the
         * hardware handoff doc's own "Five voices is an allocation
         * ceiling; a current governor must still reduce duty or
         * concurrent starts" already anticipated needing exactly this
         * kind of further tightening. */
        s.max_haptic_voices = 3u;
        s.led_brightness_ceiling_percent = 37u;
        s.cv_gate_permitted = false;
        break;

    case TILES_POWER_MODE_FAULT:
    default:
        /* "Fail outputs off and report a power fault" -- 0 haptic
         * voices and no CV/gate already achieve "outputs off" for any
         * future consumer that just respects these fields. LED ceiling
         * stays at the USB-safe value rather than going dark: lighting
         * isn't a safety concern and a fault here is most likely a
         * brief boot/enumeration transient, not a reason to blank the
         * board. */
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
    /* Seeds from a single immediate read rather than starting in some
     * default mode and waiting out the debounce window -- boot-time
     * state should be correct from the first frame, since lighting
     * reads it during its own init sweep. */
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
