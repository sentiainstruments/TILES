/* SENTIA TILES firmware entry point: brings every subsystem up in the safe
 * boot order (docs/hardware/SENTIA_FIRMWARE_CODEX_START.md), then runs the
 * cooperative main loop, one scan per module per pass.
 *
 * Boot rules: outputs stay off until configured; a subsystem that fails
 * init disables itself and never blocks the rest; a crash-recovery reboot
 * (watchdog) skips the slow, print-only steps so the instrument is back
 * fast. What each module does: firmware/README.md and each module's
 * README. */

#include <stdio.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "tusb.h"

#include "board/board_init.h"
#include "board/unit_id.h"
#include "diagnostics/calibration.h"
#include "diagnostics/i2c_scan.h"
#include "diagnostics/loop_stats.h"
#include "midi/din_midi.h"
#include "midi/identity.h"
#include "midi/midi_in.h"
#include "midi/midi_out.h"
#include "midi/usb_device.h"
#include "profiles/content.h"
#include "profiles/settings_table.h"
#include "services/boot_sequence.h"
#include "services/buttons.h"
#include "services/crash_indicator.h"
#include "services/cv_gate.h"
#include "services/debug_mode.h"
#include "services/expression.h"
#include "services/expression_control.h"
#include "services/game_mode.h"
#include "services/hall.h"
#include "services/haptics.h"
#include "services/lighting.h"
#include "services/midi_channels.h"
#include "services/midi_clock.h"
#include "services/note_map.h"
#include "services/octave_control.h"
#include "services/op_mode.h"
#include "services/pedal.h"
#include "services/power.h"
#include "services/standby.h"
#include "services/touch.h"
#include "storage/storage_flash.h"
#include "usb_vendor/usb_vendor.h"

/* Power-change callback (registered below): a power-source switch can
 * reset the two PCA9685s (button LEDs + haptic motors) without resetting
 * the MCU. Re-assert OE (cheap insurance), then reconfigure the chips,
 * then let buttons and haptics repaint their state. `new_state` is unused;
 * both resync functions read what they need live. */
static void tiles_power_recover_haptics_and_buttons(tiles_power_state_t new_state) {
    (void)new_state;
    board_pca9685_enable_outputs();
    tiles_buttons_resync_pca9685();
    tiles_haptics_resync_hardware();
    printf("[power] recovered haptics/buttons hardware after a power-mode change\n");
}

