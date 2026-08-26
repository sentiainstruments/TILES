#include "mpr121.h"

#include <stddef.h>

/* Register addresses, MPR121 datasheet Rev 4 Sections 5.2-5.13. */
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

/* Touch threshold for electrode N is 0x41+2N, release threshold is
 * 0x42+2N (Section 5.6). */
#define REG_ELE0_TOUCH_THR 0x41u

#define REG_DEBOUNCE 0x5Bu
#define REG_FILTER_GLOBAL_CDC 0x5Cu
#define REG_FILTER_GLOBAL_CDT 0x5Du
#define REG_ECR 0x5Eu
#define REG_SOFT_RESET 0x80u

#define NUM_ELECTRODES 12u

#define TOUCH_THRESHOLD 12u
/* Narrowed from Freescale's quickstart 6 (a 2:1 touch:release gap) to 9
 * (a tighter 4:3 gap) -- real feedback with the keycap/pad assembly now
 * seated: "release is sticking, lifting and losing contact is not
 * muting the note fast... should release as fast as a keyboard piano."
 * services/expression.c sends MIDI note-off the very same scan tick
 * tiles_touch_is_touched() goes false, with no debounce of its own (see
 * that file), so a sluggish-feeling release traces back to the raw
 * touch/release status itself, not anything downstream -- the electrode
 * signal has to swing all the way back down to within RELEASE_THRESHOLD
 * of baseline before the status bit clears, and requiring it to close
 * half the original touch swing (6 of 12) left more room for a lingering
 * near-threshold signal (residual capacitive coupling through the
 * keycap as a finger lifts) to still read as "touched" than a real piano
 * key's release feels like. Still a real hysteresis band (not equal to
 * the touch threshold), just a smaller one -- unmeasured against actual
 * chatter risk on the real keycap material, revisit if release starts
 * feeling twitchy instead of sticky. */
#define RELEASE_THRESHOLD 9u

static bool write_reg(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_blocking(bus, addr, buf, 2, false) == 2;
}

static bool read_regs(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    if (i2c_write_blocking(bus, addr, &reg, 1, true) != 1) {
        return false;
    }
    return i2c_read_blocking(bus, addr, buf, len, false) == (int)len;
}

bool tiles_mpr121_init(tiles_mpr121_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;

    /* Soft reset (Section 5.13: write 0x63 to register 0x80). Register
     * writes besides this and the ECR/GPIO registers only take effect
     * in Stop Mode, which soft reset returns the chip to. */
    if (!write_reg(bus, addr, REG_SOFT_RESET, 0x63u)) {
        return false;
    }

    /* Stop Mode explicitly (ECR=0x00), in case reset behavior ever
     * changes -- matches the pattern used elsewhere in this codebase of
     * not depending on implicit post-reset state. */
    if (!write_reg(bus, addr, REG_ECR, 0x00u)) {
        return false;
    }

    /* Baseline filtering -- Freescale's published quickstart defaults
     * (rising/falling/touched scenarios, Section 5.5). */
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

    /* Touch/release thresholds, same starting value for every electrode
     * -- real per-electrode tuning happens during calibration once the
     * enclosure/keycaps are assembled, not here. */
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

    /* No touch/release debounce yet (raw threshold hysteresis only) --
     * revisit during calibration. */
    if (!write_reg(bus, addr, REG_DEBOUNCE, 0x00u)) {
        return false;
    }

    /* Global charge current/time + filter iteration/sample-interval
     * settings (Section 5.8). CDC (0x5C) left at the chip's post-reset
     * default (0x10: FFI=00/6 samples, CDC=16uA) -- FFI=00 is already
     * the fastest option, nothing to gain there. CDT (0x5D) deviates
     * from the chip's default (0x24, ESI=100b/16ms) to ESI=000b/1ms --
     * the touch chip's own internal sample interval is a real latency
     * floor no amount of firmware polling can beat, and 16ms alone was
     * a meaningful chunk of end-to-end touch latency. 1ms is safe given
     * our FFI/CDT settings: actual per-cycle scan time is ~6 samples x
     * 1us x 12 electrodes = ~72us, comfortably under a 1ms period, so
     * this genuinely changes the sample rate rather than being silently
     * overridden by scan time (see the datasheet's own worked example
     * of that failure mode, Section 5.8). Tradeoff: less noise
     * averaging than Freescale's quickstart default -- revisit if touch
     * gets jittery once the real keycap/enclosure assembly exists. */
    if (!write_reg(bus, addr, REG_FILTER_GLOBAL_CDC, 0x10u)) {
        return false;
    }
    if (!write_reg(bus, addr, REG_FILTER_GLOBAL_CDT, 0x20u)) {
        return false;
    }

    /* Run Mode (Section 5.11): CL=10b (baseline tracking enabled, seed
     * the initial baseline from the first measured value's 5 high bits
     * -- shortens the early no-response window right after enabling Run
     * Mode), ELEPROX_EN=00 (no proximity channel), ELE_EN=1111b (all 12
     * electrodes enabled) -> 0x8F. */
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
    /* buf[0] = ELE0-7; buf[1] bits 0-3 = ELE8-11 (bits 4-7 are
     * EPROX/reserved/OVCF, not electrode touch data). */
    return (uint16_t)(buf[0] | ((buf[1] & 0x0Fu) << 8));
}
