#pragma once

/* "-" / "+": octave down / up (+/-3), stepping on release so the "-"+"+"
 * combo doesn't also step. The active key's LED pulses 1, 2 or 3 times.
 * Chord mode: circle + "-"/"+" moves the strip's octave.
 * "-" + "+" together: transpose mode, where "-"/"+" step the key and the
 * pads show its letter. Yields the keys to games, the manual screensaver,
 * the Feel menu and some op_mode views. */

#include <stdbool.h>
#include <stdint.h>

void tiles_octave_control_init(void);

/* Handles SW1/SW2 edges and the transpose combo, drives their LEDs, and
 * draws the transpose display. Call every main-loop pass, after
 * tiles_buttons_scan(). */
void tiles_octave_control_scan(void);

/* True while transpose mode owns the pad grid (main.c skips standby). */
bool tiles_octave_control_is_transpose_active(void);

/* What "-"/"+"'s LEDs show now (the octave, or transpose's pulse), for a
 * mode that draws every LED itself and keeps "-"/"+" as octave (the
 * physics modes, services/physics.h). */
void tiles_octave_control_led_levels(uint32_t now_ms, float *minus_level, float *plus_level);
