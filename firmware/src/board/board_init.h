#pragma once

#include "hardware/i2c.h"

/*
 * Board-level bring-up: raw GPIO directions/safe levels and I2C bus
 * setup. This is step 1 (and the bus half of step 3) of the safe boot
 * order in docs/hardware/SENTIA_FIRMWARE_CODEX_START.md.
 *
 * What this module does NOT do: it does not talk to any I2C device by
 * address (Hall muxes, LED mux controller, PCA9685s, DAC) -- that's
 * drivers/'s job, addr and all. board_i2c_init()/board_i2c_set_run_
 * speed() only get the raw pins and buses into a state where probing
 * those devices is safe. board_i2c_recover_bus() is the one exception,
 * and only barely: it never addresses a device either, it just takes
 * the bus's SDA/SCL pins back from the I2C peripheral for a moment to
 * bit-bang them directly -- still a raw-pins operation, not a device
 * transaction, so it belongs here and not in drivers/i2c_bus.c (its
 * only caller).
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

/* Real feedback chasing why a USB bus-reset event kept correlating with
 * full-MCU freezes even though "even a usb data issue shoudnt cause the
 * crash" -- the actual mechanism turned out to be a separate, real gap
 * in how this codebase (and pico-sdk itself) handles a genuinely wedged
 * I2C bus, not the USB event itself; see drivers/i2c_bus.h for the full
 * chain. This is the missing piece drivers/pca9685.c's own comment
 * already named but never implemented: "true I2C bus recovery needs a
 * bit-bang clock-pulse sequence."
 *
 * Standard I2C bus-recovery sequence (NXP UM10204 3.1.16 / the classic
 * embedded-I2C fix for a slave stuck holding SDA low mid-byte): takes
 * over `bus`'s SDA/SCL pins as plain open-drain-style GPIO (only ever
 * driven low or released to float+pull-up, exactly like real I2C, so
 * this can't fight another device even mid-recovery), pulses SCL up to
 * 9 times -- enough to walk a stuck slave through one full byte + ACK --
 * watching for SDA to release after each one, then drives a manual STOP
 * condition (SDA released high while SCL is high) regardless of whether
 * SDA ever freed up, then restores I2C peripheral function on both pins
 * and re-runs i2c_init() at TILES_I2C_RUN_HZ, which also resets the
 * peripheral's own internal state (FIFOs included) via the RESETS
 * block, independent of whatever the external wire state was.
 *
 * Assumes board_i2c_set_run_speed() has already run -- true for every
 * real caller (drivers/i2c_bus.c, reached only from scan-time chip
 * traffic, which by construction never happens before boot has already
 * raised both buses to run speed). Safe to call on either bus at any
 * point after that. */
void board_i2c_recover_bus(i2c_inst_t *bus);

/* Runs board_gpio_init() then board_i2c_init(). Convenience wrapper for
 * main.c; does not raise I2C speed or touch any device. */
void board_init(void);
