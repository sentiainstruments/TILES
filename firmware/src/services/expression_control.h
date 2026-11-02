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
 *   row 3          pressure (aftertouch) sensitivity (services/expression.c)
 *   row 4 (bottom) LED intensity: pads, underglow and button LEDs
 *                  (services/lighting.c look.led_intensity_percent)
 * Tapping a pad selects that column (1-6) and applies it at once. Rows 1-3:
 * column 4 is the default, 5-6 stronger, 2-3 weaker
 * (piecewise_column_value()). Row 4: column 6 (the default) is the full
 * brightness the power mode allows, column 1 is off. Column 1 of rows 1
 * and 4 is a real OFF and blinks when selected. The selected pad pulses
 * white (the menu standard); the others sit at a dim magenta. The menu
 * itself always shows at full brightness, so LEDs set to off can be turned
 * back on.
 *
 * While visible, expression.c starts no NEW strikes (a tap only moves a
 * slider; notes already sounding finish normally), and octave_control.c
 * ignores "-"/"+" (see tiles_expression_control_owns_pad_grid()).
 *
 * ---- Deeper menus, from the Feel menu ------------------------------------
 * With the Feel menu up (square held, or locked open), a short circle press
 * (under 0.8 s) opens one in its place:
 *   circle + square   (square held, tap circle; or locked open, circle and
 *                     square together or one after the other): Light
 *                     effects. Pad 1 off (default), 2-6 the white effects,
 *                     7-12 the colour ones (light_effects.h), 13-24 empty.
 *   circle + triangle Layouts: pads 1-7 Rows (default), Columns,
 *                     Fourths, Keys, Fifths, Tonnetz, Quarter tones
 *                     (note_map.h), the rest empty.
 * Each menu has its own colour: the Feel menu magenta, Light effects amber
 * (underglow and the Off pad), Layouts cyan (pads and underglow).
 * The current pick pulses white. A finger resting on a pad (no press)
 * previews it on the whole grid after ~0.3 s: Light effects plays the
 * touched pad as if pressed, over and over, soft to hard and back; Layouts
 * shows the layout's resting colours.
 * A press picks it and closes; lifting goes back, nothing changed; any
 * button closes without picking. Settings look.play_effect and
 * layout.melodic.
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
