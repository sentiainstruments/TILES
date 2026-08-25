/*
 * SENTIA TILES firmware entry point.
 *
 * Current scope: board bring-up, I2C discovery, LEDs (pad + underglow,
 * white, idle baseline + touch-driven brightness), function buttons
 * (debounced, LED lit while held), capacitive touch, USB MIDI (single
 * channel; note-on velocity from Hall strike acceleration and ongoing
 * aftertouch from press depth -- see services/expression.h; chromatic
 * pad->note layout with a scale-mode architecture ready for more
 * scales later -- see services/note_map.h), pedal (sustain CC64 on by
 * default, expression CC11 built but off by default -- see
 * services/pedal.h). Not yet built: MPE, haptics/motors, DIN, CV/gate,
 * the usb_vendor diagnostics interface, per-pad Hall calibration --
 * added module by module per the bring-up order in
 * docs/hardware/SENTIA_FIRMWARE_CODEX_START.md. Each phase must leave
 * this file building and the previous phase's safety guarantees intact.
 */

#include <stdio.h>

#include "pico/stdlib.h"

#include "board/board_init.h"
#include "diagnostics/i2c_scan.h"
#include "midi/usb_device.h"
#include "services/buttons.h"
#include "services/expression.h"
#include "services/hall.h"
#include "services/lighting.h"
#include "services/pedal.h"
#include "services/power.h"
#include "services/standby.h"
#include "services/touch.h"

