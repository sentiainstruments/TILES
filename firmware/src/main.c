/*
 * SENTIA TILES firmware entry point.
 *
 * Current scope: board-level bring-up only (GPIO safe states + I2C bus
 * init). Everything else -- I2C device discovery, Hall/touch/MIDI/
 * lighting/haptics services, the diagnostics/usb_vendor interface -- is
 * added module by module per the bring-up order in
 * docs/hardware/SENTIA_FIRMWARE_CODEX_START.md. Each phase must leave
 * this file building and the previous phase's safety guarantees intact.
 */

#include "pico/stdlib.h"

#include "board/board_init.h"

int main(void) {
    stdio_init_all();

    board_init();

    /* Next phase: I2C device discovery (confirm every expected address
     * ACKs at TILES_I2C_DETECT_HZ) before calling board_i2c_set_run_speed()
     * and bringing up any driver. */

    while (true) {
        tight_loop_contents();
    }

    return 0;
}
