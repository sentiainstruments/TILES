#pragma once

/*
 * Real, player-controlled minigames -- distinct from services/standby.c's
 * autonomous snake/brick-breaker animations, which stay exactly what
 * they were (ambient, self-playing idle art with no player). This
 * module is a genuinely separate feature: a deliberately-invoked
 * interactive mode with real button controls, not another idle-time
 * ambient loop. Kept as its own module rather than folded into
 * standby.c because the two serve different purposes and are likely to
 * evolve independently (control remapping, more games, difficulty
 * tuning) -- sharing state/logic between an AI-driven idle loop and a
 * player-driven game would couple two things that don't need to be
 * coupled.
 *
 * Entry/exit: hold SW3 (triangle) + SW4 (diamond) + SW5 (square) + SW6
 * (circle) together for ~0.7s to toggle game mode on/off -- SW1 ("-")
 * and SW2 ("+") are deliberately excluded from this combo since they're
 * reserved as in-game controls (see below). The same hold toggles game
 * mode off again from any state (menu or mid-game).
 *
 * Once on: the menu shows pad 1 (green) for Snake and pad 2 (orange)
 * for Brick Breaker -- touch either to launch it.
 *   Snake controls: SW1 left, SW2 right, SW3 up, SW4 down (absolute
 *   direction, not relative turning; reversing straight into the
 *   snake's own neck is ignored, the standard snake-game rule). Eats a
 *   food dot to grow; wraps around the grid's edges rather than dying
 *   on a wall hit (friendlier on a board this small); dies only on
 *   self-collision.
 *   Brick Breaker controls: SW1/SW2 move the 3-pad-wide paddle left/
 *   right. Otherwise the same physics as standby.c's autonomous
 *   version, just with the paddle player-controlled instead of AI-
 *   tracked.
 * Either game's end (snake self-collision, brick breaker won/lost)
 * flashes underglow red/purple for a couple of seconds, then returns to
 * the menu.
 *
 * Claims the same standby-active rendering path standby.c's own
 * animations and boot_sequence.c use
 * (tiles_lighting_set_standby_active(), tiles_buttons_set_standby_active(),
 * the RGB pad/underglow/button setters) -- correct and sufficient by
 * itself: buttons.c's per-button override for SW1/SW2 (octave_control.c)
 * already goes transparently inert under that same standby-active flag
 * (see buttons.c), so no changes were needed there for game mode to
 * freely drive SW1/SW2's LEDs too. main.c must skip calling
 * tiles_standby_scan() while tiles_game_mode_is_active() is true --
 * otherwise standby's own idle timer could fire mid-game and standby.c
 * would fight this module over the same rendering path. Both being
 * triggered by real button presses (the entry gesture, and exiting)
 * means standby's idle timer gets a fresh reset at the moment control
 * hands back, so there's no awkward "immediately idle right after
 * leaving a game" edge case from skipping its scan while active.
 */

#include <stdbool.h>

void tiles_game_mode_init(void);

/* Call every main-loop iteration, after tiles_buttons_scan() and
 * tiles_touch_scan() (needs fresh state from both -- buttons for the
 * entry/exit gesture and in-game controls, touch for menu selection). */
void tiles_game_mode_scan(void);

/* True whenever game mode owns the LED rendering path (menu, playing
 * either game, or the round-end flash) -- main.c uses this to skip
 * calling tiles_standby_scan() while true. */
bool tiles_game_mode_is_active(void);
