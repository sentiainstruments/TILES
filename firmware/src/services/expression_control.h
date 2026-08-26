#pragma once

/*
 * Square (SW5, "sentia")'s function-button role, plus a circle+square
 * combo for expression mute. Real feedback, in order: "when you press
 * sentia button once it turns on and off the pitch bend... when you
 * hold and press - or + you can adjust intensity of haptics on device,"
 * then, once corrected to the right button and given a fuller design:
 * "it should be when you hold shift and sentia the pads become sliders
 * one for each of the four rows... row one is haptics, row two is pitch
 * bend, 3 is the remaining axis and 4 is aftertouch... make the selected
 * level sentia magenta... hold shift and sentia together for 3 seconds
 * [for expression mute]... holding just sentia acts like a function
 * shift for modifiers -/+ for haptics," correcting which physical button
 * is which: "our shift and power button is circle. sentia is square
 * button," and finally, after a first real-hardware pass: "the lowest
 * setting is off and should be blinking when active in menu to show
 * its off. also theres no continuity between menu and arrow keys
 * control for haptics... lets change [the combo that] opens the
 * expression sub-menu... to hold square for 3 seconds alone to toggle
 * that menu."
 *
 * ---- Square alone: pitch-bend toggle, a haptic-intensity shift, and a
 * sub-menu toggle ---------------------------------------------------------
 * A short click (press+release before either hold gesture below fires)
 * toggles services/expression.c's pitch bend on/off via
 * tiles_expression_toggle_pitch_bend(). While square is held alone
 * (circle NOT also held), SW1 ("-")/SW2 ("+") step the sub-menu's row 1
 * (haptics) one COLUMN at a time -- not services/haptics.c's intensity
 * scalar directly -- via the exact same apply_row()/apply_row_haptics()
 * path a pad tap in the sub-menu uses (see below), so "-"/"+" and the
 * sub-menu can never drift out of sync: real feedback caught this
 * directly ("theres no continuity between menu and arrow keys control
 * for haptics... any changes that affect those 4 parameters should
 * always be reflected on the menu"). tiles_expression_control_shift_
 * active() is checked by octave_control.c (the same deferral pattern it
 * already uses for tiles_game_mode_is_active()/tiles_standby_owns_
 * octave_buttons()) so an intensity press doesn't *also* silently step
 * the octave/transpose key underneath.
 * Held alone for EXPRESSION_SUBMENU_TOGGLE_HOLD_MS (3000ms, edge-latched
 * so a single long hold can't re-fire, mirroring services/standby.h's
 * own circle-hold *_fired pattern; the alone-streak resets if circle
 * ever joins mid-hold, requiring a fresh uninterrupted 3s) toggles the
 * expression sub-menu open/closed -- a sticky mode, not a momentary
 * show-while-held one, so it stays open after square is released and
 * closes only on the next such 3-second hold. Reaching this threshold
 * (or forming the circle+square combo below) suppresses the short-click
 * pitch-bend toggle on that same press's eventual release, the same way
 * services/standby.c's old circle version suppressed its own short
 * click once a long-press gesture fired.
 *
 * Square's own LED (claimed permanently via services/buttons.h's
 * per-button override, same mechanism octave_control.c uses for SW1/SW2)
 * reflects all of this: solid at normal press-feedback brightness while
 * physically held (alone or as part of the combo below); once released,
 * a persistent glow -- deliberately dimmer than press feedback but "not
 * by a lot" -- while pitch bend is on, dark while off; overridden
 * entirely by the mute blink pattern (see below) whenever mute is
 * active, regardless of hold state.
 *
 * ---- The expression sub-menu --------------------------------------------
 * Once open (toggled by the square-alone 3-second hold above), the pad
 * grid is claimed -- via services/lighting.h's
 * tiles_lighting_set_standby_active(), the same rendering-ownership
 * pattern octave_control.c's transpose mode and services/standby.c's own
 * animations use -- and turned into 4 rows of 6-pad sliders, one row per
 * expression parameter:
 *   row 1 (nearest the buttons) -- services/haptics.c's global intensity
 *   row 2 -- services/expression.c's pitch bend sensitivity
 *   row 3 -- reserved for a future "remaining axis" (Y) feature; the
 *            selected column is stored but not yet consumed anywhere
 *   row 4 (bottom) -- services/expression.c's aftertouch sensitivity
 * Tapping (capacitive touch rising edge) any pad in a row selects that
 * column (1-6, left to right) as the row's new level, applied
 * immediately via each parameter's own setter -- see
 * expression_control.c's piecewise_column_value() for the shared
 * column-to-value mapping every row uses. Column 4 is every row's
 * default/"sweet spot" (matching each parameter's previous fixed
 * default exactly, so a fresh boot behaves identically to before this
 * feature existed); columns 5-6 go stronger/more sensitive, columns 2-3
 * weaker/less sensitive. Column 1 on row 1 (haptics) specifically is a
 * true OFF (0.0 intensity, not just "weak") -- real feedback: "the
 * lowest setting is off" -- and blinks rather than showing solid
 * magenta while selected, so the sub-menu itself communicates "this
 * parameter is off," not just "this is the lowest setting." Every other
 * selected pad lights solid Sentia Magenta (#FF00FF) -- every unselected
 * pad in the grid stays dark -- per real feedback: "make the selected
 * level of everything sentia magenta color to differenciate from
 * standard modes."
 * Because "-"/"+" (above) and every pad tap both funnel through the same
 * apply_row() setter, the sub-menu's stored column for every row is
 * always exactly what's actually applied -- there is no separate path
 * that could silently disagree with what the grid displays.
 *
 * While the sub-menu is open, services/expression.c's own per-pad state
 * machine is prevented from starting any NEW strike (a tap here must
 * only move a slider, never also fire a MIDI note) but a pad already
 * mid-strike or held when it opens is left alone to finish normally --
 * see tiles_expression_control_owns_pad_grid() and its call site in
 * expression.c's tiles_expression_scan().
 *
 * ---- Circle+square held for 3 seconds: expression mute -----------------
 * A separate combo, independent of the sub-menu toggle above: holding
 * SW6 (circle) and SW5 (square) together for EXPRESSION_MUTE_HOLD_MS
 * (3000ms, its own edge latch) toggles a sticky "expression mute" that
 * persists until the same 3-second combo hold toggles it off again --
 * real feedback: "a shortcut that disables everything and leaves basic
 * midi... it acts like a mute." Muted: pitch bend and poly aftertouch
 * stop being computed/sent (tiles_expression_set_muted()) and every
 * haptic effect (touch pulse, kick, sustain) stops firing, with every
 * currently-active motor cut immediately (tiles_haptics_set_muted()) --
 * note-on/off and velocity are completely unaffected, so basic MIDI
 * keeps working exactly as before. The sub-menu itself keeps working
 * while muted (still useful for queuing up settings before unmuting);
 * only square's own alone-hold behaviors (pitch-bend click, sub-menu
 * toggle, intensity shift) are suppressed while muted, since square's
 * LED is busy showing the mute indicator instead of its normal
 * toggle-state glow: a blinking two-pulse pattern followed by a rest at
 * medium brightness, repeating -- real feedback: "sentia should become
 * a blinking light with a two blink pattern and rest at medium
 * brightness to indicate expression functions mute."
 *
 * ---- Deferring to game mode -------------------------------------------
 * services/game_mode.h's Pong minigame uses SW5 (square)/SW6 (circle) as
 * its own live right-paddle up/down controls -- see game_mode.c's own
 * header. This module's entire scan bails immediately (keeping only its
 * own press-edge tracking current) whenever tiles_game_mode_is_active()
 * is true, so a paddle press during play never also toggles pitch bend,
 * mutes, or opens the sub-menu underneath; symmetrically, game_mode.c's
 * own 4-button (SW3+SW4+SW5+SW6) entry combo refuses to fire while this
 * module's sub-menu already owns the pad grid (see its gm_combo_held()),
 * so the two features can never both claim the board at once.
 */

