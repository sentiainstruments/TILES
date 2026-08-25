#pragma once

/*
 * Power-on animation, run once at boot: a white ripple rises from the
 * bottom-center of the grid (underglow stays off), fills the board,
 * fades to complete dark, then a single "Sentia Instruments Magenta"
 * (#FF00FF) pulse across pads + underglow finishes it -- then normal
 * operation begins.
 *
 * Also uses the ~2 seconds this takes productively: hall.c's rest
 * baseline is captured once at tiles_hall_init(), right at the very
 * first instant of boot -- before power/thermals have had any time to
 * settle. Re-capturing it again right as this sequence ends
 * (tiles_hall_recapture_baseline()) gives a baseline taken a couple of
 * settled seconds later instead, at essentially no extra cost since the
 * animation was going to take that long anyway.
 *
 * Blocking by design: nothing else needs to run while this plays (no
 * touch/MIDI/etc. matters yet, and TinyUSB's own background IRQ task
 * keeps USB alive regardless of what the main loop is doing -- see
 * midi/usb_device.c), so a tight sleep_ms()-paced loop is simpler than
 * threading this through the main loop's per-iteration scan functions
 * the way standby.c's animations have to be.
 *
 * Reuses the exact same standby-active rendering path standby.c's
 * animations use (tiles_lighting_set_standby_active(),
 * tiles_buttons_set_standby_active(), and the RGB pad/underglow/button
 * setters) rather than a separate one -- this is conceptually the same
 * "something other than touch owns the LEDs for a while" situation, so
 * there was no reason to build a second mechanism for it. Shares
 * board/board_layout.h's grid model with standby.c for the same reason.
 */

#include <stdbool.h>

/* Must run after tiles_lighting_init(), tiles_buttons_init(), and
 * tiles_hall_init() (needs a baseline to already exist before it can
 * usefully re-capture one). Blocks for ~2 seconds. Returns false if the
 * final baseline re-capture failed for any initialized pad (that pad's
 * baseline is left at whatever tiles_hall_init() originally captured,
 * not zeroed) -- the animation itself always completes regardless. */
bool tiles_boot_sequence_run(void);
