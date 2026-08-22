#pragma once

/*
 * Generic I2C bus enumeration: confirms every device we expect to find
 * on I2C0/I2C1 actually ACKs its address. Pure bus-level probing, no
 * per-chip protocol knowledge -- that's drivers/, not this.
 *
 * This is the "I2C discovery with every output forced off" phase from
 * SENTIA_FIRMWARE_CODEX_START.md: run after board_init() and before
 * board_i2c_set_run_speed() or any driver init.
 */

#include <stdbool.h>

/* Probes every address in board_pins.h's I2C0/I2C1 device list and
 * prints one pass/fail line per device over stdio. Returns true only if
 * every expected device ACKed. Safe to call repeatedly -- it never
 * writes device state, only tests for an ACK. */
bool tiles_diag_i2c_scan_expected_devices(void);

/* Probes every valid 7-bit address (0x08-0x77, skipping reserved
 * addresses) on both I2C0 and I2C1 and prints every one that ACKs,
 * expected or not. For diagnosing a device that isn't answering at its
 * expected address -- e.g. a mis-set hardware address strap -- rather
 * than for routine bring-up. */
void tiles_diag_i2c_full_scan(void);
