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
