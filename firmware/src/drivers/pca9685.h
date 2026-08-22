#pragma once

/*
 * NXP PCA9685 16-channel, 12-bit PWM I2C LED/PWM controller.
 *
 * Register map per the real NXP datasheet (Rev 4, 16 April 2015),
 * fetched and read directly rather than recalled from memory, because
 * this chip's power-on output state is a genuine safety-relevant fact
 * on this board: the same two chips drive both the 24 active-high
 * haptic motor channels and the 6 active-low function-button LED
 * channels.
 *
 * IMPORTANT: the PCA9685's own power-on-reset default is LEDn_OFF_H[4]=1
 * ("full off") on every channel, which drives every pin LOW. That is
 * the safe/off state for active-high motor channels (pin low = motor
 * off), but for this board's active-low-wired button LEDs (pin low =
 * LED lit, per the board map), the chip's own default state actually
 * lights the LEDs, not the reverse. tiles_pca9685_init() reproduces
 * that same "every channel full-off" state explicitly (so it doesn't
 * depend on POR behavior being preserved across a soft reset), but
 * callers with active-low-wired channels MUST immediately call
 * tiles_pca9685_set_channel_full(..., full_on=true) on those specific
 * channels right after init to get them into their genuinely-dark
 * state -- see services/buttons.c.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
} tiles_pca9685_t;

/* Wakes the chip (clears MODE1.SLEEP, waits the required 500us
 * oscillator-stabilization window), sets MODE2.OUTDRV=1 (totem-pole,
 * per the hardware handoff's non-negotiable requirement -- this
 * matches the chip's own power-on default but is written explicitly
 * rather than relied upon), then forces every one of the 16 channels
 * to "full off" (pin driven low) via the ALL_LED shortcut register.
 * See the file header for why that is not the same as "every output
 * dark" on this board. Returns false on I2C failure. */
bool tiles_pca9685_init(tiles_pca9685_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Sets channel (0-15) to the PCA9685's "full on" (pin driven
 * continuously high, full_on=true) or "full off" (pin driven
 * continuously low, full_on=false) state, bypassing the 12-bit PWM
 * count registers entirely. For binary on/off outputs like button
 * LEDs, not dimmed ones. Returns false on I2C failure or an
 * out-of-range channel. */
bool tiles_pca9685_set_channel_full(tiles_pca9685_t *dev, uint8_t channel, bool full_on);

/* Sets channel (0-15) to a normal 12-bit PWM duty cycle: on_count is
 * almost always 0 (phase-aligned to the start of each cycle);
 * off_count (0-4095) is how many of the 4096 cycle ticks the pin stays
 * high before going low. Not used for button LEDs (see
 * tiles_pca9685_set_channel_full above) -- here for haptics' eventual
 * duty-cycle motor control, not built yet. Returns false on I2C
 * failure, an out-of-range channel, or a count above 4095. */
bool tiles_pca9685_set_pwm(tiles_pca9685_t *dev, uint8_t channel, uint16_t on_count, uint16_t off_count);