int main(void) {
    /* Read once, first thing (a scratch-register read, safe before any init),
     * and reused for every skippable boot step. Each boot printf can block up
     * to the stdio timeout with no terminal attached; a crash-recovery boot
     * skips them and the boot animation so it's back as fast as USB
     * re-enumerates. */
    bool crash_recovered = watchdog_enable_caused_reboot();

    /* Before ANYTHING else: every init below writes crash-recorder traces into
     * the same ring buffer this snapshots (see debug_mode.h). */
    tiles_debug_mode_capture_crash_snapshot(crash_recovered);

    /* Before stdio_init_all(): with tinyusb_device linked, pico_stdio_usb
     * expects tusb_init() already done with our descriptors
     * (midi/usb_device.h). */
    tiles_usb_device_init();

    stdio_init_all();

    if (!crash_recovered) {
        printf("[main] SENTIA TILES unit %u/%u\n", (unsigned)TILES_UNIT_NUMBER, (unsigned)TILES_UNIT_COUNT);
    }

    board_init();

    /* I2C discovery before anything talks to a device. Skipped on a crash
     * recovery (a warm reset didn't unplug anything). */
    if (!crash_recovered) {
        tiles_diag_i2c_scan_expected_devices();
    }

    /* Raise both buses to 400 kHz now that discovery ran at 100 kHz. */
    board_i2c_set_run_speed();

    /* Power mode (GP22 + USB mounted). Before lighting, whose brightness
     * ceiling reads it on its first sweep. tud_mounted() is valid once
     * tusb_init() ran, even before enumeration. */
    tiles_power_init();

    /* Lighting needs only the TCA9554 (I2C1), so a partly populated board
     * still lights up. */
    if (!tiles_lighting_init()) {
        printf("[lighting] init failed -- check I2C1/TCA9554 and PIO availability\n");
    }

    /* Both PCA9685s: channels off, MODE2.OUTDRV=1 (safe boot step 6). Early,
     * so the active-low button LEDs are turned dark as soon as possible. */
    if (!tiles_buttons_init()) {
        printf("[buttons] one or both PCA9685 devices failed init\n");
    }

    /* PCA9685 outputs stay disabled (OE) until every channel is in its
     * intended state (after buttons init). */
    board_pca9685_enable_outputs();

    /* Scale/octave/key (kept across a crash reboot). Before octave control,
     * its first user. */
    tiles_note_map_init(crash_recovered);

    /* "-"/"+": octave shift (claims their LEDs; needs buttons init). */
    tiles_octave_control_init();

    /* Square: pitch bend toggle and the expression menu (claims its LED;
     * needs buttons init). */
    tiles_expression_control_init();

    /* Capacitive touch. */
    if (!tiles_touch_init()) {
        printf("[touch] one or both MPR121 controllers failed init\n");
    }

    /* Pedal: sustain by default (setting pedal.mode). */
    tiles_pedal_init();

    /* CV/gate: off unless external power AND explicitly enabled. After power
     * init, so its power callback has a state to react to. */
    tiles_cv_gate_init();

    /* USB vendor settings interface (endpoints are already live; this resets
     * the line buffer). */
    tiles_usb_vendor_init();

    /* Hall sensors. On failure the pads that did init still scan. */
    if (!tiles_hall_init()) {
        printf("[hall] one or more pads failed sensor init -- see per-pad status\n");
    }

    /* Boot animation (~4.7 s, ends with a Hall baseline recapture), skipped
     * on a crash recovery: the sensors never lost power, so the baseline taken
     * at init is already valid. */
    if (crash_recovered) {
        printf("[boot_sequence] skipped -- crash-recovery reboot, not a fresh power-on\n");
    } else if (!tiles_boot_sequence_run()) {
        printf("[boot_sequence] post-animation Hall baseline re-capture failed for at least one pad\n");
    }

    /* Serial calibration commands. Its init only prints help text, so it's
     * skipped on a crash recovery. */
    if (!crash_recovered) {
        tiles_calibration_init();
    }

    /* Haptics: shares the PCA9685s with buttons (writes no registers at init). */
    tiles_haptics_init();

    /* Re-assert PCA9685 state on every power-mode change (see
     * tiles_power_recover_haptics_and_buttons()). Registered after buttons and
     * haptics init. */
    tiles_power_register_callback(tiles_power_recover_haptics_and_buttons);

    /* Touch + Hall -> notes, velocity, pressure, pitch bend. */
    tiles_expression_init();

    /* Standby: needs lighting/buttons (render) and touch/pedal (activity). */
    tiles_standby_init();

    /* Game mode (hold triangle + diamond + square + circle). */
    tiles_game_mode_init();

    /* The 16-channel map. Before anything reads the zone size or claims a
     * Song channel. */
    tiles_midi_channels_init();

    /* DIN MIDI in/out; works without USB. A failure disables DIN only. Its
     * MPE zone declaration is sent after settings load (below). */
    if (!tiles_din_midi_init()) {
        printf("[main] DIN MIDI unavailable (no free PIO state machine?) -- USB MIDI unaffected\n");
    }

    /* MIDI input parser. Before anything registers a callback with it (init
     * clears the table). */
    tiles_midi_in_init();

    /* MIDI Identity replies (registers a SysEx callback). */
    tiles_midi_identity_init();

    /* MIDI clock (registers a Real-Time callback). */
    tiles_midi_clock_init();

    /* Operation modes (restores mode and running lanes after a crash). */
    tiles_op_mode_init(crash_recovered);

    /* Debug mode and the 1 s watchdog. Placed last, next to the loop it
     * guards. */
    tiles_debug_mode_init();

    /* Crash indicator: red underglow after a crash-recovery boot, since the
     * boot animation that would have shown it was skipped. */
    tiles_crash_indicator_init(crash_recovered);

    /* Settings table: captures each module's defaults, then applies what was
     * saved to flash. Must follow every module init. */
    tiles_settings_boot();

    /* Content store: custom scales the companion app pushed, into the note
     * map's CUSTOM_1..9 (after tiles_note_map_init()). */
    tiles_content_init(tiles_storage_content_region_safe() ? tiles_storage_content_ops() : NULL);

    /* MPE zone declaration for DIN (USB repeats it on mount), after settings,
     * so a saved "MPE off" announces a withdrawn zone. */
    tiles_expression_announce_mpe_zone();

    while (true) {
        tiles_loop_stats_mark(time_us_32());
        /* Must run every pass: it IS the USB stack (control transfers, CDC and
         * MIDI data). pico_stdio_usb's background servicing is compiled out when
         * tinyusb_device is linked directly, so without this call nothing is
         * serviced (the console printed nothing and MIDI never reached the host). */
        tiles_debug_trace('T');
        tud_task();

        /* Re-declare the MPE zone each time USB MIDI (re)mounts: sends before
         * enumeration are dropped, and a new host needs it again. */
        static bool s_mpe_was_mounted = false;
        bool mpe_mounted_now = tud_midi_mounted();
        if (mpe_mounted_now && !s_mpe_was_mounted) {
            tiles_expression_announce_mpe_zone();
        }
        s_mpe_was_mounted = mpe_mounted_now;

        /* DIN: flush coalesced bend/pressure and keep the transmitter running. */
        tiles_din_midi_service();

        /* MIDI input, after tud_task() (fresh RX data) and before the clock and
         * op_mode scans, which react to its callbacks. */
        tiles_debug_trace('i');
        tiles_midi_in_scan();

        /* Clock, before op_mode, so the sequencer sees this pass's clock. */
        tiles_debug_trace('M');
        tiles_midi_clock_scan();

        /* Power first among the services: lighting, haptics and CV read it this
         * pass. */
        tiles_debug_trace('P');
        tiles_power_scan();
        /* Record power-mode changes (edges only) into the crash recorder's ring,
         * so a power flicker right before a freeze shows in the report. */
        {
            static tiles_power_mode_t s_debug_last_power_mode = (tiles_power_mode_t)0xFFu;
            tiles_power_mode_t mode_now = tiles_power_get_state().mode;
            if (mode_now != s_debug_last_power_mode) {
                static const char *const power_mode_names[] = {
                    "USB_ONLY",
                    "EXTERNAL_ONLY",
                    "USB_AND_EXTERNAL",
                    "FAULT",
                };
                char buf[32];
                snprintf(buf, sizeof(buf), "[power->%s]", power_mode_names[mode_now]);
                tiles_debug_trace_str(buf);
                s_debug_last_power_mode = mode_now;
            }
        }
        tiles_debug_trace('B');
        tiles_buttons_scan();
        /* Needs fresh button state. */
        tiles_debug_mode_scan();
        /* Needs fresh buttons; before lighting, so a dismiss shows this frame. */
        tiles_debug_trace('R');
        tiles_crash_indicator_scan();
        /* After buttons ("-"/"+"). */
        tiles_debug_trace('o');
        tiles_octave_control_scan();
        tiles_debug_trace('u');
        tiles_touch_scan();
        tiles_debug_trace('d');
        tiles_pedal_scan();
        tiles_debug_trace('C');
        tiles_cv_gate_scan();
        tiles_debug_trace('v');
        tiles_usb_vendor_scan();
        /* Saves changed settings (debounced, pads idle). After the vendor scan, so
         * a SET is seen the same pass. */
        tiles_settings_scan();
        /* After buttons and touch, before expression (which reads whether the
         * expression menu owns the grid). */
        tiles_debug_trace('E');
        tiles_expression_control_scan();
        /* After buttons and touch. */
        tiles_debug_trace('G');
        tiles_game_mode_scan();
        /* After buttons, touch and the clock. */
        tiles_debug_trace('S');
        tiles_op_mode_scan();
        /* Standby, skipped while game mode, transpose, the expression menu or an
         * op_mode menu/grid mode owns the display, so its idle timer can't take
         * over the LEDs under them. */
        if (!tiles_game_mode_is_active() && !tiles_octave_control_is_transpose_active() &&
            !tiles_expression_control_owns_pad_grid() && !tiles_op_mode_owns_pad_grid()) {
            tiles_debug_trace('Y');
            tiles_standby_scan();
        }
        tiles_debug_trace('L');
        tiles_lighting_service();
        tiles_debug_trace('H');
        tiles_hall_scan();
        /* Non-blocking. */
        tiles_debug_trace('c');
        tiles_calibration_scan();
        /* After touch and Hall. */
        tiles_debug_trace('X');
        tiles_expression_scan();
        /* After expression, which triggers/updates/stops the envelopes. */
        tiles_debug_trace('K');
        tiles_haptics_scan();
        /* End of pass in the debug trace; push it to the host. No-ops unless
         * debug mode is on. */
        tiles_debug_trace_str("\r\n");
        tiles_debug_trace_flush();

        /* Feed the watchdog LAST: if anything above hangs, this isn't reached, the
         * chip resets after 1 s, and the crash recorder keeps the trace. */
        watchdog_update();

        /* No periodic diagnostic printing in the loop: with no terminal attached,
         * each blocked printf can stall the loop for the stdio timeout (a 2 s
         * status dump once caused multi-second freezes). Boot-time checks only. */

        /* No sleep: expression.c's strike detection wants as many passes as
         * possible. Pass time depends on I2C traffic (more with pads held) and
         * hasn't been measured. */
    }

    return 0;
}