int main(void) {
    /* Must run before stdio_init_all(): with tinyusb_device linked
     * explicitly (see CMakeLists.txt), pico_stdio_usb expects us to have
     * already called tusb_init() with our own composite CDC+MIDI
     * descriptors -- see midi/usb_device.h. */
    tiles_usb_device_init();

    stdio_init_all();

    board_init();

    /* Phase 2 bring-up: confirm every expected I2C device ACKs before
     * bringing up anything that talks to one. */
    tiles_diag_i2c_scan_expected_devices();

    /* Boot order step 13: raise both I2C buses to the 400kHz operating
     * speed now that enumeration has run, before any driver init below
     * starts talking to a device. Everything up to this point
     * (board_init's own bus setup and the scan above) ran at the
     * conservative 100kHz detection speed. */
    board_i2c_set_run_speed();

    /* Power source state: reads GP22 + TinyUSB's mounted flag and
     * derives the actual mode (USB-only / external-only / both / fault)
     * per the truth table in docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md.
     * Must run before tiles_lighting_init() -- lighting's brightness
     * ceiling reads this state on its very first pad sweep. GP22 is
     * already configured as an input by board_gpio_init() above, and
     * tud_mounted() is valid as soon as tusb_init() has run (it has, at
     * the very top of main via tiles_usb_device_init()) even though USB
     * likely hasn't enumerated yet at this point in boot. */
    tiles_power_init();

    /* Lighting only needs the LED mux controller (TCA9554, on I2C1) --
     * bring it up regardless of Hall/touch controller presence, so a
     * partially-populated bring-up board still shows pad/underglow
     * state instead of everything staying dark. */
    if (!tiles_lighting_init()) {
        printf("[lighting] init failed -- check I2C1/TCA9554 and PIO availability\n");
    }

    /* Both PCA9685 devices, all channels off + MODE2.OUTDRV=1, per the
     * safe boot order's non-negotiable step 6 -- run before touch/Hall
     * so the button-LED "off means lit, not dark" correction (see
     * services/buttons.c) happens as early as possible after boot. */
    if (!tiles_buttons_init()) {
        printf("[buttons] one or both PCA9685 devices failed init\n");
    }

    /* Both PCA9685 chips' outputs stay hardware-disabled (OE high, see
     * board_pins.h) until this runs -- must come after tiles_buttons_init()
     * so every channel is already in its intended state (motors off,
     * button LEDs dark) before the physical outputs go live. */
    board_pca9685_enable_outputs();

    /* Phase 3/5 bring-up: capacitive touch. */
    if (!tiles_touch_init()) {
        printf("[touch] one or both MPR121 controllers failed init\n");
    }

    /* Pedal: sustain (CC64) on by default; expression (CC11) built but
     * disabled by default -- see services/pedal.h. */
    tiles_pedal_init();

    /* Phase 4 bring-up: one Hall sensor at a time, then the full 24-pad
     * scan (see SENTIA_FIRMWARE_CODEX_START.md). A false return means
     * at least one pad's sensor failed identify/init -- tiles_hall_scan()
     * still runs for whichever pads did succeed rather than refusing to
     * start, per "a failed subsystem disables itself, it doesn't block
     * the rest." */
    if (!tiles_hall_init()) {
        printf("[hall] one or more pads failed sensor init -- see per-pad status\n");
    }

    /* Touch + Hall fusion: strike velocity + aftertouch. See
     * services/expression.h. */
    tiles_expression_init();

    /* Idle animations: needs lighting/buttons already initialized (its
     * render path drives both) and touch/pedal already initialized (its
     * activity check polls both). See services/standby.h. */
    tiles_standby_init();

    uint32_t last_scan_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        /* Runs first: lighting's ceiling_level() and any future
         * haptics/CV consumer read tiles_power_get_state() during this
         * same iteration, so the debounced state should already be
         * current by the time anything else runs. */
        tiles_power_scan();
        tiles_buttons_scan();
        tiles_touch_scan();
        tiles_pedal_scan();
        /* Must run after the three scans above so this iteration's
         * activity check sees fresh state -- see services/standby.h. */
        tiles_standby_scan();
        tiles_lighting_service();
        tiles_hall_scan();
        /* Must run after both tiles_touch_scan() and tiles_hall_scan()
         * above so it sees this iteration's fresh data from both. */
        tiles_expression_scan();

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_scan_ms >= 2000) {
            tiles_diag_i2c_scan_expected_devices();

            /* Temporary bring-up visibility for the derived power
             * state, same reasoning as the Hall print below -- replace
             * with a real usb_vendor/ diagnostics stream once that
             * exists. Names match tiles_power_mode_t's declaration
             * order. */
            static const char *const power_mode_names[] = {
                "USB_ONLY", "EXTERNAL_ONLY", "USB_AND_EXTERNAL", "FAULT",
            };
            tiles_power_state_t pwr = tiles_power_get_state();
            printf("[power] mode=%s led_ceiling=%u%% max_haptic_voices=%u cv_gate=%d\n",
                   power_mode_names[pwr.mode], pwr.led_brightness_ceiling_percent,
                   pwr.max_haptic_voices, pwr.cv_gate_permitted);

            /* Temporary bring-up visibility for pad 1's raw Hall sample,
             * over the same USB-CDC stdio as the I2C scan above. Replace
             * with a real per-pad diagnostics stream once usb_vendor/
             * exists -- this isn't meant to become the permanent way to
             * inspect Hall data. */
            tiles_hall_sample_t s = tiles_hall_get_sample(1);
            printf("[hall] pad 1: x=%d y=%d z=%d valid=%d\n", s.x, s.y, s.z, s.valid);

            printf("[standby] active=%d\n", tiles_standby_is_active());

            last_scan_ms = now_ms;
        }

        /* No sleep here (was sleep_ms(10), then sleep_ms(1)): removed
         * entirely for latency -- it bought nothing. pico_stdio_usb's
         * tud_task() runs from its own background IRQ regardless of
         * what this loop does (see midi/usb_device.c's header comment),
         * there's no watchdog yet to starve, and
         * services/expression.c's strike-detection window needs as
         * many loop iterations as possible landing inside it. The
         * resulting loop period is still unmeasured (depends on real
         * I2C transaction timing, and grows when multiple pads are held
         * via the Hall priority-scan pass) -- this is a direction, not
         * a measured number. */
    }

    return 0;
}
