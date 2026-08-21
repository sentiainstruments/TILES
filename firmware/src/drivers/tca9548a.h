#pragma once

/*
 * TCA9548A 8-channel I2C mux driver. One control register: writing a
 * byte with bit N set connects channel N to the upstream bus; writing
 * 0x00 disconnects every channel.
 *
 * Enforces nothing about the cross-chip "only one Hall mux channel
 * across all three TCA9548A devices at a time" invariant -- that's a
 * system-level policy owned by services/hall (which holds all three
 * instances), not something one chip's driver can know about on its own.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/i2c.h"

typedef struct {
    i2c_inst_t *bus;
    uint8_t addr;
} tiles_tca9548a_t;

void tiles_tca9548a_init(tiles_tca9548a_t *dev, i2c_inst_t *bus, uint8_t addr);

/* Disconnects every channel on this chip. */
bool tiles_tca9548a_disable_all(tiles_tca9548a_t *dev);

/* Connects exactly `channel` (0-7) on this chip, disconnecting any
 * other channel that was open on it. */
bool tiles_tca9548a_select_channel(tiles_tca9548a_t *dev, uint8_t channel);
