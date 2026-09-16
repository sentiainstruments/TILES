#pragma once

#include "hardware/i2c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Shared, bounded I2C transaction wrapper for every chip driver in this
 * directory -- replaces each driver's own direct i2c_write_timeout_us()/
 * i2c_read_timeout_us() calls (and its own locally-duplicated
 * TILES_I2C_TIMEOUT_US) with one shared implementation, for two reasons
 * that go beyond just deduplicating a #define:
 *
 * 1. pico-sdk 2.3.0's own i2c_read_timeout_us() (hardware_i2c/i2c.c's
 *    i2c_read_blocking_internal()) has a real gap: before it ever gets
 *    to the timeout-checked part of the transaction, it first waits for
 *    TX FIFO room to submit the read-request byte --
 *        while (!i2c_get_write_available(i2c)) tight_loop_contents();
 *    (hardware/i2c.h:430, inlined into that same internal read path) --
 *    and that ONE wait passes no timeout_check at all, regardless of
 *    what timeout the caller asked for. If the TX FIFO is ever left
 *    stuck full -- which a bus already wedged by an earlier failed
 *    transaction (see point 2) will do -- this call hangs forever,
 *    silently reintroducing the exact "hang forever" failure this whole
 *    file's timeout convention exists to prevent (see drivers/pca9685.c's
 *    own long-standing comment on that history), just for reads
 *    specifically, and just once the bus is already in trouble.
 *    Confirmed directly against this project's own vendored pico-sdk
 *    (tag 2.3.0) source, not assumed. tiles_i2c_read() below closes this
 *    by checking i2c_get_write_available() itself first, against its OWN
 *    deadline, before ever calling into the SDK's read function -- so
 *    that specific unbounded wait is never reached in a stuck state from
 *    this codebase's own code.
 *
 * 2. Neither i2c_write_timeout_us() nor i2c_read_timeout_us() cleans up
 *    the I2C peripheral on a timeout (as opposed to a hardware-reported
 *    abort) -- pico-sdk's own internal timeout path deliberately skips
 *    reading/clearing the abort-source register and skips waiting for a
 *    STOP condition (there's nothing safe to clean up if the transaction
 *    may still be live on the wire), but with the side effect that
 *    whatever originally wedged the bus is never actually cleared --
 *    exactly the gap drivers/pca9685.c's own comment already flagged:
 *    "true I2C bus recovery needs a bit-bang clock-pulse sequence this
 *    driver doesn't have." tiles_i2c_write()/tiles_i2c_read() close this
 *    too: on ANY failure, either one calls board_i2c_recover_bus() before
 *    returning, so the bus is back in a known-good state before the NEXT
 *    attempt on ANY device on that bus, rather than staying wedged until
 *    the watchdog eventually resets the whole MCU.
 *
 * Real feedback that prompted going this far rather than trusting the
 * existing per-driver timeouts to already be enough: "even a usb data
 * issue shoudnt cause the crash" -- correct, and it doesn't: the USB
 * bus-reset events already found correlating with several real freezes
 * (see services/README.md's two-board comparison test entry) are best
 * understood as a probably-coincidental symptom of the same underlying
 * electrical event that ALSO wedges an I2C bus, not a cause of the
 * freeze by itself. This file is the fix for the I2C half of that,
 * independent of whatever that shared root trigger turns out to be.
 */

/* addr/src/len/nostop match i2c_write_timeout_us()'s own signature.
 * Returns true iff every byte was acknowledged; on ANY failure, recovers
 * the bus (see board_i2c_recover_bus()) before returning false, so a
 * wedge this call caused or inherited doesn't outlive this one call. */
bool tiles_i2c_write(i2c_inst_t *bus, uint8_t addr, const uint8_t *src, size_t len, bool nostop);

/* addr/dst/len/nostop match i2c_read_timeout_us()'s own signature.
 * Returns true iff every byte was read; on ANY failure (including this
 * function's own outer FIFO-room wait -- see this file's header comment,
 * point 1), recovers the bus before returning false. */
bool tiles_i2c_read(i2c_inst_t *bus, uint8_t addr, uint8_t *dst, size_t len, bool nostop);
