#pragma once

/*
 * Function button reading (debounced) and their PCA9685-driven LEDs.
 *
 * V1 behavior: a button's LED lights while (and only while) that button
 * is held. Debounced per the 10ms default in
 * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's scheduler defaults.
 *
 * Owns both physical PCA9685 chips. services/haptics.c's 24 motor
 * channels share these same two chips -- it reaches them via
 * tiles_buttons_pca9685_for_addr() below rather than re-running
 * tiles_pca9685_init() itself (which would force every channel back to
 * "full off," glitching any active motor).
 */

#include <stdbool.h>
#include <stdint.h>

#include "pca9685.h"

/* Wakes both PCA9685 chips (see drivers/pca9685.h -- forces every
 * channel to "full off," which lights these active-low-wired button
 * LEDs, not darkens them), then immediately corrects all 6 button-LED
 * channels to their genuinely-dark state. Returns false if either chip
 * failed I2C init. */
bool tiles_buttons_init(void);

/* Reads all 6 buttons with debounce, updates each one's LED to match
 * its debounced pressed state. Call every main-loop iteration. */
void tiles_buttons_scan(void);

/* Debounced pressed state for button id 1-6 (SW1-SW6: left capsule,
 * right capsule, triangle, diamond, square, circle). False if out of
 * range. */
bool tiles_button_is_pressed(uint8_t button_id);

/* ---- Standby animation hooks (services/standby.c) ----------------------
 *
 * true: tiles_buttons_scan()'s normal "LED follows press" behavior stops
 * touching the LEDs (button reads/debounce/tiles_button_is_pressed()
 * keep working normally -- standby still needs to detect a press to wake
 * up), leaving them free for tiles_buttons_set_standby_led() below.
 * false: immediately re-asserts every button's LED from its current
 * debounced state -- needed because tiles_buttons_scan() only writes an
 * LED on a press/release edge, so simply clearing the flag wouldn't by
 * itself repaint an LED animation left mid-brightness. */
void tiles_buttons_set_standby_active(bool active);

/* Sets button id 1-6's LED to a smooth 0.0 (dark) - 1.0 (fully lit)
 * brightness via the PCA9685's 12-bit PWM, instead of the on/off-only
 * path tiles_buttons_scan() normally uses. No-op unless standby is
 * active. */
void tiles_buttons_set_standby_led(uint8_t button_id, float level_0_to_1);

/* ---- Per-button LED override (e.g. services/octave_control.c) ----------
 *
 * For a button whose default "LED follows press" behavior has been
 * replaced by some other persistent function (e.g. the octave-shift
 * indicator on SW1/SW2) -- distinct from the standby hooks above, which
 * apply to all 6 buttons at once only while idle; this applies to one
 * button at a time, at any time, including during normal operation.
 *
 * true: tiles_buttons_scan()'s normal "LED follows press" behavior stops
 * touching this button's LED (press/release tracking and
 * tiles_button_is_pressed() keep working normally), leaving it free for
 * tiles_buttons_set_override_led() below. false: immediately re-asserts
 * the LED from its current debounced state, same reasoning as
 * tiles_buttons_set_standby_active()'s false case. */
void tiles_buttons_set_override_active(uint8_t button_id, bool active);

/* Sets one overridden button's LED to a smooth 0.0-1.0 brightness.
 * No-op unless that button's override is active. Also a transparent
 * no-op while standby is active -- standby's own animation already
 * writes every button each frame, so a controller can call this every
 * scan unconditionally without needing to know standby exists (same
 * pattern touch.c relies on for tiles_lighting_set_pad_press()); the
 * override's own next scan after standby ends repaints it correctly. */
void tiles_buttons_set_override_led(uint8_t button_id, float level_0_to_1);

/* ---- Shared-resource accessor (services/haptics.c) ----------------------
 *
 * Returns the already-initialized tiles_pca9685_t for
 * TILES_I2C1_ADDR_HAPTIC_PCA9685_1 or _2, or NULL for any other address.
 * Callers must not call tiles_pca9685_init() on the result -- see the
 * file header above. */
tiles_pca9685_t *tiles_buttons_pca9685_for_addr(uint8_t addr);
