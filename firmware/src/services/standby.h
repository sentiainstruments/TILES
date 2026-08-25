#pragma once

/*
 * Standby (idle) animations: after 1 minute with no touch/Hall/button/
 * pedal activity, the pad grid + function buttons + underglow stop
 * reflecting real input and instead run one of 7 rotating ambient
 * animations, switching to a new random one (never the current
 * animation or the one before it) every couple of minutes -- see
 * standby.c's s_animations[]. Any activity exits standby immediately and
 * hands rendering back to touch.c/buttons.c's normal behavior. Hall
 * depth is included alongside touch specifically because real hardware
 * showed MPR121 touch alone not reliably waking standby while the pad
 * animation is running -- see TILES_STANDBY_HALL_WAKE_DEPTH in
 * standby.c.
 *
 * After TILES_STANDBY_POWER_SAVING_TIMEOUT_MS (15 minutes) of TOTAL
 * inactivity, standby's animations stop and the board drops to a deeper
 * power-saving state: everything dark except the circle function button
 * pulsing gently to show how to wake it back up. Same wake conditions as
 * standby (any touch/button/pedal activity, or Hall depth once that's
 * re-enabled).
 *
 * Deliberately a lighting-only concept: touch/Hall/expression/MIDI keep
 * running completely unaware standby exists (see standby.c's header for
 * why) -- playing during "standby" still works exactly as it always
 * does, only the idle LED behavior changes.
 *
 * Buttons and pads are treated as one 5-row x 6-col logical grid (row 0
 * = the 6 function buttons, physically just above pad row 1; rows 1-4 =
 * the pad grid) for animation purposes -- see board/board_layout.h
 * (shared with services/boot_sequence.c) for the mapping from board
 * columns to button ids, and from board pad numbers to the 4 underglow
 * anchor points, both currently based on the user's verbal description
 * of the physical layout rather than a hardware doc, so flagged there as
 * easy to correct once seen lit on the real board.
 * Buttons are plain monochrome PCA9685 PWM, not addressable RGB like the
 * pads/underglow -- every animation collapses color to a single
 * brightness for the button row, additionally scaled down
 * (BUTTON_STANDBY_BRIGHTNESS_SCALE in standby.c) since buttons read
 * noticeably brighter than pads at the same commanded duty on real
 * hardware. Underglow normally mirrors whatever the pad grid's animation
 * shows at its 4 anchor points, but some animations (the graphic
 * equalizer, the circular underglow wave) need underglow to do something
 * genuinely different from any one pad -- see
 * s_animation_underglow_override[] in standby.c.
 */

#include <stdbool.h>

/* Seeds the idle clock from the current time and seeds this module's
 * pseudo-random source (used by the shooting-stars animation). Call
 * once, after tiles_lighting_init()/tiles_buttons_init() (both are
 * required by the standby render path) and after tiles_touch_init()/
 * tiles_pedal_init() (both are polled for activity detection). */
void tiles_standby_init(void);

/* Polls touch/button/pedal state for activity, drives the
 * awake/standby/power-saving state machine, and -- while in standby or
 * power-saving -- advances and renders the current frame at its own
 * internal frame rate. Call every main-loop iteration, after
 * tiles_buttons_scan()/tiles_touch_scan()/tiles_pedal_scan() so this
 * iteration's activity check sees fresh state. */
void tiles_standby_scan(void);

/* For diagnostics. True only for the animated standby state, not
 * power-saving -- see tiles_standby_is_power_saving() below for that. */
bool tiles_standby_is_active(void);

/* For diagnostics. True only for the deeper power-saving state (15
 * minutes of total inactivity past entering standby). */
bool tiles_standby_is_power_saving(void);
