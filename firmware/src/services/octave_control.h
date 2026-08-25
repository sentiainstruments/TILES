#pragma once

/*
 * Default function of SW1 ("-") and SW2 ("+"): octave shift down/up,
 * applied via services/note_map.c's tiles_note_map_set_octave_shift()
 * (note_map.c owns the actual shift value and applies it to every
 * note -- this module is just the button-driven controller for it).
 *
 * A press (rising edge, not "while held") of "+" moves the shift up by
 * one octave; "-" moves it down by one, clamped to
 * +/-TILES_NOTE_MAP_MAX_OCTAVE_SHIFT (3).
 *
 * The active-direction button's LED shows the current shift's magnitude
 * via a distinct pattern per level (the other direction's LED, and both
 * at shift 0, stay dark):
 *   magnitude 1: solid on
 *   magnitude 2: slow breathing pulse
 *   magnitude 3: three quick blinks, then a solid hold, then repeat
 *
 * Claims SW1/SW2 permanently via services/buttons.h's per-button
 * override mechanism (tiles_buttons_set_override_active()) -- their
 * LEDs stop following "lit while held" and are driven by the pattern
 * above instead; the other 4 buttons are untouched by this module.
 *
 * "-"/"+" are meant to become general-purpose modifier buttons later
 * (held as a modifier for other menus/combos, per the product's own
 * direction) -- this module only implements their V1 default function,
 * octave shift; it isn't a generic modifier framework and shouldn't be
 * stretched into one until something else actually needs it.
 */

void tiles_octave_control_init(void);

/* Detects SW1/SW2 press edges and drives their LED pattern. Call every
 * main-loop iteration, after tiles_buttons_scan() so this iteration's
 * debounced press state is fresh. */
void tiles_octave_control_scan(void);
