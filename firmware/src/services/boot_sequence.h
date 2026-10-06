#pragma once

/* Power-on animation, run once at boot (~4.7 s):
 *   1. White "rain" floods down from the function buttons (row 0, the
 *      source) through pad rows 1-4. Underglow off.
 *   2. Buttons and pads fade to dark together.
 *   3. One slow Sentia magenta (#FF00FF) pulse on pads + underglow. The
 *      buttons stay dark: they are monochrome PWM and can't show magenta.
 * Everything is smoothstep-eased with wide soft edges (linear, narrow
 * edges looked jumpy).
 *
 * The time is used for a second Hall baseline capture at the end, after
 * power and temperature have settled a little
 * (tiles_hall_recapture_baseline()).
 *
 * Blocking by design, since nothing else needs to run yet. The frame wait
 * pumps tud_task() (boot_frame_delay()): nothing else services USB, and a
 * plain sleep would stall enumeration during boot.
 *
 * Uses the same standby rendering hooks as services/standby.c
 * (tiles_lighting_set_standby_active() etc.) and board/board_layout.h's
 * grid. */

#include <stdbool.h>

/* Call after tiles_lighting_init(), tiles_buttons_init() and
 * tiles_hall_init(). Blocks ~4.7 s. False if the final baseline recapture
 * failed for any initialized pad (that pad keeps its init baseline); the
 * animation always completes. */
bool tiles_boot_sequence_run(void);
