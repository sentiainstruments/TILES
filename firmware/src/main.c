/*
 * SENTIA TILES firmware entry point.
 *
 * Current scope: board bring-up, I2C discovery, LEDs (pad + underglow;
 * idle pads colored by note role -- root blue, naturals white, sharps
 * dark -- brightening to plain white on touch; see services/lighting.h),
 * function buttons
 * (debounced, LED lit while held), capacitive touch, USB MIDI (single
 * channel; note-on velocity from Hall strike acceleration and ongoing
 * aftertouch from press depth -- see services/expression.h; chromatic
 * pad->note layout with a scale-mode architecture ready for more
 * scales later -- see services/note_map.h), pedal (sustain CC64 on by
 * default, expression CC11 built but off by default -- see
 * services/pedal.h), per-pad haptic feedback (velocity-mapped kick on
 * strike, aftertouch-mapped sustain while held -- see
 * services/haptics.h), standby idle animations plus a deeper
 * power-saving state after 15 minutes of total inactivity (see
 * services/standby.h), a power-on animation that doubles as a Hall
 * baseline re-capture window (see services/boot_sequence.h), SW1/SW2's
 * default octave-shift function (see services/octave_control.h),
 * SW5/square ("sentia")'s pitch-bend toggle + haptic-intensity shift,
 * and its combo with SW6/circle for a 4-row pad-slider expression
 * sub-menu (haptics/pitch-bend/aftertouch sensitivity, one reserved
 * row) plus a 3-second-hold expression mute (see
 * services/expression_control.h), player-controlled minigames toggled
 * by holding SW3-SW6 (real snake + brick breaker, distinct from
 * standby's autonomous versions of the same -- see services/game_mode.h),
 * a serial-driven Hall calibration capture tool (rest/full-press/
 * max-press snapshots -- see diagnostics/calibration.h), operation modes
 * (melodic/chord/sequencer/arpeggiator) toggled by a single SW4/diamond
 * click -- sequencer mode fully built (24 pads = 24 steps, played back
 * from a real external MIDI clock, see services/midi_clock.h and
 * services/op_mode.h), chord/arp modes selectable but not yet
 * implemented. Not yet built:
 * MPE, DIN, CV/gate, the usb_vendor diagnostics interface, a real
 * per-pad Hall calibration curve (this is capture only, no curve is
 * derived or applied yet) -- added module by module per the bring-up
 * order in docs/hardware/SENTIA_FIRMWARE_CODEX_START.md. Each phase must
 * leave this file building and the previous phase's safety guarantees
 * intact.
 */

#include <stdio.h>

#include "pico/stdlib.h"

#include "tusb.h"

