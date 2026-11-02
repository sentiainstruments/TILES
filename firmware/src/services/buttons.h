#pragma once

/* Function keys (debounced 10 ms) and their LEDs, lit while held unless
 * standby or an override takes them. Owns both PCA9685s: haptics.c gets
 * them via tiles_buttons_pca9685_for_addr() (re-initializing would glitch
 * running motors). */

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

/* ---- Standby animation hooks ----------------------------------------------
 * true: scan stops writing LEDs (keys still read) for
 * tiles_buttons_set_standby_led(). false: repaints every LED at once. */
void tiles_buttons_set_standby_active(bool active);

/* Sets button 1-6's LED to 0.0 (dark) - 1.0 (lit) via 12-bit PWM. No-op
 * unless standby is active. */
void tiles_buttons_set_standby_led(uint8_t button_id, float level_0_to_1);

/* ---- Per-key LED override (e.g. the octave indicator) ---------------------
 * true: scan stops writing this LED for tiles_buttons_set_override_led().
 * false: repaints it at once. */
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

/* LED intensity (Feel menu row 4, services/lighting.c) for every button
 * LED, 0 (off) to 1 (as drawn). Redraws them all when it changes. */
void tiles_buttons_set_led_intensity(float intensity_0_to_1);

/* ---- Power-transition recovery ------------------------------------------
 * A power switch can reset both PCA9685s without the RP2350 (it once killed
 * haptics). Re-runs their setup and repaints the key LEDs; called before
 * tiles_haptics_resync_hardware(). */
void tiles_buttons_resync_pca9685(void);
