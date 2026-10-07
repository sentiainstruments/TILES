#include "i2c_bus.h"

#include "../board/board_init.h"

#include "pico/time.h"

/* 5 ms: ample for the longest transaction here (a few bytes at 400 kHz),
 * and turns a wedged bus into a bounded failure. */
#define TILES_I2C_TIMEOUT_US 5000u

bool tiles_i2c_write(i2c_inst_t *bus, uint8_t addr, const uint8_t *src, size_t len, bool nostop) {
    bool ok = i2c_write_timeout_us(bus, addr, src, len, nostop, TILES_I2C_TIMEOUT_US) == (int)len;
    if (!ok) {
        board_i2c_recover_bus(bus);
    }
    return ok;
}

bool tiles_i2c_read(i2c_inst_t *bus, uint8_t addr, uint8_t *dst, size_t len, bool nostop) {
    /* The SDK's own wait for TX FIFO room ignores its timeout (see the header),
     * so do it here first. */
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
