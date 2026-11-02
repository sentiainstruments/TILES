#pragma once

/* Power source from GP22 (TPS2121 status) and USB mounted: the one place to
 * ask what's allowed now (budgets, haptic voices, LED ceiling, CV/gate).
 * Callbacks react to changes at once. FAULT reports the safest limits. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_POWER_MODE_USB_ONLY = 0,
    TILES_POWER_MODE_EXTERNAL_ONLY,
    TILES_POWER_MODE_USB_AND_EXTERNAL,
    TILES_POWER_MODE_FAULT, /* GP22 high while USB not mounted: invalid/transient */
} tiles_power_mode_t;

typedef struct {
    tiles_power_mode_t mode;

    /* Never automatically above 500 mA (firmware/README.md non-negotiables).
     * A validated higher USB budget would be a manual profile choice. */
    uint32_t usb_operating_budget_ma;

    /* Planning figure for the main 5 V rail: 500 (USB_ONLY/FAULT) or 2500
     * (external). Not measured. */
    uint32_t main_5v_budget_ma;

    /* Allocation ceiling, not an electrical guarantee; the per-voice duty
     * limits live in services/haptics.c. */
    uint8_t max_haptic_voices;

    /* See docs/architecture/defaults-and-safeguards.md "LED color and
     * brightness". */
    uint8_t led_brightness_ceiling_percent;

    /* True only with external power; CV and gate stay off otherwise (hardware
     * non-negotiable). */
    bool cv_gate_permitted;
} tiles_power_state_t;

void tiles_power_init(void);

/* Reads GP22 + tud_mounted(), debounces, and on a real change updates the
 * state and fires the callbacks. Call every main-loop pass; cheap. */
void tiles_power_scan(void);

/* Snapshot copy, no I/O. */
tiles_power_state_t tiles_power_get_state(void);
tiles_power_mode_t tiles_power_get_mode(void);

typedef void (*tiles_power_change_callback_t)(tiles_power_state_t new_state);

/* Fired synchronously from tiles_power_scan() on every debounced mode
 * change (FAULT included). Up to TILES_POWER_MAX_CALLBACKS, no allocation.
 * False if the table is full. */
#define TILES_POWER_MAX_CALLBACKS 4u
bool tiles_power_register_callback(tiles_power_change_callback_t callback);
