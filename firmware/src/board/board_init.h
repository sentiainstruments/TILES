#pragma once

#include "hardware/i2c.h"

/* Board bring-up: GPIO safe levels and I2C bus setup (step 1 and the bus
 * half of step 3 of the boot order in
 * docs/hardware/SENTIA_FIRMWARE_CODEX_START.md).
 *
 * Never addresses an I2C device; that is drivers/. board_i2c_recover_bus()
 * bit-bangs the raw pins without addressing anything, so it lives here
 * too (its only caller is drivers/i2c_bus.c). */

/* Configures every GPIO to its documented safe boot direction/level.
 * Must run before anything else touches a pin. Idempotent. */
void board_gpio_init(void);

/* Initializes I2C0 and I2C1 at TILES_I2C_DETECT_HZ (100kHz), the safe
 * speed for initial device discovery. Must run before any I2C traffic. */
void board_i2c_init(void);

/* Raises both buses to TILES_I2C_RUN_HZ (400 kHz). Call only after every
 * expected device has ACKed at the discovery speed. */
void board_i2c_set_run_speed(void);

/* Drives the shared PCA9685 OE (GP20) low, enabling both chips' outputs.
 * Call ONLY after every PCA9685 channel is configured (services/buttons.c,
 * haptics): the register contents go live on the pins immediately. */
void board_pca9685_enable_outputs(void);

/* Standard I2C bus recovery (NXP UM10204 3.1.16) for a device stuck
 * holding SDA mid-byte. Takes the bus pins as open-drain GPIO (only
 * driven low or released, so it can't fight a device), clocks SCL up to 9
 * times until SDA releases, sends a manual STOP either way, then hands the
 * pins back and re-runs i2c_init() at TILES_I2C_RUN_HZ, which also resets
 * the peripheral's internal state. Assumes board_i2c_set_run_speed() has
 * run (true for every caller, all scan-time). See drivers/i2c_bus.h. */
void board_i2c_recover_bus(i2c_inst_t *bus);

/* Runs board_gpio_init() then board_i2c_init(). Convenience wrapper for
 * main.c; does not raise I2C speed or touch any device. */
void board_init(void);
