#pragma once

/*
 * Default function of SW1 ("-") and SW2 ("+"): octave shift down/up,
 * applied via services/note_map.c's tiles_note_map_set_octave_shift()
 * (note_map.c owns the actual shift value and applies it to every
 * note -- this module is just the button-driven controller for it).
 *
 * A press of "+" moves the shift up by one octave; "-" moves it down by
 * one, clamped to +/-TILES_NOTE_MAP_MAX_OCTAVE_SHIFT (3). Fires on
 * RELEASE, not the press itself (see tiles_octave_control_scan()'s own
 * comment) -- real feedback found that firing on press let the leading
 * button of the SW1+SW2 transpose combo below register a real step an
 * instant before the second button joined, since a human can never
 * press both in exactly the same tick; a release-gated "did this press
 * ever become part of the combo" check closes that race regardless of
 * which button happens to land first.
 *
 * The active-direction button's LED shows the current shift's magnitude
 * via a distinct pattern per level, all built from the same underlying
 * "pulse" shape (a smooth raised-cosine bump) so the three read as one
 * coherent family rather than unrelated effects (the other direction's
 * LED, and both at shift 0, stay dark):
 *   magnitude 1: that pulse repeating evenly forever, no rest
 *   magnitude 2: two of that pulse back to back, then a dim (not fully
 *                dark) rest, then repeat
 *   magnitude 3: the same shape as magnitude 2 with one more pulse
 *                appended before the rest -- three pulses, dim rest,
 *                repeat
 *
 * ---- Transpose mode ------------------------------------------------------
 * Holding SW1+SW2 together (a quick "click them together," not a long
 * hold like services/game_mode.h's 4-button combo) toggles transpose
 * mode -- the first concrete use of "-"/"+" as general-purpose
 * modifiers, per the product direction noted below. While active:
 *   - Both LEDs pulse together (magnitude 1's continuous pulse shape,
 *     same phase on both) instead of showing the octave pattern.
 *   - "-"/"+" step the key center (services/note_map.c's
 *     tiles_note_map_set_key_offset(), wraps 0-11/C-B) instead of the
 *     octave shift.
 *   - The pad grid (claimed via tiles_lighting_set_standby_active(),
 *     same rendering-ownership pattern services/standby.c and
 *     services/game_mode.h use) shows the current key's natural-note
 *     letter in caps, centered, via services/pixel_font.h. For a sharp
 *     key, the letter alternates with a plain "+"-shaped cross as a
 *     second flash, since there's no room to draw "#" into a 4x4 glyph
 *     -- a proper plus contained in its own 4x4 box (2-column vertical
 *     arm, full 4-row height; 1-row horizontal arm, 4 columns wide,
 *     centered), not a bar spanning the full 6-wide grid (real
 *     feedback: the horizontal arm was originally too long relative to
 *     the vertical one). The letter always shows first for a moment
 *     after entering the mode or changing key, so the flash never
 *     starts mid-cross. Underglow goes dark while this is showing.
 * Holding SW1+SW2 together again exits back to normal octave-shift
 * behavior. tiles_octave_control_is_transpose_active() lets main.c skip
 * services/standby.h's idle scan while this owns the pad grid, the same
 * way it already does for services/game_mode.h.
 *
 * ---- Deferring to game mode, manual screensaver scrolling, and the
 * expression sub-menu ----------------------------------------------------
 * services/game_mode.h's minigames (Snake, Brick Breaker, Tetris) also
 * use SW1/SW2 as their own left/right controls, services/standby.h's
 * manually-entered screensaver (hold SW6/circle for 6s) repurposes them
 * as animation-scroll controls, and services/expression_control.h's
 * expression sub-menu -- visible whenever SW5/square is held alone
 * (repurposing SW1/SW2 as a haptic-intensity shift) or left open sticky
 * (where a fresh SW1-SW4 press instead dismisses it) -- claims the whole
 * pad grid and needs SW1/SW2 left alone either way -- see
 * tiles_standby_owns_octave_buttons() and
 * tiles_expression_control_owns_pad_grid() respectively. This module's
 * scan runs unconditionally every main-loop tick with no gate of its
 * own, so without an explicit check here, every in-game, scroll,
 * intensity, or sub-menu-dismiss press would *also* silently step the
 * octave or transpose key underneath. tiles_game_mode_is_active(),
 * tiles_standby_owns_octave_buttons(), and
 * tiles_expression_control_owns_pad_grid() are all checked at the top of
 * tiles_octave_control_scan(): while any is true, this module only keeps
 * its press-edge tracking current (so a still-held button doesn't read
 * as a fresh press the instant control hands back) and does nothing else
 * -- button-LED writes are already a no-op during game mode too, since
 * game_mode.c also claims buttons.c's standby-active flag (see
 * tiles_buttons_set_override_led()'s doc comment in buttons.h); during
 * the manual screensaver, standby.c itself owns the same standby-active
 * flag for the same reason (the expression sub-menu doesn't touch
 * SW1/SW2's LEDs at all, so they simply keep their normal default "lit
 * while pressed" behavior throughout).
 *
 * Claims SW1/SW2 permanently via services/buttons.h's per-button
 * override mechanism (tiles_buttons_set_override_active()) -- their
 * LEDs stop following "lit while held" and are driven by the pattern
 * above instead; the other 4 buttons are untouched by this module.
 *
 * "-"/"+" are meant to become general-purpose modifier buttons for
 * other menus/combos down the line, per the product's own direction --
 * transpose mode above is the first real instance of that; octave shift
 * is still the V1 default function otherwise.
 */

#include <stdbool.h>

void tiles_octave_control_init(void);

/* Detects SW1/SW2 press edges and the transpose-mode combo, drives
 * their LED pattern, and renders the transpose key display when active.
 * Call every main-loop iteration, after tiles_buttons_scan() so this
 * iteration's debounced press state is fresh. */
void tiles_octave_control_scan(void);

/* True while transpose mode owns the pad grid -- see the file header.
 * Checked by main.c to skip services/standby.h's idle scan while this
 * is active, the same way it already does for services/game_mode.h. */
bool tiles_octave_control_is_transpose_active(void);
