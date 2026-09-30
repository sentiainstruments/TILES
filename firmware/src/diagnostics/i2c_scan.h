#pragma once

/* I2C bus enumeration: checks that every expected device on I2C0/I2C1
 * ACKs. Bus-level only; chip protocols are drivers/.
 *
 * The "I2C discovery with every output forced off" boot phase
 * (SENTIA_FIRMWARE_CODEX_START.md): after board_init(), before
 * board_i2c_set_run_speed() and any driver init. */

#include <stdbool.h>

/* Probes every I2C0/I2C1 device in board_pins.h and prints pass/fail per
 * device. True only if all ACKed. Never writes device state. */
bool tiles_diag_i2c_scan_expected_devices(void);

/* Probes every valid 7-bit address (0x08-0x77) on both buses and prints
 * each one that ACKs. For finding a device at an unexpected address. */
void tiles_diag_i2c_full_scan(void);
