#pragma once

/*
 * Function button reading (debounced) and their PCA9685-driven LEDs.
 *
 * V1 behavior: a button's LED lights while (and only while) that button
 * is held. Debounced per the 10ms default in
 * docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md's scheduler defaults.
 *
 * Owns both physical PCA9685 chips. Haptics (24 motor channels, not
 * built yet) will need to talk to these same two chips -- when that's
 * built, it needs an accessor into these already-initialized instances
 * rather than re-running tiles_pca9685_init() (which would force every
 * channel back to "full off," glitching any active motor). Flagging
 * this ownership question here rather than building a shared-resource
 * abstraction now that nothing else needs yet.
 */

#include <stdbool.h>
#include <stdint.h>

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
