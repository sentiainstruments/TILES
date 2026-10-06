#pragma once

/* TCA9548A 8-channel I2C mux. One control register: bit N connects
 * channel N upstream; 0x00 disconnects all.
 *
 * The "one Hall channel open across all three muxes" rule is enforced by
 * services/hall, which owns all three instances. */

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

/* Connects exactly `channel` (0-7), disconnecting any other on this chip. */
bool tiles_tca9548a_select_channel(tiles_tca9548a_t *dev, uint8_t channel);
