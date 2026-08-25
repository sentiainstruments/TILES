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
 * Once on: the menu shows pad 1 (green) for Snake, pad 2 (orange) for
 * Brick Breaker, pad 3 (cyan) for Tetris, and pad 4 (blue) for Pong --
 * touch any to launch it.
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
 *   Tetris controls: SW1/SW2 move the falling piece left/right, SW3
 *   rotates it (a simplified 2-orientation rotation per piece, not full
 *   4-state SRS -- the board is only 4 rows tall so the extra states
 *   would rarely matter), SW4 hard-drops it. Standard tetromino set and
 *   colors, line clears shift everything above down (and flash
 *   underglow white -- see below); topping out (a freshly spawned piece
 *   has nowhere to go) ends the round.
 *   Pong controls, two players on one board: column 1 is the left
 *   paddle (SW1 up, SW2 down), column 6 is the right paddle (SW5 up,
 *   SW6 down -- the mirror-image pair to SW1/SW2; unverified against
 *   what the user actually meant by "circle and the other button next
 *   to it"). Both paddles 2 pads tall, white; the ball is a blue dot.
 *   Pong deliberately does NOT use the round-end flow below -- a rally
 *   on a board this small can end in seconds, so returning to the menu
 *   on every missed point would be disruptive. A miss instead flashes
 *   underglow white briefly and re-serves immediately, staying in play
 *   until the player deliberately exits via the entry/exit combo above.
 * Every other game's end (snake self-collision, brick breaker
 * won/lost) flashes underglow red/purple for a couple of seconds, then
 * returns to the menu; Tetris topping out flashes plain red instead
 * (real feedback: "when game is lost it should flash red," distinct
 * from its own white line-clear flash above).
 *
 * Claims the same standby-active rendering path standby.c's own
 * animations and boot_sequence.c use
 * (tiles_lighting_set_standby_active(), tiles_buttons_set_standby_active(),
 * the RGB pad/underglow/button setters) -- correct and sufficient for
 * *LED writes*: buttons.c's per-button override for SW1/SW2
 * (octave_control.c) already goes transparently inert under that same
 * standby-active flag (see buttons.c), so no changes were needed there
 * for game mode to freely drive SW1/SW2's LEDs too.
 * That inertness only covers LED *writes*, though -- octave_control.c's
 * button *reads* (SW1/SW2 rising edges -> octave/key-offset steps) run
 * unconditionally every scan regardless of standby-active, so without an
 * explicit check there, every left/right press in Snake/Brick
 * Breaker/Tetris would *also* silently step the octave or transpose key
 * underneath the game. octave_control.c now checks
 * tiles_game_mode_is_active() itself and skips all of its own action
 * logic (while keeping its press-edge tracking current) whenever a game
 * owns the buttons -- see its file header.
 * main.c must skip calling tiles_standby_scan() while
 * tiles_game_mode_is_active() is true -- otherwise standby's own idle
 * timer could fire mid-game and standby.c would fight this module over
 * the same rendering path. Both being triggered by real button presses
 * (the entry gesture, and exiting) means standby's idle timer gets a
 * fresh reset at the moment control hands back, so there's no awkward
 * "immediately idle right after leaving a game" edge case from skipping
 * its scan while active.
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
