#pragma once

/*
 * Power source state: derives the actual power mode from GP22 (TPS2121
 * ST) + TinyUSB's mounted state, per the exact truth table in
 * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md "Power/connection
 * states". The mux switching itself needs no firmware at all -- TPS2121
 * does that in hardware, unconditionally, with external power winning
 * whenever both are present. This module's job is purely to know which
 * state resulted, and give every other module ONE place to ask "is
 * this safe right now" instead of each re-reading GP22 and re-deriving
 * budgets/permissions itself.
 *
 * Built as infrastructure ahead of its real consumers (haptics, CV,
 * gate -- none exist yet): tiles_power_get_state() gives a live,
 * always-current snapshot (budgets, voice ceiling, brightness ceiling,
 * CV/gate permission) that a future module can just read every time it
 * needs to know, with zero GP22/debounce logic of its own.
 * tiles_power_register_callback() is the trigger half -- a future
 * module that needs to *react instantly* to a transition (e.g. CV/gate
 * must tri-state the moment external power disappears, not on its next
 * poll) subscribes once and gets called synchronously from
 * tiles_power_scan() whenever the debounced state changes. Both paths
 * are exercised now by services/lighting.c (live read) so the
 * mechanism is proven working before anything safety-critical depends
 * on it.
 *
 * FAULT mode (GP22 high while USB isn't mounted -- "invalid/transient"
 * per the hardware doc) always reports the safest possible limits
 * (USB-only budget, 0 haptic voices, CV/gate not permitted) rather than
 * requiring every consumer to special-case it -- a module that just
 * respects tiles_power_state_t's fields is automatically safe during a
 * fault without writing any fault-handling code of its own.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_POWER_MODE_USB_ONLY = 0,
    TILES_POWER_MODE_EXTERNAL_ONLY,
    TILES_POWER_MODE_USB_AND_EXTERNAL,
    TILES_POWER_MODE_FAULT, /* GP22 high while USB not mounted -- invalid/transient */
} tiles_power_mode_t;

typedef struct {
    tiles_power_mode_t mode;

    /* Never auto-selected above 500mA -- see the hard rule in
     * firmware/README.md's non-negotiables. A manually-validated higher
     * USB budget (USB_DEMO_VALIDATED_1P5A) is a future profiles/
     * concern, not derived here. */
    uint32_t usb_operating_budget_ma;

    /* Conservative estimate for the main 5V rail under this mode --
     * 500 (USB_ONLY/FAULT) or 2500 (EXTERNAL_ONLY/USB_AND_EXTERNAL),
     * per the hardware handoff's documented targets. Not a live
     * current measurement -- nothing here measures real current draw. */
    uint32_t main_5v_budget_ma;

    /* Allocation ceilings, not electrical guarantees -- a future
     * haptics current governor still has to enforce actual duty/current
     * limits underneath this. 0 until haptics exists to consume it. */
    uint8_t max_haptic_voices;

    /* Matches services/lighting.c's ceiling percentages -- see
     * docs/architecture/defaults-and-safeguards.md "LED color and
     * brightness". */
    uint8_t led_brightness_ceiling_percent;

    /* True only in EXTERNAL_ONLY / USB_AND_EXTERNAL -- CV and gate must
     * stay off otherwise, per the hardware handoff's non-negotiable. */
    bool cv_gate_permitted;
} tiles_power_state_t;

void tiles_power_init(void);

/* Reads GP22 + tud_mounted(), debounces the combined raw state, and --
 * only on an actual debounced change -- updates the live state and
 * fires every registered callback. Call every main-loop iteration;
 * cheap (one GPIO read, one TinyUSB call) when nothing is changing. */
void tiles_power_scan(void);

/* Always-current snapshot. Safe to call as often as needed -- this is
 * a plain struct copy, no I/O. */
tiles_power_state_t tiles_power_get_state(void);
tiles_power_mode_t tiles_power_get_mode(void);

typedef void (*tiles_power_change_callback_t)(tiles_power_state_t new_state);

/* Registers a callback fired synchronously from tiles_power_scan()
 * whenever the debounced mode changes (including into/out of FAULT).
 * Up to TILES_POWER_MAX_CALLBACKS listeners -- a fixed-size table, no
 * dynamic allocation. Returns false if the table is full. */
#define TILES_POWER_MAX_CALLBACKS 4u
bool tiles_power_register_callback(tiles_power_change_callback_t callback);
