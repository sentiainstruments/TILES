#pragma once

#include <stdbool.h>

/* Crash indicator: after a crash-recovery reboot (which skips the boot
 * animation so recovery is fast), the underglow pulses red until the
 * player acknowledges it with a single click of circle (shift) on its own.
 *
 * "On its own" means no other function button was down at any point
 * during the press, so lifting off a multi-button combo that includes
 * circle (debug mode, the MPE toggle) circle-last can't dismiss it.
 *
 * Rendering is in services/lighting.c, which overrides the underglow
 * while tiles_crash_indicator_is_active(); this module only owns the
 * state and the gesture. */

/* Call once at boot with whether this boot is a crash recovery (main.c,
 * watchdog_enable_caused_reboot()), before the first scan. Inert when
 * false. */
void tiles_crash_indicator_init(bool crash_recovered);

/* Call every main-loop pass, after tiles_buttons_scan() and before
 * tiles_lighting_service(), so a dismiss shows in the same frame. No-op
 * once inactive. */
void tiles_crash_indicator_scan(void);

/* True until the crash is acknowledged. Read by services/lighting.c. */
bool tiles_crash_indicator_is_active(void);
