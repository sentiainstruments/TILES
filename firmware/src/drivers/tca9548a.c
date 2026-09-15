#include "tca9548a.h"

/* See drivers/pca9685.c's identical constant for the full "haptic motor
 * locked on after a freeze" rationale -- tiles_tca9548a_select_channel()
 * gates every one of 24 pads' Hall reads (3 of these muxes cover all 24
 * pads between them), so an unbounded i2c_write_blocking() here is one
 * of the most exposed paths to the same class of hang. */
#define TILES_I2C_TIMEOUT_US 5000u

void tiles_tca9548a_init(tiles_tca9548a_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;
}

bool tiles_tca9548a_disable_all(tiles_tca9548a_t *dev) {
    uint8_t value = 0x00u;
    return i2c_write_timeout_us(dev->bus, dev->addr, &value, 1, false, TILES_I2C_TIMEOUT_US) == 1;
}

bool tiles_tca9548a_select_channel(tiles_tca9548a_t *dev, uint8_t channel) {
    if (channel > 7u) {
        return false;
    }
    uint8_t value = (uint8_t)(1u << channel);
    return i2c_write_timeout_us(dev->bus, dev->addr, &value, 1, false, TILES_I2C_TIMEOUT_US) == 1;
}
