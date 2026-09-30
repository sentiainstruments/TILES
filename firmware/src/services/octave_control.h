#pragma once

/* Default function of SW1 ("-") and SW2 ("+"): octave shift down/up, in
 * +/-TILES_NOTE_MAP_MAX_OCTAVE_SHIFT (3) steps. services/note_map.c owns
 * the value; this is the button controller. Steps fire on RELEASE, so the
 * first button of the "-"+"+" combo (humans never press both in the same
 * tick) doesn't also step the octave.
 *
 * The active direction's LED shows the magnitude, all built from one
 * smooth pulse shape (the other LED, and both at 0, stay dark):
 *   1: the pulse repeating evenly
 *   2: two pulses, a dim rest, repeat
 *   3: three pulses, a dim rest, repeat
 *
 * ---- Transpose mode ----------------------------------------------------
 * Clicking "-"+"+" together toggles transpose mode:
 *   - Both LEDs pulse together.
 *   - "-"/"+" step the key (tiles_note_map_set_key_offset(), wraps C..B).
 *   - The pad grid shows the key's letter (services/pixel_font.h); a sharp
 *     key alternates the letter with a 4x4 "+" (no room for "#"). The
 *     letter always shows first after a change. Underglow is dark.
 * The grid is claimed through tiles_lighting_set_standby_active(), and
 * main.c skips standby while this is active.
 *
 * ---- Yielding the buttons ----------------------------------------------
 * Game mode, the manual screensaver (which uses "-"/"+" to scroll),
 * the expression menu (services/expression_control.h) and some op_mode
 * views use SW1/SW2 themselves. While any of them owns the buttons
 * (checked at the top of tiles_octave_control_scan()), this module only
 * keeps its edge tracking current, so a held button doesn't read as a new
 * press when control returns.
 *
 * SW1/SW2's LEDs are claimed permanently through the buttons.h override
 * mechanism; the other four buttons are untouched. */

#include <stdbool.h>

void tiles_octave_control_init(void);

/* Handles SW1/SW2 edges and the transpose combo, drives their LEDs, and
 * draws the transpose display. Call every main-loop pass, after
 * tiles_buttons_scan(). */
void tiles_octave_control_scan(void);

/* True while transpose mode owns the pad grid (main.c skips standby). */
bool tiles_octave_control_is_transpose_active(void);
