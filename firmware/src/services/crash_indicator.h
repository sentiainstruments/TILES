#pragma once

#include <stdbool.h>

/* After a crash reboot the underglow pulses red until circle is clicked on
 * its own (no other key down during the press, so lifting off a combo
 * can't dismiss it). lighting.c draws it. */

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
