#pragma once

#include "hardware/i2c.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded I2C transactions for every driver, closing two pico-sdk 2.3.0
 * gaps: the read's unbounded wait for TX FIFO room (done here against our
 * deadline), and no recovery after a timeout (any failure recovers the bus,
 * so the next transaction starts clean). */

/* Same arguments as i2c_write_timeout_us(). True iff every byte was
 * acknowledged; recovers the bus before returning false. */
bool tiles_i2c_write(i2c_inst_t *bus, uint8_t addr, const uint8_t *src, size_t len, bool nostop);

/* Same arguments as i2c_read_timeout_us(). True iff every byte was read;
 * recovers the bus before returning false. */
bool tiles_i2c_read(i2c_inst_t *bus, uint8_t addr, uint8_t *dst, size_t len, bool nostop);
