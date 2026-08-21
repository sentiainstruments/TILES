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

#include <stdio.h>

#include "pico/stdlib.h"

#include "board/board_init.h"
#include "diagnostics/i2c_scan.h"
#include "services/hall.h"
#include "services/lighting.h"

int main(void) {
    stdio_init_all();

    board_init();

    /* Phase 2 bring-up: confirm every expected I2C device ACKs before
     * bringing up anything that talks to one. */
    tiles_diag_i2c_scan_expected_devices();

    /* Lighting only needs the LED mux controller (TCA9554, on I2C1) --
     * bring it up regardless of Hall/touch controller presence, so a
     * partially-populated bring-up board still shows pad/underglow
     * state instead of everything staying dark. */
    if (!tiles_lighting_init()) {
        printf("[lighting] init failed -- check I2C1/TCA9554 and PIO availability\n");
    }

    /* Phase 4 bring-up: one Hall sensor at a time, then the full 24-pad
     * scan (see SENTIA_FIRMWARE_CODEX_START.md). A false return means
     * at least one pad's sensor failed identify/init -- tiles_hall_scan()
     * still runs for whichever pads did succeed rather than refusing to
     * start, per "a failed subsystem disables itself, it doesn't block
     * the rest." */
    if (!tiles_hall_init()) {
        printf("[hall] one or more pads failed sensor init -- see per-pad status\n");
    }

    uint32_t last_scan_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        tiles_lighting_service();
        tiles_hall_scan();

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_scan_ms >= 2000) {
            tiles_diag_i2c_scan_expected_devices();

            /* Temporary bring-up visibility for pad 1's raw Hall sample,
             * over the same USB-CDC stdio as the I2C scan above. Replace
             * with a real per-pad diagnostics stream once usb_vendor/
             * exists -- this isn't meant to become the permanent way to
             * inspect Hall data. */
            tiles_hall_sample_t s = tiles_hall_get_sample(1);
            printf("[hall] pad 1: x=%d y=%d z=%d valid=%d\n", s.x, s.y, s.z, s.valid);

            last_scan_ms = now_ms;
        }

        sleep_ms(10);
    }

    return 0;
}
