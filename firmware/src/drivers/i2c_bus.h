#pragma once

#include "hardware/i2c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded I2C transactions shared by every chip driver. Beyond one shared
 * timeout, it closes two pico-sdk 2.3.0 gaps:
 *
 * 1. i2c_read_timeout_us() first waits for TX FIFO room with no timeout
 *    (hardware/i2c.h:430). On a wedged bus that hangs forever.
 *    tiles_i2c_read() does that wait itself, against its own deadline.
 * 2. On a timeout, neither SDK call clears whatever wedged the bus. Both
 *    wrappers call board_i2c_recover_bus() on any failure, so the next
 *    transaction on that bus starts clean instead of waiting for the
 *    watchdog. */

/* Same arguments as i2c_write_timeout_us(). True iff every byte was
 * acknowledged; recovers the bus before returning false. */
bool tiles_i2c_write(i2c_inst_t *bus, uint8_t addr, const uint8_t *src, size_t len, bool nostop);

/* Same arguments as i2c_read_timeout_us(). True iff every byte was read;
 * recovers the bus before returning false. */
bool tiles_i2c_read(i2c_inst_t *bus, uint8_t addr, uint8_t *dst, size_t len, bool nostop);
