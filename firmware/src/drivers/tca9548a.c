#include "tca9548a.h"

#include "i2c_bus.h"

/* Gates every Hall read (3 muxes cover all 24 pads); all writes go through
 * drivers/i2c_bus. */

void tiles_tca9548a_init(tiles_tca9548a_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;
}

bool tiles_tca9548a_disable_all(tiles_tca9548a_t *dev) {
    uint8_t value = 0x00u;
    return tiles_i2c_write(dev->bus, dev->addr, &value, 1, false);
}

bool tiles_tca9548a_select_channel(tiles_tca9548a_t *dev, uint8_t channel) {
    if (channel > 7u) {
        return false;
    }
    uint8_t value = (uint8_t)(1u << channel);
    return tiles_i2c_write(dev->bus, dev->addr, &value, 1, false);
}
