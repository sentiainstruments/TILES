#include "mpr121.h"

#include "i2c_bus.h"

#include <stddef.h>

/* Register addresses, MPR121 datasheet Rev 4, sections 5.2-5.13. */
#define REG_ELE0_7_TOUCH_STATUS 0x00u
#define REG_ELE8_PROX_TOUCH_STATUS 0x01u

#define REG_MHDR 0x2Bu
#define REG_NHDR 0x2Cu
#define REG_NCLR 0x2Du
#define REG_FDLR 0x2Eu
#define REG_MHDF 0x2Fu
#define REG_NHDF 0x30u
#define REG_NCLF 0x31u
#define REG_FDLF 0x32u
#define REG_NHDT 0x33u
#define REG_NCLT 0x34u
#define REG_FDLT 0x35u

/* Touch threshold for electrode N is 0x41+2N, release 0x42+2N (5.6). */
#define REG_ELE0_TOUCH_THR 0x41u

#define REG_DEBOUNCE 0x5Bu
#define REG_FILTER_GLOBAL_CDC 0x5Cu
#define REG_FILTER_GLOBAL_CDT 0x5Du
#define REG_ECR 0x5Eu
#define REG_SOFT_RESET 0x80u

#define NUM_ELECTRODES 12u

#define TOUCH_THRESHOLD 12u
/* Release threshold 9 (quickstart value is 6): the smaller hysteresis gap
 * lets a lifting finger clear touch sooner, so notes stop as fast as a
 * piano key. expression.c sends Note-Off on the same scan touch clears, so
 * release feel is set here. Not yet measured for chatter on the final
 * keycap material; raise it if release gets twitchy. */
#define RELEASE_THRESHOLD 9u

/* Touch is read every main-loop pass, so this is one of the busiest I2C
 * users; all access goes through drivers/i2c_bus. */

static bool write_reg(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return tiles_i2c_write(bus, addr, buf, 2, false);
}

static bool read_regs(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    if (!tiles_i2c_write(bus, addr, &reg, 1, true)) {
        return false;
    }
    return tiles_i2c_read(bus, addr, buf, len, false);
}

bool tiles_mpr121_init(tiles_mpr121_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;

    /* Soft reset (5.13: write 0x63 to 0x80). Most registers only take writes in
     * Stop Mode, which reset returns to. */
    if (!write_reg(bus, addr, REG_SOFT_RESET, 0x63u)) {
        return false;
    }

    /* Stop Mode explicitly (ECR=0x00) rather than relying on post-reset state. */
    if (!write_reg(bus, addr, REG_ECR, 0x00u)) {
        return false;
    }

    /* Baseline filtering: Freescale's quickstart values (5.5). */
    static const struct {
        uint8_t reg;
        uint8_t value;
    } filter_regs[] = {
        {REG_MHDR, 0x01u}, {REG_NHDR, 0x01u}, {REG_NCLR, 0x00u}, {REG_FDLR, 0x00u},
        {REG_MHDF, 0x01u}, {REG_NHDF, 0x01u}, {REG_NCLF, 0xFFu}, {REG_FDLF, 0x02u},
        {REG_NHDT, 0x00u}, {REG_NCLT, 0x00u}, {REG_FDLT, 0x00u},
    };
    for (size_t i = 0; i < sizeof(filter_regs) / sizeof(filter_regs[0]); i++) {
        if (!write_reg(bus, addr, filter_regs[i].reg, filter_regs[i].value)) {
            return false;
        }
    }

    /* Same thresholds for every electrode; per-electrode tuning belongs to
     * calibration on the assembled unit. */
    for (uint8_t e = 0; e < NUM_ELECTRODES; e++) {
        uint8_t touch_reg = (uint8_t)(REG_ELE0_TOUCH_THR + 2u * e);
        uint8_t release_reg = (uint8_t)(touch_reg + 1u);
        if (!write_reg(bus, addr, touch_reg, TOUCH_THRESHOLD)) {
            return false;
        }
        if (!write_reg(bus, addr, release_reg, RELEASE_THRESHOLD)) {
            return false;
        }
    }

    /* No debounce (threshold hysteresis only). */
    if (!write_reg(bus, addr, REG_DEBOUNCE, 0x00u)) {
        return false;
    }

    /* Global charge/filter settings (5.8). CDC (0x5C) stays at the reset default
     * 0x10 (FFI 6 samples, 16 uA). CDT (0x5D) sets ESI to 1 ms instead of the
     * 16 ms default: the chip's sample interval is a latency floor polling
     * can't beat. A full scan is ~72 us (6 samples x 1 us x 12 electrodes), well
     * inside 1 ms. Tradeoff: less noise averaging. */
    if (!write_reg(bus, addr, REG_FILTER_GLOBAL_CDC, 0x10u)) {
        return false;
    }
    if (!write_reg(bus, addr, REG_FILTER_GLOBAL_CDT, 0x20u)) {
        return false;
    }

    /* Run Mode (5.11): CL=10b (baseline tracking, seeded from the first
     * reading), no proximity channel, all 12 electrodes -> 0x8F. */
    return write_reg(bus, addr, REG_ECR, 0x8Fu);
}

uint16_t tiles_mpr121_read_touched(tiles_mpr121_t *dev, bool *ok) {
    uint8_t buf[2];
    bool success = read_regs(dev->bus, dev->addr, REG_ELE0_7_TOUCH_STATUS, buf, 2);
    if (ok != NULL) {
        *ok = success;
    }
    if (!success) {
        return 0;
    }
    /* buf[0] = ELE0-7; buf[1] bits 0-3 = ELE8-11 (bits 4-7 are not touch data). */
    return (uint16_t)(buf[0] | ((buf[1] & 0x0Fu) << 8));
}
