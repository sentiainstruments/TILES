#pragma once

/* Square (SW5): pitch bend toggle, the Feel menu, and MPE on/off.
 *   - Click: toggle pitch bend (while the menu is locked: close it).
 *   - Hold: the Feel menu shows; "-"/"+" step haptics. Released before 2 s
 *     it hides; held 2 s it locks open until another 2 s hold, a click, or
 *     a fresh key press.
 *   - Circle + square held 2 s: MPE on/off (square's LED pulses while off).
 * Feel menu rows, top to bottom: haptics, bend sensitivity, pressure
 * sensitivity, LED intensity; a tap selects a column (4 = default, 6 for
 * LEDs; column 1 of rows 1 and 4 = off). While it shows, no new strikes
 * start and "-"/"+" aren't octave. From the Feel menu a short circle press
 * opens Light effects (with square) or Layouts (with triangle): a resting
 * finger previews, a press picks. Idle during a game. */

#include <stdbool.h>

void tiles_expression_control_init(void);

/* Square's gestures, the menu and its LED. Every pass after the button and
 * touch scans, before tiles_expression_scan() and lighting. */
void tiles_expression_control_scan(void);

/* True while the menu owns the pad grid (momentary or sticky). main.c
 * skips standby, octave_control.c ignores "-"/"+", and expression.c starts
 * no new strikes. */
bool tiles_expression_control_owns_pad_grid(void);
