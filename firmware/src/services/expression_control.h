#pragma once

/* Square (SW5, "sentia"): pitch bend toggle and the expression menu, plus
 * the circle+square MPE toggle.
 *
 * ---- Square alone -----------------------------------------------------
 *   - Click: toggles pitch bend (tiles_expression_toggle_pitch_bend()).
 *   - While held alone (circle not held), from the first instant: the
 *     expression menu shows, and "-"/"+" step the haptics row one column
 *     at a time through the same apply_row() path as a pad tap, so the
 *     menu always shows what is applied.
 *   - Released before EXPRESSION_SUBMENU_TOGGLE_HOLD_MS (2 s): the menu
 *     hides again (a momentary peek).
 *   - Held 2 s: the menu locks open (sticky). A sticky menu closes on
 *     another 2 s hold, a click of square or circle, or a fresh press of
 *     SW1-SW4 while square isn't held (while it is, "-"/"+" still adjust
 *     haptics). When sticky, square's click closes the menu instead of
 *     toggling pitch bend.
 *   - Reaching 2 s (or forming the circle+square combo) cancels that
 *     press's click.
 *
 * Square's LED (claimed via the buttons.h override): lit while held; after
 * release, a slightly dimmer glow while pitch bend is on, dark when off; a
 * soft pulse whenever MPE is off, overriding the rest.
 *
 * ---- The expression menu ----------------------------------------------
 * While visible it claims the pad grid (tiles_lighting_set_standby_active())
 * and shows 4 rows of 6-pad sliders:
 *   row 1 (top)    haptics intensity (services/haptics.c)
 *   row 2          pitch bend sensitivity (services/expression.c)
 *   row 3          reserved for a future Y axis (stored, not used yet)
 *   row 4 (bottom) aftertouch sensitivity (services/expression.c)
 * Tapping a pad selects that column (1-6) and applies it at once
 * (piecewise_column_value() maps columns to values for every row). Column
 * 4 is each row's default, 5-6 stronger, 2-3 weaker. Row 1 column 1 is
 * haptics OFF and blinks when selected. The selected pad pulses white (the
 * menu standard); the others sit at a dim magenta.
 *
 * While visible, expression.c starts no NEW strikes (a tap only moves a
 * slider; notes already sounding finish normally), and octave_control.c
 * ignores "-"/"+" (see tiles_expression_control_owns_pad_grid()).
 *
 * ---- Circle + square held 2 s: MPE on/off -----------------------------
 * Toggles tiles_expression_set_mpe_enabled() (the `expression.mpe_enabled`
 * setting; MPE on is the default) after EXPRESSION_MPE_TOGGLE_HOLD_MS.
 * Square's LED pulses while MPE is off.
 *
 * ---- Game mode ------------------------------------------------------------
 * A game can use square/circle as controls, so while
 * tiles_game_mode_is_active() this scan only keeps edge tracking current.
 * In turn, game mode's 4-button entry refuses to fire while this menu owns
 * the grid, so the two never claim the board at once. */

#include <stdbool.h>

void tiles_expression_control_init(void);

/* Handles square's click/holds, the menu's momentary/sticky/dismiss rules
 * and the MPE combo; drives square's LED; draws the menu. Call every
 * main-loop pass after tiles_buttons_scan() and tiles_touch_scan(), and
 * before tiles_expression_scan() (which reads owns_pad_grid) and
 * tiles_lighting_service(). */
void tiles_expression_control_scan(void);

/* True while the menu owns the pad grid (momentary or sticky). main.c
 * skips standby, octave_control.c ignores "-"/"+", and expression.c starts
 * no new strikes. */
bool tiles_expression_control_owns_pad_grid(void);
