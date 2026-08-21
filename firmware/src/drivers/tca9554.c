#include "tca9554.h"

#define REG_OUTPUT_PORT 0x01u
#define REG_CONFIG 0x03u

static bool write_reg(tiles_tca9554_t *dev, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    int ret = i2c_write_blocking(dev->bus, dev->addr, buf, 2, false);
    return ret == 2;
}

static uint8_t all_muxes_disabled_bits(void) {
    return (uint8_t)((1u << TILES_TCA9554_PORT_MUX1_ENABLE_N) |
                      (1u << TILES_TCA9554_PORT_MUX2_ENABLE_N) |
                      (1u << TILES_TCA9554_PORT_MUX3_ENABLE_N));
}

bool tiles_tca9554_init(tiles_tca9554_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;
    dev->shadow_output = all_muxes_disabled_bits();

    if (!write_reg(dev, REG_OUTPUT_PORT, dev->shadow_output)) {
        return false;
    }

    /* Config register: 0 = output, 1 = input. P0-P5 output, P6/P7 input. */
    const uint8_t config = 0xC0u;
    return write_reg(dev, REG_CONFIG, config);
}

bool tiles_tca9554_disable_all_muxes(tiles_tca9554_t *dev) {
    dev->shadow_output |= all_muxes_disabled_bits();
    return write_reg(dev, REG_OUTPUT_PORT, dev->shadow_output);
}

bool tiles_tca9554_set_select(tiles_tca9554_t *dev, uint8_t channel) {
    if (channel > 7u) {
        return false;
    }
    dev->shadow_output = (uint8_t)((dev->shadow_output & ~0x07u) | (channel & 0x07u));
    return write_reg(dev, REG_OUTPUT_PORT, dev->shadow_output);
}

bool tiles_tca9554_enable_mux(tiles_tca9554_t *dev, uint8_t mux_index) {
    uint8_t enable_bit;
    switch (mux_index) {
        case 1: enable_bit = TILES_TCA9554_PORT_MUX1_ENABLE_N; break;
        case 2: enable_bit = TILES_TCA9554_PORT_MUX2_ENABLE_N; break;
        case 3: enable_bit = TILES_TCA9554_PORT_MUX3_ENABLE_N; break;
        default: return false;
    }

    uint8_t output = (uint8_t)((dev->shadow_output | all_muxes_disabled_bits()) & ~(1u << enable_bit));
    dev->shadow_output = output;
    return write_reg(dev, REG_OUTPUT_PORT, dev->shadow_output);
}
