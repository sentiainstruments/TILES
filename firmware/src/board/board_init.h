#pragma once

/*
 * Board-level bring-up: raw GPIO directions/safe levels and I2C bus
 * setup. This is step 1 (and the bus half of step 3) of the safe boot
 * order in docs/hardware/SENTIA_FIRMWARE_CODEX_START.md.
 *
 * What this module does NOT do: it does not touch any I2C device (Hall
 * muxes, LED mux controller, PCA9685s, DAC) -- that requires drivers/
 * which don't exist yet. board_init() only gets the raw pins and buses
 * into a state where probing those devices is safe.
 */

/* Configures every GPIO to its documented safe boot direction/level.
 * Must run before anything else touches a pin. Idempotent. */
void board_gpio_init(void);

/* Initializes I2C0 and I2C1 at TILES_I2C_DETECT_HZ (100kHz), the safe
 * speed for initial device discovery. Must run before any I2C traffic. */
void board_i2c_init(void);

/* Raises both I2C buses to TILES_I2C_RUN_HZ (400kHz). Call only after
 * I2C discovery has confirmed every expected device ACKs at the lower
 * speed -- see the boot order in the firmware bring-up docs. */
void board_i2c_set_run_speed(void);

/* Drives the PCA9685 shared OE pin (GP20 -- see board_pins.h) low,
 * enabling both chips' outputs. Call ONLY after every PCA9685 channel
 * has already been configured to its intended state (services/buttons.c
 * and, later, haptics) -- enabling OE makes each chip's current
 * register content immediately live on its physical output pins. Not
 * safe to call before that configuration has happened. */
void board_pca9685_enable_outputs(void);

/* Runs board_gpio_init() then board_i2c_init(). Convenience wrapper for
 * main.c; does not raise I2C speed or touch any device. */
void board_init(void);