#include "board/board_init.h"
#include "diagnostics/calibration.h"
#include "diagnostics/i2c_scan.h"
#include "midi/midi_out.h"
#include "midi/usb_device.h"
#include "services/boot_sequence.h"
#include "services/buttons.h"
#include "services/expression.h"
#include "services/expression_control.h"
#include "services/game_mode.h"
#include "services/hall.h"
#include "services/haptics.h"
#include "services/lighting.h"
#include "services/midi_clock.h"
#include "services/octave_control.h"
#include "services/op_mode.h"
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

    /* SW1 ("-")/SW2 ("+")'s default function: octave shift, applied via
     * services/note_map.c. Claims both buttons' LEDs via buttons.h's
     * per-button override -- needs tiles_buttons_init() (above) already
     * run. See services/octave_control.h. */
    tiles_octave_control_init();

    /* SW5 (square, "sentia")'s function-button role: pitch-bend toggle,
     * a simple haptic-intensity shift, and (combined with SW6/circle)
     * the expression sub-menu + mute. Claims square's LED via the same
     * per-button override mechanism octave_control.c uses for SW1/SW2 --
     * needs tiles_buttons_init() (above) already run. See
     * services/expression_control.h. */
    tiles_expression_control_init();

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

    /* Power-on animation (~4s, blocking) -- also re-captures the Hall
     * rest baseline right as it ends, now that a few settled seconds
     * have passed since tiles_hall_init()'s very-first-instant capture
     * above. See services/boot_sequence.h. */
    if (!tiles_boot_sequence_run()) {
        printf("[boot_sequence] post-animation Hall baseline re-capture failed for at least one pad\n");
    }

    /* Serial-driven Hall calibration capture -- needs tiles_hall_init()
     * above already run. See diagnostics/calibration.h. */
    tiles_calibration_init();

    /* Haptics: needs tiles_buttons_init() (above) already run, since it
     * shares both PCA9685 chip instances with buttons rather than
     * re-initializing them -- see services/haptics.h. Doesn't write any
     * PCA9685 register itself (every channel is already "full off" from
     * buttons' own init), so exact ordering relative to
     * board_pca9685_enable_outputs() above doesn't matter electrically;
     * placed here because services/expression.c is what actually drives
     * it. */
    tiles_haptics_init();

    /* Touch + Hall fusion: strike velocity + aftertouch, and (via
     * haptics above) a velocity-mapped kick + aftertouch-mapped sustain.
     * See services/expression.h. */
    tiles_expression_init();

    /* Idle animations: needs lighting/buttons already initialized (its
     * render path drives both) and touch/pedal already initialized (its
     * activity check polls both). See services/standby.h. */
    tiles_standby_init();

    /* Player-controlled minigames (hold SW3+SW4+SW5+SW6 to toggle) --
     * needs lighting/buttons/touch already initialized, same as standby
     * above, whose rendering path it shares. See services/game_mode.h. */
    tiles_game_mode_init();

    /* MIDI clock RX (USB MIDI IN) -- the timing source op_mode.h's
     * sequencer mode runs from. See services/midi_clock.h. */
    tiles_midi_clock_init();

    /* Operation modes (melodic/chord/sequencer/arp), SW4/diamond single
     * click -- needs lighting/buttons/touch already initialized, same as
     * standby/game_mode above, whose rendering path it shares. See
     * services/op_mode.h. */
    tiles_op_mode_init();

    uint32_t last_scan_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        /* MUST run every iteration: this is what actually services the
         * USB stack (processes control transfers, moves CDC/MIDI data
         * to and from the hardware FIFOs) -- pico_stdio_usb's automatic
         * background-IRQ tud_task() servicing (which the loop-end
         * comment below used to claim happens "regardless of what this
         * loop does") is compiled out by pico-sdk whenever the
         * application links tinyusb_device directly and provides its
         * own descriptors, exactly what midi/usb_device.h describes --
         * see pico-sdk's pico/stdio_usb.h:
         * PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK defaults to 0
         * whenever LIB_TINYUSB_DEVICE is set, specifically so the
         * application calls tud_task() itself instead. Nothing here
         * ever did, which is a real, confirmed bug: printf() over
         * USB-CDC (the debug/calibration console) produced zero bytes
         * on real hardware, and the same missing pump plausibly
         * explains why USB MIDI has never been verified end-to-end in
         * a DAW either (see firmware/README.md's known gaps) -- without
         * this, DTR/line-state never gets processed so
         * stdio_usb_connected() never returns true, and queued MIDI
         * bytes never actually reach the host even if tud_midi_mounted()
         * happens to read true. */
        tud_task();

        /* Sends the MPE zone configuration (see midi/midi_out.h) the
         * FIRST iteration tud_midi_mounted() reads true, not once at
         * cold boot -- calling this before the host has actually
         * enumerated the device would be silently dropped by every send
         * in midi_out.c's own tud_midi_mounted() gate, since USB
         * enumeration only completes after several tud_task() calls
         * above, not synchronously at startup. Re-fires if the device
         * remounts (e.g. unplug/replug, or a host restart) since a fresh
         * enumeration means the previous zone config may not have
         * reached whatever's on the other end this time. */
        static bool s_mpe_was_mounted = false;
        bool mpe_mounted_now = tud_midi_mounted();
        if (mpe_mounted_now && !s_mpe_was_mounted) {
            tiles_midi_mpe_init();
        }
        s_mpe_was_mounted = mpe_mounted_now;

        /* Drains USB MIDI IN for Start/Continue/Stop/Clock bytes --
         * needs tud_task() above already run this iteration so the RX
         * FIFO is current, and must run before tiles_op_mode_scan()
         * below so this tick's fresh clock state is what sequencer mode
         * sees. See services/midi_clock.h. */
        tiles_midi_clock_scan();

        /* Runs first: lighting's ceiling_level() and any future
         * haptics/CV consumer read tiles_power_get_state() during this
         * same iteration, so the debounced state should already be
         * current by the time anything else runs. */
        tiles_power_scan();
        tiles_buttons_scan();
        /* Must run after tiles_buttons_scan() so this iteration's
         * debounced SW1/SW2 state is fresh. See
         * services/octave_control.h. */
        tiles_octave_control_scan();
        tiles_touch_scan();
        tiles_pedal_scan();
        /* Must run after tiles_buttons_scan() (fresh circle/square
         * state) and tiles_touch_scan() (fresh touch state for the
         * expression sub-menu's slider taps) above, and before
         * tiles_expression_scan() below so this tick's fresh "does the
         * sub-menu own the grid" state gates new-strike suppression
         * correctly. See services/expression_control.h. */
        tiles_expression_control_scan();
        /* Must run after tiles_buttons_scan()/tiles_touch_scan() above
         * so this iteration's entry-gesture/in-game-control/menu-
         * selection input is fresh. See services/game_mode.h. */
        tiles_game_mode_scan();
        /* Must run after tiles_buttons_scan()/tiles_touch_scan()
         * (diamond click + menu/step taps) and tiles_midi_clock_scan()
         * (fresh clock state for sequencer playback) above. See
         * services/op_mode.h. */
        tiles_op_mode_scan();
        /* Must run after the scans above so this iteration's activity
         * check sees fresh state -- see services/standby.h. Skipped
         * entirely while game mode, octave_control.c's transpose mode,
         * expression_control.c's sub-menu, or op_mode.c's menu/sequencer
         * owns the rendering path, so standby's own idle timer can't fire
         * mid-game/mid-transpose/mid-sub-menu/mid-sequencer and fight any
         * of them over the same pads/buttons/underglow -- see
         * services/game_mode.h's, services/octave_control.h's,
         * services/expression_control.h's, and services/op_mode.h's file
         * headers for the full reasoning. */
        if (!tiles_game_mode_is_active() && !tiles_octave_control_is_transpose_active() &&
            !tiles_expression_control_owns_pad_grid() && !tiles_op_mode_owns_pad_grid()) {
            tiles_standby_scan();
        }
        tiles_lighting_service();
        tiles_hall_scan();
        /* Non-blocking (zero-timeout stdio read) -- cheap even when
         * nothing has been typed. See diagnostics/calibration.h. */
        tiles_calibration_scan();
        /* Must run after both tiles_touch_scan() and tiles_hall_scan()
         * above so it sees this iteration's fresh data from both. */
        tiles_expression_scan();
        /* Advances KICK -> GAP -> SUSTAIN timing for any pad
         * expression_scan() just triggered/updated/stopped this
         * iteration. */
        tiles_haptics_scan();

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

            printf("[standby] active=%d deep_sleep=%d manual_scroll=%d\n", tiles_standby_is_active(),
                   tiles_standby_is_deep_sleep(), tiles_standby_owns_octave_buttons());

            last_scan_ms = now_ms;
        }

        /* No sleep here (was sleep_ms(10), then sleep_ms(1)): removed
         * entirely for latency -- it bought nothing. tud_task() is now
         * called explicitly at the top of this loop every iteration
         * (see that call's comment for why pico_stdio_usb's automatic
         * background-IRQ servicing does NOT cover this app, contrary to
         * what this comment used to claim), there's no watchdog yet to
         * starve, and
         * services/expression.c's strike-detection window needs as
         * many loop iterations as possible landing inside it. The
         * resulting loop period is still unmeasured (depends on real
         * I2C transaction timing, and grows when multiple pads are held
         * via the Hall priority-scan pass) -- this is a direction, not
         * a measured number. */
    }

    return 0;
}
