#include "pca9685.h"

#include "i2c_bus.h"

#include "pico/time.h"

/* Register addresses, PCA9685 datasheet Rev 4, tables 5-7. */
#define REG_MODE1 0x00u
#define REG_MODE2 0x01u
#define REG_LED0_ON_L 0x06u
#define REG_ALL_LED_ON_L 0xFAu /* ALL_LED_ON_L=0xFA, ON_H=0xFB, OFF_L=0xFC, OFF_H=0xFD */

#define MODE2_OUTDRV_BIT 0x04u /* bit 2 */
#define LED_H_FULL_BIT 0x10u   /* bit 4, both LEDn_ON_H (full ON) and LEDn_OFF_H (full OFF) */

#define NUM_CHANNELS 16u

/* Every write goes through drivers/i2c_bus (bounded, with bus recovery).
 * Unbounded writes once let a wedged bus hang the main loop with a haptic
 * motor stuck on. */
static bool write_reg(i2c_inst_t *bus, uint8_t addr, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return tiles_i2c_write(bus, addr, buf, 2, false);
}

static uint8_t channel_on_l_reg(uint8_t channel) {
    return (uint8_t)(REG_LED0_ON_L + 4u * channel);
}

bool tiles_pca9685_init(tiles_pca9685_t *dev, i2c_inst_t *bus, uint8_t addr) {
    dev->bus = bus;
    dev->addr = addr;

    /* Wake: clear SLEEP (bit 4). The rest of MODE1 goes to 0, including
     * ALLCALL (each chip is addressed individually). */
    if (!write_reg(bus, addr, REG_MODE1, 0x00u)) {
        return false;
    }

    /* Datasheet: the oscillator needs up to 500 us after SLEEP clears before
     * PWM register access is reliable. */
    sleep_us(500);

    if (!write_reg(bus, addr, REG_MODE2, MODE2_OUTDRV_BIT)) {
        return false;
    }

    /* Every channel full off (pin low) in one ALL_LED write. That is the POR
     * default, written explicitly. Not "all dark" on this board; see header. */
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

    /* LEDn_OFF_H[4] wins if both full bits are set, so write both explicitly
     * and never leave a stale bit. */
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
