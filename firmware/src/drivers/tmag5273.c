#include "tmag5273.h"

/* Register offsets, TMAG5273 datasheet Table 8-1. */
#define REG_DEVICE_CONFIG_1 0x00u
#define REG_DEVICE_CONFIG_2 0x01u
#define REG_SENSOR_CONFIG_1 0x02u
#define REG_SENSOR_CONFIG_2 0x03u
#define REG_MANUFACTURER_ID_LSB 0x0Eu
#define REG_MANUFACTURER_ID_MSB 0x0Fu
#define REG_X_MSB_RESULT 0x12u

/* Table 8-17/8-18 reset values -- spells "TI" (MSB=0x54='T', LSB=0x49='I'). */
#define EXPECTED_MANUFACTURER_ID_LSB 0x49u
#define EXPECTED_MANUFACTURER_ID_MSB 0x54u

/* DEVICE_CONFIG_1 (Table 8-3): CRC_EN=0, MAG_TEMPCO=0, CONV_AVG=0 (1x,
 * fastest), I2C_RD=0 (standard sequential register read). */
#define DEVICE_CONFIG_1_VALUE 0x00u

/* DEVICE_CONFIG_2 (Table 8-4): THR_HYST=0, LP_LN=0 (low active current
 * mode), I2C_GLITCH_FILTER=0 (on), TRIGGER_MODE=0, OPERATING_MODE=2h
 * (continuous measure) -> 0b0000_0010. */
#define DEVICE_CONFIG_2_VALUE 0x02u

/* SENSOR_CONFIG_1 (Table 8-5): MAG_CH_EN=7h (X,Y,Z enabled) in bits
 * 7-4, SLEEPTIME=0 (unused outside wake-up-and-sleep mode) -> 0111_0000. */
#define SENSOR_CONFIG_1_VALUE 0x70u

/* SENSOR_CONFIG_2 (Table 8-6): THRX_COUNT=0, MAG_THR_DIR=0,
 * MAG_GAIN_CH=0, ANGLE_EN=0 (no angle calc), X_Y_RANGE=1, Z_RANGE=1
 * (+/-80mT on every axis) -> 0000_0011. */
#define SENSOR_CONFIG_2_VALUE 0x03u

static bool write_reg(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_blocking(bus, addr, buf, 2, false) == 2;
}

/* Standard sequential register read (datasheet Figure 6-9): write the
 * starting register address with no STOP, then repeated-START into a
 * block read of `len` consecutive registers. */
static bool read_regs(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    if (i2c_write_blocking(bus, addr, &reg, 1, true) != 1) {
        return false;
    }
    return i2c_read_blocking(bus, addr, buf, len, false) == (int)len;
}

bool tiles_tmag5273_identify(i2c_inst_t *bus, uint8_t addr) {
    uint8_t id[2];
    if (!read_regs(bus, addr, REG_MANUFACTURER_ID_LSB, id, 2)) {
        return false;
    }
    return id[0] == EXPECTED_MANUFACTURER_ID_LSB && id[1] == EXPECTED_MANUFACTURER_ID_MSB;
}

bool tiles_tmag5273_init(tiles_tmag5273_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;

    if (!write_reg(bus, addr, REG_DEVICE_CONFIG_1, DEVICE_CONFIG_1_VALUE)) {
        return false;
    }
    if (!write_reg(bus, addr, REG_DEVICE_CONFIG_2, DEVICE_CONFIG_2_VALUE)) {
        return false;
    }
    if (!write_reg(bus, addr, REG_SENSOR_CONFIG_1, SENSOR_CONFIG_1_VALUE)) {
        return false;
    }
    if (!write_reg(bus, addr, REG_SENSOR_CONFIG_2, SENSOR_CONFIG_2_VALUE)) {
        return false;
    }

    return true;
}

bool tiles_tmag5273_read_xyz(const tiles_tmag5273_t *dev, tiles_tmag5273_sample_t *out) {
    uint8_t buf[6];
    if (!read_regs(dev->bus, dev->addr, REG_X_MSB_RESULT, buf, sizeof(buf))) {
        return false;
    }

    out->x = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    out->y = (int16_t)(((uint16_t)buf[2] << 8) | buf[3]);
    out->z = (int16_t)(((uint16_t)buf[4] << 8) | buf[5]);
    return true;
}
