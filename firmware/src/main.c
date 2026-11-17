/* SENTIA TILES firmware entry point: brings every subsystem up in the safe
 * boot order, then runs the cooperative main loop. Outputs stay off until
 * configured; a failed subsystem disables itself; a crash-recovery boot
 * skips the slow steps. Modules: firmware/README.md. */

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "tusb.h"

#include "board/board_init.h"
#include "board/unit_id.h"
#include "diagnostics/boot_stage.h"
#include "diagnostics/fault.h"
#include "midi/product_identity.h"
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
#include "services/device_mode.h"
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
#include "services/play_state.h"
#include "services/power.h"
#include "services/show_link.h"
#include "services/standby.h"
#include "services/touch.h"
#include "storage/storage_flash.h"
#include "usb_vendor/usb_vendor.h"
#include "audio/sample_stream.h"
#include "audio/usb_audio.h"

/* A power switch can reset the PCA9685s without the MCU: re-assert OE,
 * reconfigure, then let buttons and haptics repaint. */
static void tiles_power_recover_haptics_and_buttons(tiles_power_state_t new_state) {
    (void)new_state;
    board_pca9685_enable_outputs();
    tiles_buttons_resync_pca9685();
    tiles_haptics_resync_hardware();
    printf("[power] recovered haptics/buttons hardware after a power-mode change\n");
}

/* See the main loop. */
static void timing_tick(void) {
    tiles_debug_trace('t');
    tiles_usb_task();
    tiles_midi_in_scan();
    tiles_midi_clock_scan();
    tiles_op_mode_playback_tick();
}

#define BOOT_WATCHDOG_MS 8000u

