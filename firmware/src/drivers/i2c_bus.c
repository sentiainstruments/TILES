#include "i2c_bus.h"

#include "../board/board_init.h"

#include "pico/time.h"

/* Same 5ms budget every driver in this directory used to define locally
 * (drivers/pca9685.c's comment has the full "haptic motor locked on
 * after a freeze" history) -- generous headroom over the longest real
 * transaction on this board (a handful of bytes at 400kHz), while still
 * turning a wedged transaction into a bounded failure instead of a hang. */
#define TILES_I2C_TIMEOUT_US 5000u

bool tiles_i2c_write(i2c_inst_t *bus, uint8_t addr, const uint8_t *src, size_t len, bool nostop) {
    bool ok = i2c_write_timeout_us(bus, addr, src, len, nostop, TILES_I2C_TIMEOUT_US) == (int)len;
    if (!ok) {
        board_i2c_recover_bus(bus);
    }
    return ok;
}

bool tiles_i2c_read(i2c_inst_t *bus, uint8_t addr, uint8_t *dst, size_t len, bool nostop) {
    /* Closes the gap this file's header comment (point 1) documents:
     * i2c_read_timeout_us()'s own internal wait for TX FIFO room ignores
     * its timeout argument entirely. Bounding it here, ourselves, first,
     * means a wedged bus is caught by OUR deadline instead of hanging
     * inside the SDK call below. */
    absolute_time_t fifo_deadline = make_timeout_time_us(TILES_I2C_TIMEOUT_US);
    while (i2c_get_write_available(bus) == 0) {
        if (time_reached(fifo_deadline)) {
            board_i2c_recover_bus(bus);
            return false;
        }
        tight_loop_contents();
    }

    bool ok = i2c_read_timeout_us(bus, addr, dst, len, nostop, TILES_I2C_TIMEOUT_US) == (int)len;
    if (!ok) {
        board_i2c_recover_bus(bus);
    }
    return ok;
}
