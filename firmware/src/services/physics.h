#pragma once

/* The physics modes on hardware (simulations: physics_core.h), channel 11.
 * Shared moves: a strike makes something (velocity = how big) that loops
 * and stays lit; strike it again, or circle + tap, to remove it; circle +
 * hold 1 s clears all. Circle: tap tempo. Square: the mode's other physics
 * (Splash, Flip, Pull, Pendulum wave). "-"/"+": octave (Orbits: stop/play).
 * They follow the shared tempo and keep moving in other modes. */

#include "midi_clock.h"
#include "physics_core.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_PHYS_RIPPLES = 0,
    TILES_PHYS_SAND,
    TILES_PHYS_ORBITS,
    TILES_PHYS_CRADLE,
    TILES_PHYS_KIND_COUNT
} tiles_phys_kind_t;

#define TILES_PHYS_NONE (-1)

void tiles_physics_init(void);
/* Entering a mode: a finger already down isn't a strike. */
void tiles_physics_enter(tiles_phys_kind_t kind);

/* Every scan and playback tick: moves every mode on, plays what happens.
 * `shown`: the kind on the pads (felt, and its fingers live), or
 * TILES_PHYS_NONE. */
void tiles_physics_advance(tiles_midi_clock_state_t clock, int shown);

/* Every scan while the mode is shown. */
void tiles_physics_handle_input(tiles_phys_kind_t kind, uint32_t now_ms);
void tiles_physics_render(tiles_phys_kind_t kind, uint32_t now_ms, float beat_flash, float diamond_led);

/* Square, clicked (op_mode.c passes it on). */
void tiles_physics_toggle_square(tiles_phys_kind_t kind);
bool tiles_physics_square_on(tiles_phys_kind_t kind);

/* Something moving or sounding (the mode menu's background blink). */
bool tiles_physics_is_playing(tiles_phys_kind_t kind);

/* Orbits' transport, for op_mode.c's "-"/"+". */
void tiles_physics_orbits_play(void);
void tiles_physics_orbits_stop(void);
bool tiles_physics_orbits_is_running(void);

/* Ends every sounding note (a panic, a game taking the channel). */
void tiles_physics_end_all_notes(void);
