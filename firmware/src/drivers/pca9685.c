#include "pca9685.h"

#include "pico/time.h"

/* Register addresses, PCA9685 datasheet Rev 4 Table 5/6/7. */
#define REG_MODE1 0x00u
#define REG_MODE2 0x01u
#define REG_LED0_ON_L 0x06u
#define REG_ALL_LED_ON_L 0xFAu /* ALL_LED_ON_L=0xFA, ON_H=0xFB, OFF_L=0xFC, OFF_H=0xFD */

#define MODE2_OUTDRV_BIT 0x04u /* bit 2 */
#define LED_H_FULL_BIT 0x10u   /* bit 4, both LEDn_ON_H (full ON) and LEDn_OFF_H (full OFF) */

#define NUM_CHANNELS 16u

static bool write_reg(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_write_blocking(bus, addr, buf, 2, false) == 2;
}

static uint8_t channel_on_l_reg(uint8_t channel) {
    return (uint8_t)(REG_LED0_ON_L + 4u * channel);
}

bool tiles_pca9685_init(tiles_pca9685_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;

    /* Wake: clear SLEEP (bit4). Everything else in MODE1 goes to 0,
     * including ALLCALL -- this board addresses each chip individually,
     * no LED-All-Call group writes needed. */
    if (!write_reg(bus, addr, REG_MODE1, 0x00u)) {
        return false;
    }

    /* Datasheet: "It takes 500us max for the oscillator to be up and
     * running once SLEEP bit has been set to logic 0. Timings on LEDn
     * outputs are not guaranteed if PWM control registers are accessed
     * within the 500us window." */
    sleep_us(500);

    if (!write_reg(bus, addr, REG_MODE2, MODE2_OUTDRV_BIT)) {
        return false;
    }

    /* Force every channel to "full off" (pin low) via the ALL_LED
     * shortcut register in one write. Matches the chip's own POR
     * default, written explicitly so this doesn't depend on that
     * default surviving a soft reset. See the header for why this
     * alone is not "every output dark" on this board. */
    uint8_t all_led_off_h = (uint8_t)(REG_ALL_LED_ON_L + 3u);
    return write_reg(bus, addr, all_led_off_h, LED_H_FULL_BIT);
}

bool tiles_pca9685_set_channel_full(tiles_pca9685_t *dev, uint8_t channel, bool full_on) {
    if (channel >= NUM_CHANNELS) {
        return false;
    }

    uint8_t on_l = channel_on_l_reg(channel);
    uint8_t on_h = (uint8_t)(on_l + 1u);
    uint8_t off_l = (uint8_t)(on_l + 2u);
    uint8_t off_h = (uint8_t)(on_l + 3u);

    /* Datasheet: "If LEDn_ON_H[4] and LEDn_OFF_H[4] are set at the same
     * time, the LEDn_OFF_H[4] function takes precedence." Always write
     * both bits explicitly to this call's intent so no stale bit from a
     * previous call can linger. */
    if (!write_reg(dev->bus, dev->addr, on_l, 0x00u)) {
        return false;
    }
    if (!write_reg(dev->bus, dev->addr, on_h, full_on ? LED_H_FULL_BIT : 0x00u)) {
        return false;
    }
    if (!write_reg(dev->bus, dev->addr, off_l, 0x00u)) {
        return false;
    }
    if (!write_reg(dev->bus, dev->addr, off_h, full_on ? 0x00u : LED_H_FULL_BIT)) {
        return false;
    }

    return true;
}

bool tiles_pca9685_set_pwm(tiles_pca9685_t *dev, uint8_t channel, uint16_t on_count, uint16_t off_count) {
    if (channel >= NUM_CHANNELS || on_count > 4095u || off_count > 4095u) {
        return false;
    }

    uint8_t on_l = channel_on_l_reg(channel);
    uint8_t on_h = (uint8_t)(on_l + 1u);
    uint8_t off_l = (uint8_t)(on_l + 2u);
    uint8_t off_h = (uint8_t)(on_l + 3u);

    if (!write_reg(dev->bus, dev->addr, on_l, (uint8_t)(on_count & 0xFFu))) {
        return false;
    }
    if (!write_reg(dev->bus, dev->addr, on_h, (uint8_t)((on_count >> 8) & 0x0Fu))) {
        return false;
    }
    if (!write_reg(dev->bus, dev->addr, off_l, (uint8_t)(off_count & 0xFFu))) {
        return false;
    }
    if (!write_reg(dev->bus, dev->addr, off_h, (uint8_t)((off_count >> 8) & 0x0Fu))) {
        return false;
    }

    return true;
}