#include <stdbool.h>

void tiles_expression_control_init(void);

/* Detects square's press/hold/click, the sub-menu-toggle hold, and the
 * circle+square mute combo; drives square's LED; and renders the
 * sub-menu when open. Call every main-loop iteration, after
 * tiles_buttons_scan() (fresh button state) and tiles_touch_scan()
 * (fresh touch state for the sub-menu's slider taps), before
 * tiles_expression_scan() (so this tick's fresh "does the sub-menu own
 * the grid" state gates strike suppression correctly) and before
 * tiles_lighting_service() (so any sub-menu pad writes this tick land
 * before the frame composites). See the file header for the full
 * reasoning. */
void tiles_expression_control_scan(void);

/* True while the expression sub-menu (toggled by square's own 3-second
 * alone-hold) owns the pad grid. Checked by main.c to skip
 * services/standby.h's idle scan while this is active, the same way it
 * already does for services/game_mode.h and octave_control.c's
 * transpose mode, and by services/expression.c to suppress starting new
 * strikes while a tap here should only move a slider. */
bool tiles_expression_control_owns_pad_grid(void);

/* True only while square (SW5) is currently held ALONE (circle NOT also
 * held) -- see the file header's "Square alone" section. While true,
 * SW1/SW2 are repurposed here as haptic-intensity controls instead of
 * their normal octave_control.c function, and octave_control.c must skip
 * its own SW1/SW2 handling entirely (same deferral pattern as
 * tiles_standby_owns_octave_buttons()) so an intensity-adjustment press
 * doesn't *also* silently step the octave/transpose key underneath. */
bool tiles_expression_control_shift_active(void);
