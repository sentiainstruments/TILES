#pragma once

/* Player-controlled minigames. Separate from standby's self-playing game
 * demos (ambient art with no player); the two evolve independently.
 *
 * Entry/exit: hold triangle + diamond + square + circle for ~0.7 s ("-"
 * and "+" are left out: they are game controls). The same hold is the ONLY
 * way out, from the menu or mid-game; no single button exits. Losing a
 * round returns to the game menu.
 *
 * Menu: pad 1 (green) Snake, pad 2 (orange) Tile Breaker, pad 3 (cyan)
 * Tile Drop, pad 4 (blue) Paddle, pad 5 (white) Echo; touch one to start.
 *   Snake: "-" left, "+" right, triangle up, diamond down (absolute
 *     directions; reversing into the neck is ignored). Wraps at the edges,
 *     dies only on self-collision.
 *   Tile Breaker: "-"/"+" move the 3-pad paddle. Same physics as the
 *     standby demo, player-controlled.
 *   Tile Drop: "-"/"+" move, triangle rotates (2 orientations per piece),
 *     diamond hard-drops. A small custom piece set for a 4-row board: dot,
 *     domino, straight tromino, corner tromino, 2x2 square. Line clears
 *     flash the underglow white; topping out ends the round.
 *   Paddle (two players): left paddle column 1 ("-" up, "+" down), right
 *     paddle column 6 (square up, circle down). Paddles 2 pads, white; the
 *     ball is blue. First to 2 wins. A miss flashes the underglow white and
 *     re-serves. The score glows on each player's buttons (1 point: the
 *     "up" button; 2: both). A win freezes the board a couple of seconds,
 *     then returns to the menu.
 *   Echo: a growing pattern of pads flashes, each with a haptic
 *     and a color; repeat it by pressing (Hall depth, not touch). Each
 *     round adds one step; a wrong pad ends the game.
 * Round end: Snake and Tile Breaker flash the underglow red/purple,
 * Tile Drop and Echo red, then back to the menu.
 *
 * Rendering uses the standby hooks (tiles_lighting_set_standby_active(),
 * tiles_buttons_set_standby_active(), the RGB setters). Button READS still
 * reach other modules, so octave_control.c and expression_control.c check
 * tiles_game_mode_is_active() and ignore their buttons while a game runs.
 * main.c skips standby's scan while active; the entry/exit presses reset
 * standby's idle timer anyway. */

#include <stdbool.h>

void tiles_game_mode_init(void);

/* Call every main-loop pass, after tiles_buttons_scan() and
 * tiles_touch_scan(). */
void tiles_game_mode_scan(void);

/* True whenever game mode owns rendering (menu, a game, or a round-end
 * flash). main.c skips standby while true. */
bool tiles_game_mode_is_active(void);
