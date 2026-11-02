#pragma once

/* Player minigames (separate from standby's self-playing demos). Hold
 * triangle + diamond + square + circle ~0.7 s to enter or leave (the only
 * way out). Menu pads 1-5: Snake, Tile Breaker, Tile Drop, Paddle (two
 * players), Echo; controls in game_mode.c. Drawn through the standby hooks;
 * octave and Feel controls ignore their keys during a game. */

#include <stdbool.h>

void tiles_game_mode_init(void);

/* Call every main-loop pass, after tiles_buttons_scan() and
 * tiles_touch_scan(). */
void tiles_game_mode_scan(void);

/* True whenever game mode owns rendering (menu, a game, or a round-end
 * flash). main.c skips standby while true. */
bool tiles_game_mode_is_active(void);
