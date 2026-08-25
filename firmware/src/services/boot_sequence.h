#pragma once

/*
 * Power-on animation, run once at boot: a white "rain" floods down
 * through the grid, starting at the function buttons (row 0 -- they light
 * first, as the flood's source) and flowing down through the pads to row
 * 4 (underglow stays off), then both buttons and pads fade together to
 * complete dark, then a single, slow, smoothstep-eased "Sentia
 * Instruments Magenta" (#FF00FF) pulse across pads + underglow finishes
 * it -- then normal operation begins. Function buttons are explicitly
 * blacked out right before phase 3 and never touched again for the rest
 * of the sequence, since they're plain monochrome PWM, not addressable
 * RGB, and can't show magenta at all. (An earlier version held them dark
 * for the entire sequence including the rain/fade, which read as wrong
 * once seen on real hardware -- they're logically the flood's source, so
 * leaving them unlit through the rain looked incomplete. A version before
 * that had them lit during the magenta phase too, which is the part
 * that's still wrong and stays excluded.) The rain's downward direction
 * (from real feedback -- an earlier version rose from the bottom-center
 * outward instead) starts at the buttons' position and lights them along
 * with everything else.
 *
 * Pacing is also the result of real feedback: the earlier version used
 * linear, fairly quick, narrow-edged transitions that read as "jumpy" --
 * now smoothstep-eased throughout, with wider soft edges and longer
 * durations.
 *
 * Also uses the ~4 seconds this takes productively: hall.c's rest
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
 * usefully re-capture one). Blocks for ~4 seconds. Returns false if the
 * final baseline re-capture failed for any initialized pad (that pad's
 * baseline is left at whatever tiles_hall_init() originally captured,
 * not zeroed) -- the animation itself always completes regardless. */
bool tiles_boot_sequence_run(void);
