#pragma once

/* Function buttons (debounced, 10 ms per the handoff's scheduler defaults)
 * and their PCA9685-driven LEDs. By default a button's LED is lit while
 * it is held; standby and per-button overrides can take the LEDs over.
 *
 * Owns both PCA9685 chips. services/haptics.c's 24 motor channels share
 * them and get them through tiles_buttons_pca9685_for_addr(); calling
 * tiles_pca9685_init() again would force every channel off and glitch
 * running motors. */

#include <stdbool.h>
#include <stdint.h>

#include "pca9685.h"

/* Wakes both PCA9685s (init sets every channel "full off", which LIGHTS
 * the active-low button LEDs; see drivers/pca9685.h), then turns the 6
 * button LEDs dark. False if either chip failed init. */
bool tiles_buttons_init(void);

/* Reads all 6 buttons with debounce and updates each default-mode LED.
 * Call every main-loop pass. */
void tiles_buttons_scan(void);

/* Debounced state for button 1-6 (SW1-SW6: "-", "+", triangle, diamond,
 * square, circle). False if out of range. */
bool tiles_button_is_pressed(uint8_t button_id);

/* ---- Standby animation hooks (services/standby.c) ----------------------
 *
 * true: tiles_buttons_scan() stops writing LEDs (button state keeps
 * updating, since standby needs a press to wake), leaving them to
 * tiles_buttons_set_standby_led(). false: repaints every LED from its
 * current state at once (scan only writes on edges, so clearing the flag
 * alone would leave the animation's last frame). */
void tiles_buttons_set_standby_active(bool active);

/* Sets button 1-6's LED to 0.0 (dark) - 1.0 (lit) via 12-bit PWM. No-op
 * unless standby is active. */
void tiles_buttons_set_standby_led(uint8_t button_id, float level_0_to_1);

/* ---- Per-button LED override (e.g. services/octave_control.c) ----------
 *
 * For a button whose LED shows something else (e.g. the octave
 * indicator on "-"/"+"). Per button and at any time, unlike standby.
 *
 * true: scan stops writing this LED (press state still tracked), leaving
 * it to tiles_buttons_set_override_led(). false: repaints it from its
 * current state at once. */
void tiles_buttons_set_override_active(uint8_t button_id, bool active);

/* Sets an overridden button's LED to 0.0-1.0. No-op unless overridden,
 * and silently ignored during standby (standby redraws every button each
 * frame), so callers can call it every scan without knowing about
 * standby; the next call after standby repaints it. */
void tiles_buttons_set_override_led(uint8_t button_id, float level_0_to_1);

/* ---- Shared chips (services/haptics.c) -----------------------------------
 *
 * The initialized tiles_pca9685_t for TILES_I2C1_ADDR_HAPTIC_PCA9685_1 or
 * _2, else NULL. Never call tiles_pca9685_init() on it (see above). */
tiles_pca9685_t *tiles_buttons_pca9685_for_addr(uint8_t addr);

/* ---- Power-transition recovery (main.c) ---------------------------------
 *
 * Both PCA9685s sit on a rail the power-source switch disturbs: a glitch
 * can silently reset their configuration without resetting the RP2350
 * (pulling the power plug once killed haptics this way). Re-runs the boot
 * configuration on both chips and repaints the default-mode button LEDs
 * (standby/override LEDs are redrawn by their owners every frame). Called
 * from main.c's power-change callback, followed by
 * tiles_haptics_resync_hardware(): the chips must be configured first. */
void tiles_buttons_resync_pca9685(void);
