#pragma once

/* The physics modes on the hardware: Ripples, Sand, Orbits and Newton's
 * cradle (their physics: services/physics_core.h). services/op_mode.c owns
 * the mode switch, the shared buttons and the clock. The modes play the
 * pads' own notes (the scale, lowest bottom-left), Sand the scale counted
 * up from the key note; they share one set of moves:
 *
 *   pads          make something: a strike's velocity is how big, the press
 *                 how much (a stone's size, Sand's pour). What you place
 *                 stays lit, and loops like a sequencer: a Ripples stone,
 *                 a planet, a swing. Strike it again to take it away (a
 *                 swing: touch one of its balls).
 *   circle + tap  removes that one thing too (a stone, a column of sand, a
 *                 planet, a swing). While circle is held the scene dims and
 *                 what a tap would remove glows red.
 *   circle + hold a pad 1 s: clears everything.
 *   circle        tap tempo, as in the sequencer.
 *   square        click: the mode's other physics (Splash, Flip, Pull,
 *                 Pendulum wave; its LED lit while on). Held: the Feel menu.
 *   "-" / "+"     octave down / up (Ripples, Sand, the cradle), as in
 *                 melodic mode; Orbits: stop / play.
 *
 * Each mode shows something at rest: the pond's surface, the floor the
 * sand lands on and the playhead crossing it, the sun, the six balls.
 *
 * Time is the shared tempo (tap tempo or a DAW's clock), and while the
 * clock runs the modes keep its beat grid: their own beat count is pulled
 * onto the clock's phase. They keep moving in the background when another
 * mode is up, like Bouncing notes; only the shown one is felt, and a
 * finger held in one is let go when it leaves. Notes on MIDI channel 11,
 * with Bouncing notes (the same note from both at once can cut short). */

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
