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
 * implemented. CV/gate (services/cv_gate.h) mirrors this instrument's
 * own note stream to a standard monophonic 1V/octave pitch CV + gate +
 * pressure CV, hard-gated on services/power.h's own external-power
 * confirmation and defaulting off even when power is present -- not
 * yet confirmed against real hardware (drivers/dac80502.h is a new,
 * unverified driver, see its own header comment). usb_vendor/usb_
 * vendor.h is a first, deliberately simple line-based GET/SET/LIST
 * settings protocol over a new USB vendor interface -- pedal mode/
 * polarity, MPE enabled, pitch-bend/aftertouch sensitivity, CV/gate
 * enable + calibration -- proven with tools/tiles_control.py, not yet
 * the real Electron companion app or the fuller protocol (pad remap,
 * calibration capture, live sensor streaming, profiles, firmware
 * update) docs/protocol/README.md's own design notes describe. Not yet
 * built: DIN, a real per-pad Hall calibration curve (this is capture
 * only, no curve is derived or applied yet) -- added module by module
 * per the bring-up order in docs/hardware/SENTIA_FIRMWARE_CODEX_START.md.
 * Each phase must leave this file building and the previous phase's
 * safety guarantees intact.
 */

#include <stdio.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "tusb.h"

#include "board/board_init.h"
#include "board/unit_id.h"
#include "diagnostics/calibration.h"
#include "diagnostics/i2c_scan.h"
#include "midi/din_midi.h"
#include "midi/midi_in.h"
#include "midi/midi_out.h"
#include "midi/usb_device.h"
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
#include "services/midi_clock.h"
#include "services/note_map.h"
#include "services/octave_control.h"
#include "services/op_mode.h"
#include "services/pedal.h"
#include "services/power.h"
#include "services/standby.h"
#include "services/touch.h"
#include "usb_vendor/usb_vendor.h"

/* See the registration call site (tiles_power_register_callback()
 * below) for the full reasoning -- fires on every debounced power-mode
 * change, re-asserting known-good hardware state on both PCA9685 chips
 * (shared by button LEDs and all 24 haptic motors) in case the actual
 * source-switch transient silently reset them without resetting this
 * MCU. Re-asserts board_pca9685_enable_outputs() (the OE gate) first as
 * cheap insurance -- that's a plain GPIO the MCU itself holds, so a
 * chip-only glitch shouldn't have touched it, but it costs nothing to
 * reassert -- then the two subsystems' own resync functions, chips
 * first. `new_state` is unused: both resync functions re-derive
 * whatever they need live rather than trusting a value handed in at
 * callback-registration time. */
static void tiles_power_recover_haptics_and_buttons(tiles_power_state_t new_state) {
    (void)new_state;
    board_pca9685_enable_outputs();
    tiles_buttons_resync_pca9685();
    tiles_haptics_resync_hardware();
    printf("[power] recovered haptics/buttons hardware after a power-mode change\n");
}

