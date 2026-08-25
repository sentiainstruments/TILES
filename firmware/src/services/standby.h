#pragma once

/*
 * Standby (idle) animations: after 1 minute with no touch/button/pedal
 * activity, the pad grid + function buttons + underglow stop reflecting
 * real input and instead run one of a rotating set of ambient
 * animations, cycling to the next one every couple of minutes. Any
 * activity exits standby immediately and hands rendering back to
 * touch.c/buttons.c's normal behavior.
 *
 * Deliberately a lighting-only concept: touch/Hall/expression/MIDI keep
 * running completely unaware standby exists (see standby.c's header for
 * why) -- playing during "standby" still works exactly as it always
 * does, only the idle LED behavior changes.
 *
 * Buttons and pads are treated as one 5-row x 6-col logical grid (row 0
 * = the 6 function buttons, physically just above pad row 1; rows 1-4 =
 * the pad grid) for animation purposes -- see standby.c for the mapping
 * from board columns to button ids, and from board pad numbers to the 4
 * underglow anchor points, both currently based on the user's verbal
 * description of the physical layout rather than a hardware doc, so
 * flagged there as easy to correct once seen lit on the real board.
 */

#include <stdbool.h>

/* Seeds the idle clock from the current time and seeds this module's
 * pseudo-random source (used by the shooting-stars animation). Call
 * once, after tiles_lighting_init()/tiles_buttons_init() (both are
 * required by the standby render path) and after tiles_touch_init()/
 * tiles_pedal_init() (both are polled for activity detection). */
void tiles_standby_init(void);

/* Polls touch/button/pedal state for activity, drives the
 * standby/awake state machine, and -- while in standby -- advances and
 * renders the current animation at its own internal frame rate. Call
 * every main-loop iteration, after tiles_buttons_scan()/
 * tiles_touch_scan()/tiles_pedal_scan() so this iteration's activity
 * check sees fresh state. */
void tiles_standby_scan(void);

/* For diagnostics. */
bool tiles_standby_is_active(void);
