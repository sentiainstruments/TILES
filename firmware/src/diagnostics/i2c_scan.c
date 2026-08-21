#include "i2c_scan.h"

#include <stdio.h>

#include "board_pins.h"
#include "hardware/i2c.h"

/* Standard pico-sdk bus-scan technique: a zero-length write only sends
 * the address byte and waits for ACK/NACK, never touching device state. */
static bool probe(i2c_inst_t *bus, uint8_t addr) {
    uint8_t dummy = 0;
    int ret = i2c_write_blocking(bus, addr, &dummy, 0, false);
    return ret >= 0;
}

static bool check_device(const char *label, i2c_inst_t *bus, uint8_t addr) {
    bool ok = probe(bus, addr);
    printf("[i2c-scan] %-30s addr=0x%02X bus=%s : %s\n", label, addr,
           bus == i2c0 ? "I2C0" : "I2C1", ok ? "ACK" : "no response");
    return ok;
}

bool tiles_diag_i2c_scan_expected_devices(void) {
    bool all_ok = true;

    all_ok = check_device("Hall mux 1 (TCA9548A)", i2c0, TILES_I2C0_ADDR_HALL_MUX1) && all_ok;
    all_ok = check_device("Hall mux 2 (TCA9548A)", i2c0, TILES_I2C0_ADDR_HALL_MUX2) && all_ok;
    all_ok = check_device("Hall mux 3 (TCA9548A)", i2c0, TILES_I2C0_ADDR_HALL_MUX3) && all_ok;
    all_ok = check_device("Touch controller 1 (MPR121)", i2c0, TILES_I2C0_ADDR_TOUCH1) && all_ok;
    all_ok = check_device("Touch controller 2 (MPR121)", i2c0, TILES_I2C0_ADDR_TOUCH2) && all_ok;

    all_ok = check_device("Haptic PWM 1 (PCA9685)", i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_1) && all_ok;
    all_ok = check_device("Haptic PWM 2 (PCA9685)", i2c1, TILES_I2C1_ADDR_HAPTIC_PCA9685_2) && all_ok;
    all_ok = check_device("LED mux controller (TCA9554)", i2c1, TILES_I2C1_ADDR_LED_MUX_TCA9554) && all_ok;

    /* Hall sensors are not individually probed here: all 24 share address
     * 0x35 behind their mux channel, so probing that address without
     * first selecting a mux channel would just find whichever channel
     * happens to be enabled (or none). Per-sensor presence is confirmed
     * by drivers/tca9548a + drivers/tmag5273 once those exist. */

    printf("[i2c-scan] %s\n", all_ok ? "all expected devices present" : "one or more expected devices missing");
    return all_ok;
}
