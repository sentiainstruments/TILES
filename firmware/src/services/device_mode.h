#pragma once

/* Controller mode or standalone mode (README, "Controller and standalone
 * mode"). Controller: MIDI/MPE, settings and the console over USB, the
 * sound core off. Standalone: TILES's own sound too, a stereo USB audio
 * input, with USB run on core 1 so every 1 ms packet goes out on time.
 * The setting device.mode picks it; a tap or hold of triangle during the
 * boot animation switches to the other one. Its final pulse shows which:
 * Sentia magenta for standalone, periwinkle blue for controller only.
 * The USB descriptor differs, so a change restarts TILES; a restart for a
 * switch skips the animation. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_DEVICE_MODE_SETTING_ID 0x0B00u

typedef enum {
    TILES_DEVICE_CONTROLLER = 0,
    TILES_DEVICE_STANDALONE = 1,
} tiles_device_mode_t;

/* First thing at boot, before USB: the saved mode. */
void tiles_device_mode_boot(void);

tiles_device_mode_t tiles_device_mode(void);
bool tiles_device_mode_is_standalone(void);

/* True if this boot is the restart into a mode just switched to: the boot
 * animation was already seen, so it is skipped. */
bool tiles_device_mode_quick_restart(void);

/* The boot animation: triangle pressed (switches the choice), and the
 * mode chosen so far, which its pulse shows. */
void tiles_device_mode_boot_toggle(void);
tiles_device_mode_t tiles_device_mode_boot_choice(void);

/* The device.mode setting. Its saved value applied at boot is the mode
 * already running and is ignored; a different value later saves the
 * settings and restarts into it. */
uint32_t tiles_device_mode_setting_get(void);
void tiles_device_mode_setting_set(uint32_t value);
/* After the settings are loaded: a switch chosen during the animation is
 * carried out, and from now on a change is a request. */
void tiles_device_mode_settings_loaded(void);

/* Main loop: carries out a requested change (after the reply has gone). */
void tiles_device_mode_service(uint32_t now_ms);
