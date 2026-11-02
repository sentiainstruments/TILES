#pragma once

/* Power-on animation (~4.2 s, blocking): white rain from the keys down the
 * pads, a fade to dark, one slow magenta pulse. Ends with a second Hall
 * baseline capture. Frame waits pump tud_task() so USB enumerates. */

#include <stdbool.h>

/* Call after tiles_lighting_init(), tiles_buttons_init() and
 * tiles_hall_init(). Blocks ~4.2 s. False if the final baseline recapture
 * failed for any initialized pad (that pad keeps its init baseline); the
 * animation always completes. */
bool tiles_boot_sequence_run(void);
