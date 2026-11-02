#pragma once

#include "hardware/i2c.h"

/* Board bring-up: GPIO safe levels and I2C bus setup (boot steps 1 and 3,
 * docs/hardware/SENTIA_FIRMWARE_CODEX_START.md). Never addresses a device;
 * bus recovery bit-bangs the raw pins, so it lives here too. */

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

/* I2C bus recovery (UM10204 3.1.16) for a device holding SDA: clocks SCL
 * up to 9 times as open-drain GPIO, sends a STOP, then re-inits the bus at
 * TILES_I2C_RUN_HZ. Called from drivers/i2c_bus.c. */
void board_i2c_recover_bus(i2c_inst_t *bus);

/* Runs board_gpio_init() then board_i2c_init(). Convenience wrapper for
 * main.c; does not raise I2C speed or touch any device. */
void board_init(void);