int main(void) {
    /* Above the 150 MHz rating the UART and SPI run from the USB PLL's
     * 48 MHz (I2C and PIO follow the system clock either way). */
    if (clock_get_hz(clk_sys) > 150000000u) {
        clock_configure_undivided(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, USB_CLK_HZ);
    }
    /* Read once, first: a crash-recovery boot skips the boot prints (each can
     * block on the stdio timeout) and the animation. */
    bool crash_recovered = watchdog_enable_caused_reboot();

    /* Before ANYTHING else: every init below writes crash-recorder traces into
     * the same ring buffer this snapshots (see debug_mode.h). */
    tiles_debug_mode_capture_crash_snapshot(crash_recovered);
    tiles_boot_stage_start(crash_recovered);
    tiles_fault_boot();

    /* A watchdog from here, not only from debug mode's 1 s one before the
     * loop: a hang during boot froze the board for good (no reset, USB
     * already enumerated). 8 s covers the ~4.2 s animation, which feeds it. */
    watchdog_enable(BOOT_WATCHDOG_MS, true);

    /* Controller or standalone mode: before USB, whose descriptor depends
     * on it (services/device_mode.h). The sampler's slots in both: they can
     * be loaded in either, and only sound in standalone mode. */
    tiles_boot_stage("device_mode");
    tiles_device_mode_boot();
    tiles_boot_stage("sample_stream");
    tiles_sample_stream_init();

    /* Before stdio_init_all(): with tinyusb_device linked, pico_stdio_usb
     * expects tusb_init() already done with our descriptors
     * (midi/usb_device.h). In standalone mode core 1 does it. */
    tiles_boot_stage("usb");
    if (tiles_device_mode_is_standalone()) {
        tiles_usb_audio_start();
    } else {
        tiles_usb_device_init(false);
    }

    tiles_boot_stage("stdio");
    stdio_init_all();
    if (tiles_device_mode_is_standalone()) {
        tiles_usb_console_without_task();
    }

    if (!crash_recovered) {
        printf("[main] SENTIA TILES unit %u/%u\n", (unsigned)TILES_UNIT_NUMBER, (unsigned)TILES_UNIT_COUNT);
    }

    tiles_boot_stage("board_init");
    board_init();

    /* I2C discovery before anything talks to a device. Skipped on a crash
     * recovery (a warm reset didn't unplug anything). */
    if (!crash_recovered) {
        tiles_diag_i2c_scan_expected_devices();
    }

    /* Raise both buses to 400 kHz now that discovery ran at 100 kHz. */
    tiles_boot_stage("i2c_speed");
    board_i2c_set_run_speed();

    /* Power mode (GP22 + USB mounted). Before lighting, whose brightness
     * ceiling reads it on its first sweep. tud_mounted() is valid once
     * tusb_init() ran, even before enumeration. */
    tiles_boot_stage("power");
    tiles_power_init();

    /* Lighting needs only the TCA9554 (I2C1), so a partly populated board
     * still lights up. */
    tiles_boot_stage("lighting");
    if (!tiles_lighting_init()) {
        printf("[lighting] init failed -- check I2C1/TCA9554 and PIO availability\n");
    }

    /* Both PCA9685s: channels off, MODE2.OUTDRV=1 (safe boot step 6). Early,
     * so the active-low button LEDs are turned dark as soon as possible. */
    tiles_boot_stage("buttons");
    if (!tiles_buttons_init()) {
        printf("[buttons] one or both PCA9685 devices failed init\n");
    }

    /* PCA9685 outputs stay disabled (OE) until every channel is in its
     * intended state (after buttons init). */
    board_pca9685_enable_outputs();

    /* Scale/octave/key (kept across a crash reboot). Before octave control,
     * its first user. */
    tiles_boot_stage("note_map");
    tiles_note_map_init(crash_recovered);

    /* ...and across a power cycle: the saved scale/key/octave go back into
     * note_map now, the saved mode at op_mode init. */
    tiles_boot_stage("play_state");
    tiles_play_state_init(tiles_storage_state_region_safe() ? tiles_storage_state_ops() : NULL, crash_recovered);

    /* "-"/"+": octave shift (claims their LEDs; needs buttons init). */
    tiles_boot_stage("octave");
    tiles_octave_control_init();

    /* Square: pitch bend toggle and the expression menu (claims its LED;
     * needs buttons init). */
    tiles_boot_stage("expression_control");
    tiles_expression_control_init();

    /* Capacitive touch. */
    tiles_boot_stage("touch");
    if (!tiles_touch_init()) {
        printf("[touch] one or both MPR121 controllers failed init\n");
    }

    /* Pedal: sustain by default (setting pedal.mode). */
    tiles_boot_stage("pedal");
    tiles_pedal_init();

    /* CV/gate: off unless external power AND explicitly enabled. After power
     * init, so its power callback has a state to react to. */
    tiles_boot_stage("cv_gate");
    tiles_cv_gate_init();

    /* USB vendor settings interface (endpoints are already live; this resets
     * the line buffer). */
    tiles_boot_stage("usb_vendor");
    tiles_usb_vendor_init();

    /* Hall sensors. On failure the pads that did init still scan. */
    tiles_boot_stage("hall");
    if (!tiles_hall_init()) {
        printf("[hall] one or more pads failed sensor init -- see per-pad status\n");
    }

    /* Boot animation (~4.2 s, ends with a Hall baseline recapture), skipped
     * on a crash recovery: the sensors never lost power, so the baseline taken
     * at init is already valid. */
    tiles_boot_stage("animation");
    if (crash_recovered) {
        printf("[boot_sequence] skipped -- crash-recovery reboot, not a fresh power-on\n");
    } else if (tiles_device_mode_quick_restart()) {
        printf("[boot_sequence] skipped -- restarted into the mode just chosen\n");
    } else if (!tiles_boot_sequence_run()) {
        printf("[boot_sequence] post-animation Hall baseline re-capture failed for at least one pad\n");
    }

    /* Serial calibration commands. Its init only prints help text, so it's
     * skipped on a crash recovery. */
    tiles_boot_stage("calibration");
    if (!crash_recovered) {
        tiles_calibration_init();
    }

    /* Haptics: shares the PCA9685s with buttons (writes no registers at init). */
    tiles_boot_stage("haptics");
    tiles_haptics_init();

    /* Re-assert PCA9685 state on every power-mode change (see
     * tiles_power_recover_haptics_and_buttons()). Registered after buttons and
     * haptics init. */
    tiles_power_register_callback(tiles_power_recover_haptics_and_buttons);

    /* Touch + Hall -> notes, velocity, pressure, pitch bend. */
    tiles_boot_stage("expression");
    tiles_expression_init();

    /* Standby: needs lighting/buttons (render) and touch/pedal (activity). */
    tiles_boot_stage("standby");
    tiles_standby_init();

    /* Game mode (hold triangle + diamond + square + circle). */
    tiles_boot_stage("game_mode");
    tiles_game_mode_init();

    /* The 16-channel map. Before anything reads the zone size or claims a
     * Song channel. */
    tiles_boot_stage("midi_channels");
    tiles_midi_channels_init();

    /* DIN MIDI in/out; works without USB. A failure disables DIN only. Its
     * MPE zone declaration is sent after settings load (below). */
    tiles_boot_stage("din_midi");
    if (!tiles_din_midi_init()) {
        printf("[main] DIN MIDI unavailable (no free PIO state machine?) -- USB MIDI unaffected\n");
    }

    /* MIDI input parser. Before anything registers a callback with it (init
     * clears the table). */
    tiles_boot_stage("midi_in");
    tiles_midi_in_init();

    /* MIDI Identity replies (registers a SysEx callback). */
    tiles_boot_stage("identity");
    tiles_midi_identity_init();

    /* A computer's light show over SysEx (registers a SysEx callback). */
    tiles_boot_stage("show_link");
    tiles_show_link_init();

    /* MIDI clock (registers a Real-Time callback). */
    tiles_boot_stage("midi_clock");
    tiles_midi_clock_init();

    /* Operation modes (restores mode and running lanes after a crash). */
    tiles_boot_stage("op_mode");
    tiles_op_mode_init(crash_recovered);

    /* Debug mode and the 1 s watchdog. Placed last, next to the loop it
     * guards. */
    tiles_boot_stage("debug_mode");
    tiles_debug_mode_init();

    /* Crash indicator: red underglow after a crash-recovery boot (the
     * animation that would have shown it was skipped), and after a crash
     * the firmware caught itself (fault.c). */
    tiles_crash_indicator_init(crash_recovered || tiles_fault_restarted());

    /* Settings table: captures each module's defaults, then applies what was
     * saved to flash. Must follow every module init. */
    tiles_boot_stage("settings");
    tiles_settings_boot();

    /* Content store: custom scales the companion app pushed, into the note
     * map's CUSTOM_1..9 (after tiles_note_map_init()). */
    tiles_boot_stage("content");
    tiles_content_init(tiles_storage_content_region_safe() ? tiles_storage_content_ops() : NULL);

    /* MPE zone declaration for DIN (USB repeats it on mount), after settings,
     * so a saved "MPE off" announces a withdrawn zone. */
    tiles_expression_announce_mpe_zone();

    /* The last caught crash into flash: last before the loop, with core 1
     * (started with USB in standalone mode) able to be paused. */
    tiles_boot_stage("fault_store");
    char firmware[16];
    if (TILES_FW_POINT == 0u) {
        snprintf(firmware, sizeof(firmware), "%s.%u", TILES_FW_STAGE_NAME, (unsigned)TILES_FW_BUILD);
    } else {
        snprintf(firmware, sizeof(firmware), "%s.%u.%u", TILES_FW_STAGE_NAME, (unsigned)TILES_FW_BUILD,
                 (unsigned)TILES_FW_POINT);
    }
    tiles_fault_store_init(tiles_storage_fault_region_safe() ? tiles_storage_fault_ops() : NULL, firmware);

    /* Between the slower services: USB, MIDI in, clock and playback again, so a
     * Clock arriving mid-pass is stepped within a fraction of a pass. */
    /* After a crash restart, notes a host or a DIN synth was holding never get
     * their Note-Off: all notes off now (DIN, and USB if it's back already)
     * and again once USB mounts. */
    bool panic_on_mount = crash_recovered || tiles_fault_restarted();
    if (panic_on_mount) {
        tiles_midi_send_panic();
    }
    tiles_boot_stage("loop");
    while (true) {
        tiles_loop_stats_mark(time_us_32());
        /* Must run every pass: it is the USB stack (pico_stdio_usb's background
         * servicing is compiled out with tinyusb_device linked directly). */
        tiles_debug_trace('T');
        tiles_usb_task();
        tiles_device_mode_service(to_ms_since_boot(get_absolute_time()));

        /* Re-declare the MPE zone each time USB MIDI (re)mounts: sends before
         * enumeration are dropped, and a new host needs it again. */
        static bool s_mpe_was_mounted = false;
        bool mpe_mounted_now = tud_midi_mounted();
        if (mpe_mounted_now && !s_mpe_was_mounted) {
            if (panic_on_mount) {
                tiles_midi_send_panic();
                panic_on_mount = false;
            }
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
        tiles_usb_audio_set_tempo(60000.0f / tiles_midi_clock_get_ms_per_beat());
        tiles_usb_audio_follow_clock(tiles_midi_clock_is_running(), tiles_midi_clock_song_pulse());

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
        /* A motor chip that lost power on its own comes back asleep: set it
         * up again and put the motors back (buttons.h). */
        if (tiles_buttons_check_pca9685(to_ms_since_boot(get_absolute_time()))) {
            tiles_haptics_resync_hardware();
        }
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
        timing_tick();
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
        } else {
            tiles_standby_hold_off();
        }
        tiles_show_link_service();
        tiles_debug_trace('L');
        tiles_lighting_service();
        timing_tick();
        tiles_debug_trace('H');
        tiles_hall_scan();
        timing_tick();
        /* Non-blocking. */
        tiles_debug_trace('c');
        tiles_calibration_scan();
        /* After touch and Hall. */
        tiles_debug_trace('X');
        tiles_expression_scan();
        /* After expression, which triggers/updates/stops the envelopes. */
        tiles_debug_trace('K');
        tiles_haptics_scan();
        timing_tick();
        /* End of pass in the debug trace; push it to the host. No-ops unless
         * debug mode is on. */
        tiles_debug_trace_str("\r\n");
        tiles_debug_trace_flush();

        /* Feed the watchdog LAST: if anything above hangs, this isn't reached, the
         * chip resets after 1 s, and the crash recorder keeps the trace. */
        watchdog_update();
        tiles_fault_watch_core1();

        /* No periodic diagnostic printing in the loop: with no terminal attached,
         * each blocked printf can stall the loop for the stdio timeout (a 2 s
         * status dump once caused multi-second freezes). Boot-time checks only. */

        /* No sleep: expression.c's strike detection wants as many passes as
         * possible. Pass time depends on I2C traffic (more with pads held) and
         * hasn't been measured. */
    }

    return 0;
}
