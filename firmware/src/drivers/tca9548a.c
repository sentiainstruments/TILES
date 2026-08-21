#include "tca9548a.h"

void tiles_tca9548a_init(tiles_tca9548a_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;
}

bool tiles_tca9548a_disable_all(tiles_tca9548a_t *dev) {
    uint8_t value = 0x00u;
    return i2c_write_blocking(dev->bus, dev->addr, &value, 1, false) == 1;
}

bool tiles_tca9548a_select_channel(tiles_tca9548a_t *dev, uint8_t channel) {
    if (channel > 7u) {
        return false;
    }
    uint8_t value = (uint8_t)(1u << channel);
    return i2c_write_blocking(dev->bus, dev->addr, &value, 1, false) == 1;
}