int main(void) {
    /* Real feedback, computing an honest answer to "how long will boot
     * take if [a watchdog recovery] happens now": every printf() below
     * that fires unconditionally at boot -- this banner, the 9 calls
     * inside tiles_diag_i2c_scan_expected_devices(), calibration.c's own
     * help text -- can each individually block up to
     * PICO_STDIO_USB_STDOUT_TIMEOUT_US (500ms) whenever nobody has a
     * terminal open to drain the USB-CDC buffer, exactly the ordinary
     * case once TILES is plugged into a DAW rather than a dev machine.
     * That's ~11 calls, up to ~5.5s worst case, on EVERY boot -- was
     * already true before tonight, just never mattered until recovery
     * SPEED became the actual point rather than "eventually recovers."
     * Computed once, this early -- watchdog_enable_caused_reboot() is a
     * raw hardware scratch-register read, safe to call before ANY
     * peripheral init, board_init() included -- and reused everywhere
     * below a boot-time print/scan is skippable, instead of leaving
     * each site to call it again fresh. A genuine fresh power-on still
     * gets the full, useful diagnostic output; a crash-recovery boot
     * gets back to running as fast as USB re-enumeration itself takes,
     * not that plus several more seconds of prints nobody's there to
     * read anyway during a live set. */
    bool crash_recovered = watchdog_enable_caused_reboot();

    /* Must run right here, before literally anything else below --
     * see services/debug_mode.h's own comment on tiles_debug_mode_
     * capture_crash_snapshot() for the finding that made this move
     * necessary: every crash report captured this session showed the
     * SAME early write_pad() signature instead of the real freeze,
     * because the snapshot copy used to happen from inside tiles_
     * debug_mode_init() at its usual spot near the bottom of this
     * function -- by then, tiles_lighting_init() and several other
     * init calls below had already overwritten the very ring buffer
     * that copy was meant to preserve. Only touches __uninitialized_ram
     * state, no hardware dependency, so nothing stops it running this
     * early. */
    tiles_debug_mode_capture_crash_snapshot(crash_recovered);

    /* Must run before stdio_init_all(): with tinyusb_device linked
     * explicitly (see CMakeLists.txt), pico_stdio_usb expects us to have
     * already called tusb_init() with our own composite CDC+MIDI
     * descriptors -- see midi/usb_device.h. */
    tiles_usb_device_init();

    stdio_init_all();

    if (!crash_recovered) {
        printf("[main] SENTIA TILES unit %u/%u\n", (unsigned)TILES_UNIT_NUMBER, (unsigned)TILES_UNIT_COUNT);
    }

    board_init();

    /* Phase 2 bring-up: confirm every expected I2C device ACKs before
     * bringing up anything that talks to one. Skipped on a crash-
     * recovery boot -- see this function's own opening comment; nothing
     * about the I2C devices themselves changed since a moment ago, a
     * software reset didn't unplug anything, so re-verifying presence
     * here buys nothing but several more seconds of unread prints. */
    if (!crash_recovered) {
        tiles_diag_i2c_scan_expected_devices();
    }

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

    /* Scale/octave-shift/key-offset -- real feedback: "we need to make
     * sure it reboots to last state completely includeing sequence,
     * layout, scale, play state." No hardware dependency of its own,
     * but must run before tiles_octave_control_init() just below, its
     * first real consumer. See services/note_map.h's own comment on
     * tiles_note_map_init(). */
    tiles_note_map_init(crash_recovered);

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

    /* CV/gate: hard-gated on external power + its own explicit enable,
     * both defaulting to "off" -- see services/cv_gate.h. Needs
     * tiles_power_init() (already run above) so tiles_power_register_
     * callback() has a real state to react to from the start. */
    tiles_cv_gate_init();

    /* USB vendor interface: the settings half of "the control
     * software" -- see usb_vendor/usb_vendor.h. TinyUSB's own vendor
     * endpoints are already live from tiles_usb_device_init() above;
     * this just resets this module's own line-assembly buffer. */
    tiles_usb_vendor_init();

    /* Phase 4 bring-up: one Hall sensor at a time, then the full 24-pad
     * scan (see SENTIA_FIRMWARE_CODEX_START.md). A false return means
     * at least one pad's sensor failed identify/init -- tiles_hall_scan()
     * still runs for whichever pads did succeed rather than refusing to
     * start, per "a failed subsystem disables itself, it doesn't block
     * the rest." */
    if (!tiles_hall_init()) {
        printf("[hall] one or more pads failed sensor init -- see per-pad status\n");
    }

    /* Real feedback: "the reboot is not acceptable since it takes too
     * long... why would it reboot if power is tabl[e]. it might loose
     * conection but not reboot." While the underlying hang that makes
     * the watchdog reset in the first place is still being tracked down
     * (see services/README.md's own history), a crash-recovery reboot
     * should get back to USABLE as fast as USB re-enumeration itself
     * takes, not that PLUS this animation's own ~4.7s on top -- even
     * now that it correctly pumps tud_task() throughout (see boot_
     * sequence.c/.h's own history just below), that's still real wall-
     * clock time nobody asked to sit through a second time. Reuses
     * this function's own early crash_recovered (see main()'s opening
     * comment) instead of calling watchdog_enable_caused_reboot() a
     * second time here -- it's a raw hardware scratch-register read,
     * so calling it again would still be harmless, just redundant now
     * that one early read covers every boot-time skip in this
     * function, this one included. Skipping the Hall-baseline
     * recapture this animation would otherwise also do is correct
     * here, not just incidental to skipping the animation: that
     * recapture's own justification (a few settled seconds since a
     * just-power-cycled MCU) doesn't apply after a WARM reset at all --
     * the sensors never lost power, so the baseline tiles_hall_init()
     * already captured a moment ago this exact boot is exactly as
     * valid as one taken 4 more seconds from now would be. */
    if (crash_recovered) {
        printf("[boot_sequence] skipped -- crash-recovery reboot, not a fresh power-on\n");
    } else if (!tiles_boot_sequence_run()) {
        printf("[boot_sequence] post-animation Hall baseline re-capture failed for at least one pad\n");
    }

    /* Serial-driven Hall calibration capture -- needs tiles_hall_init()
     * above already run. See diagnostics/calibration.h. Skipped on a
     * crash-recovery boot along with the other boot-time-only prints
     * above (see main()'s opening comment): tiles_calibration_init()'s
     * only effect is printing its own help text over CDC, which can
     * itself block up to 500ms with nobody connected to drain it, and
     * a warm reset doesn't change what that text would say anyway. */
    if (!crash_recovered) {
        tiles_calibration_init();
    }

    /* Haptics: needs tiles_buttons_init() (above) already run, since it
     * shares both PCA9685 chip instances with buttons rather than
     * re-initializing them -- see services/haptics.h. Doesn't write any
     * PCA9685 register itself (every channel is already "full off" from
     * buttons' own init), so exact ordering relative to
     * board_pca9685_enable_outputs() above doesn't matter electrically;
     * placed here because services/expression.c is what actually drives
     * it. */
    tiles_haptics_init();

    /* Real feedback: "pulling power plug killed haptics tho. power
     * managment is not ready yet." A real power-source switch physically
     * disturbs the rail both PCA9685 chips (button LEDs + all 24 haptic
     * motors) live on -- a brief glitch there can silently reset their
     * own internal config/PWM state without resetting the RP2350 itself,
     * leaving this firmware's own tracked state (which pad is mid-
     * SUSTAIN, which button is held) completely correct and unaware
     * anything happened, while the actual motor/LED hardware silently
     * reverted to power-on defaults. Re-asserts the known-good hardware
     * state on every debounced power-mode change (services/power.h),
     * not just once at boot -- see services/buttons.h's
     * tiles_buttons_resync_pca9685() and services/haptics.h's
     * tiles_haptics_resync_hardware() for what each half actually does
     * and why the order (chips reconfigured first, then each subsystem
     * repaints its own state on top) matters. Registered after both
     * buttons_init() and haptics_init() above, since this can fire
     * (in principle) the moment the debounce window closes. */
    tiles_power_register_callback(tiles_power_recover_haptics_and_buttons);

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

    /* DIN MIDI jacks (IN + OUT) -- real feedback: "are midi plugs
     * working?" / "yes build DIN MIDI". Independent of USB: works with no
     * host at all. A failure here disables DIN only (printed, nothing
     * else blocked). On success, sends the MPE zone configuration once
     * now, while only DIN is up -- the USB mount below repeats it for USB.
     * See midi/din_midi.h. */
    if (tiles_din_midi_init()) {
        tiles_midi_mpe_init();
    } else {
        printf("[main] DIN MIDI unavailable (no free PIO state machine?) -- USB MIDI unaffected\n");
    }

    /* Shared MIDI IN parser (USB + DIN sources) -- must run before
     * tiles_midi_clock_init() below, which registers a callback with it;
     * this resets that registration table, so registering before this
     * ran would get silently wiped. See midi/midi_in.h. */
    tiles_midi_in_init();

    /* MIDI clock RX (USB MIDI IN) -- the timing source op_mode.h's
     * sequencer mode runs from. See services/midi_clock.h. */
    tiles_midi_clock_init();

    /* Operation modes (melodic/chord/sequencer/arp), SW4/diamond single
     * click -- needs lighting/buttons/touch already initialized, same as
     * standby/game_mode above, whose rendering path it shares. Reuses
     * this function's own early crash_recovered -- real feedback: "we
     * need to make sure it reboots to last state completely includeing
     * sequence, layout, scale, play state." See services/op_mode.h. */
    tiles_op_mode_init(crash_recovered);

    /* Real-hardware trace logging (diamond+square+circle held 8s to
     * toggle) -- see services/debug_mode.h's own header for the full
     * "why not printf()" reasoning. No hardware dependency, so no
     * particular init ordering requirement; placed last simply to be
     * near the loop it instruments. */
    tiles_debug_mode_init();

    /* Real feedback: "we need an indicator for crash now that we skip
     * boot sequence" -- the animation used to be the only visible sign
     * a fresh boot had just happened; skipping it on crash-recovery
     * (see this function's own opening comment) left that kind of boot
     * silent. Reuses this function's own early crash_recovered rather
     * than re-deriving it -- see services/crash_indicator.h for the
     * full feature (underglow pulse + circle-held-alone-2s dismiss). */
    tiles_crash_indicator_init(crash_recovered);

    /* The settings table (profiles/settings.h): captures every setting's boot
     * default from its module, then restores whatever was last saved to flash
     * (pedal/expression/CV calibration, LED look, DIN polarity, harmonics...).
     * Must run AFTER every module a setting binds to has initialized -- all of
     * them have by here -- and before the loop. Real feedback: "yes start
     * with the settings table and flash saving." */
    tiles_settings_boot();

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
        tiles_debug_trace('T');
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

        /* Feeds coalesced pitch bend/pressure/expression into the DIN
         * transmit queue and keeps the transmitter running. Cheap when
         * nothing is pending. See midi/din_midi.h. */
        tiles_din_midi_service();

        /* Drains USB MIDI IN (Real-Time bytes AND, since Scene Launch
         * mode, SysEx) -- needs tud_task() above already run this
         * iteration so the RX FIFO is current, and must run before
         * tiles_midi_clock_scan() below (which reacts to a callback this
         * fires, rather than reading MIDI itself) and tiles_op_mode_scan()
         * further down (which owns the Scene Launch SysEx callback). See
         * midi/midi_in.h. */
        tiles_debug_trace('i');
        tiles_midi_in_scan();

        /* Reacts to whatever Start/Continue/Stop/Clock bytes the scan
         * just above found -- must run before tiles_op_mode_scan() below
         * so this tick's fresh clock state is what sequencer mode sees.
         * See services/midi_clock.h. */
        tiles_debug_trace('M');
        tiles_midi_clock_scan();

        /* Runs first: lighting's ceiling_level() and any future
         * haptics/CV consumer read tiles_power_get_state() during this
         * same iteration, so the debounced state should already be
         * current by the time anything else runs. */
        tiles_debug_trace('P');
        tiles_power_scan();
        /* Real feedback, testing the crash-dig-in above: "yeah [do that]
         * and implement in case of crash a report is generated capturing
         * last activity before blackout" -- edge-triggered (only on an
         * actual MODE change, never periodic) so this can't repeat the
         * exact "unconditional printf every N ms" mistake main.c's own
         * history above already made once with this same power-state
         * data. Recorded into debug_mode.c's always-on ring buffer (see
         * that file's own header) regardless of whether live debug mode
         * is currently toggled, specifically so a genuine power-mode
         * flicker right before a freeze -- the user's own suspicion,
         * given a new cable/direct power stopped reproducing it -- would
         * show up in a crash report even if nobody was watching live
         * when it happened. */
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
        /* Needs fresh button state, same as tiles_octave_control_scan()
         * just below -- see services/debug_mode.h's own header. */
        tiles_debug_mode_scan();
        /* Needs fresh button state (circle-held-alone dismiss). Must
         * run before tiles_lighting_service() (below) so a dismiss that
         * just happened this tick is already reflected in this same
         * tick's render. See services/crash_indicator.h. */
        tiles_debug_trace('R');
        tiles_crash_indicator_scan();
        /* Must run after tiles_buttons_scan() so this iteration's
         * debounced SW1/SW2 state is fresh. See
         * services/octave_control.h. */
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
        /* Notices changed settings (from the USB shell, or on-device) and
         * saves them to flash -- debounced, and only with the pads idle. Must
         * run after tiles_usb_vendor_scan() so a SET is seen the same tick. */
        tiles_settings_scan();
        /* Must run after tiles_buttons_scan() (fresh circle/square
         * state) and tiles_touch_scan() (fresh touch state for the
         * expression sub-menu's slider taps) above, and before
         * tiles_expression_scan() below so this tick's fresh "does the
         * sub-menu own the grid" state gates new-strike suppression
         * correctly. See services/expression_control.h. */
        tiles_debug_trace('E');
        tiles_expression_control_scan();
        /* Must run after tiles_buttons_scan()/tiles_touch_scan() above
         * so this iteration's entry-gesture/in-game-control/menu-
         * selection input is fresh. See services/game_mode.h. */
        tiles_debug_trace('G');
        tiles_game_mode_scan();
        /* Must run after tiles_buttons_scan()/tiles_touch_scan()
         * (diamond click + menu/step taps) and tiles_midi_clock_scan()
         * (fresh clock state for sequencer playback) above. See
         * services/op_mode.h. */
        tiles_debug_trace('S');
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
            tiles_debug_trace('Y');
            tiles_standby_scan();
        }
        tiles_debug_trace('L');
        tiles_lighting_service();
        tiles_debug_trace('H');
        tiles_hall_scan();
        /* Non-blocking (zero-timeout stdio read) -- cheap even when
         * nothing has been typed. See diagnostics/calibration.h. */
        tiles_debug_trace('c');
        tiles_calibration_scan();
        /* Must run after both tiles_touch_scan() and tiles_hall_scan()
         * above so it sees this iteration's fresh data from both. */
        tiles_debug_trace('X');
        tiles_expression_scan();
        /* Advances KICK -> GAP -> SUSTAIN timing for any pad
         * expression_scan() just triggered/updated/stopped this
         * iteration. */
        tiles_debug_trace('K');
        tiles_haptics_scan();
        /* One line per full loop iteration, and pushed to the host now
         * rather than waiting for TinyUSB's own fill-a-packet auto-flush
         * -- see services/debug_mode.h's own tiles_debug_trace_flush()
         * comment. Both no-ops while debug mode is inactive. */
        tiles_debug_trace_str("\r\n");
        tiles_debug_trace_flush();

        /* Real feedback: "let it run until faliure and then you can
         * check the log for the final thing before crashing." Placed
         * LAST, after every single stage above has already run this
         * iteration -- if anything upstream hangs, this call is never
         * reached, so services/debug_mode.h's own DEBUG_WATCHDOG_
         * TIMEOUT_MS (1 second, armed in tiles_debug_mode_init() above)
         * resets the chip instead of leaving it frozen forever, and
         * whatever the trace ring held at that exact moment survives the
         * reset intact (see that file's own header for the full
         * mechanism) for the next debug-mode entry to report. */
        watchdog_update();

        /* A periodic (every 2s) unconditional bring-up dump used to live
         * here -- tiles_diag_i2c_scan_expected_devices() (9 printf calls)
         * plus 3 more for power/Hall/standby state, 12 total, forever,
         * whether or not a serial terminal was ever attached to drain
         * them. Real feedback: "froze again with enough time" -- with no
         * terminal open (the normal case once TILES is plugged into a
         * DAW/synth rig rather than a dev machine), the USB-CDC TX buffer
         * fills and every printf() call blocks up to
         * PICO_STDIO_USB_STDOUT_TIMEOUT_US (500ms) waiting for room that
         * never appears (same bug class as this file's own haptics.c
         * entry above, and the pitch-bend one before it) -- except this
         * one didn't need capture mode or a dense external clock to
         * trigger it, just the device staying powered on for a couple of
         * minutes, since it fired on a plain wall-clock timer regardless
         * of what the player was doing. Up to 12 * 500ms = 6s of main
         * loop stall possible every 2s once the buffer settled into
         * "always full" -- easily read as a full freeze. Removed outright
         * rather than gated on stdio_usb_connected(): both prints were
         * already self-labeled "Temporary bring-up visibility... replace
         * with a real usb_vendor/ diagnostics stream once that exists",
         * and a connected-but-idle terminal can still let the buffer fill
         * regardless, which a connection check wouldn't catch. The
         * one-shot boot-time tiles_diag_i2c_scan_expected_devices() call
         * above (tiles_op_mode_init() region) is unaffected -- fires once
         * before the loop even starts, not on a recurring timer. */

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
