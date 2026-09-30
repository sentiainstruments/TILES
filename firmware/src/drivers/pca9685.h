#pragma once

/* NXP PCA9685 16-channel, 12-bit PWM controller (datasheet Rev 4, 2015).
 * The same two chips drive the 24 active-high haptic motors and the 6
 * active-low button LEDs, so the power-on output state matters.
 *
 * IMPORTANT: the POR default is "full off" on every channel (pin LOW).
 * That turns motors off but LIGHTS the active-low button LEDs.
 * tiles_pca9685_init() sets the same state explicitly; callers with
 * active-low channels must then call tiles_pca9685_set_channel_full(...,
 * true) on them to make them dark (see services/buttons.c). */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
} tiles_pca9685_t;

/* Wakes the chip (with the 500 us oscillator wait), sets MODE2.OUTDRV=1
 * (totem-pole, required by the hardware), then sets all 16 channels full
 * off (pin low). False on I2C failure. */
bool tiles_pca9685_init(tiles_pca9685_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Sets a channel (0-15) to full on (pin high) or full off (pin low),
 * bypassing PWM. For binary outputs like button LEDs. False on I2C failure
 * or a bad channel. */
bool tiles_pca9685_set_channel_full(tiles_pca9685_t *dev, uint8_t channel, bool full_on);

/* Sets a channel (0-15) to a 12-bit PWM duty: on_count is usually 0,
 * off_count (0-4095) is the ticks the pin stays high per 4096-tick cycle.
 * False on I2C failure, a bad channel or a count above 4095. */
bool tiles_pca9685_set_pwm(tiles_pca9685_t *dev, uint8_t channel, uint16_t on_count, uint16_t off_count);
