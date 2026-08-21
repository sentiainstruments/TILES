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
