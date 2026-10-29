#pragma once

/* Bouncing notes mode: every column is a lane where a ball bounces; it plays
 * the lane's note each time it lands on the bottom pad. The physics is
 * services/bounce_core.h; this file is the hardware side. services/op_mode.c
 * owns the mode switch, the shared buttons and the clock.
 *
 *   pads          tap: the column's ball drops from that pad's height
 *                 (bottom = 1 .. top = 4) and keeps bouncing; the same
 *                 height again stops it (off). Higher drops land less often
 *                 and louder: heights 1-4 land every 1, 2, 4, 6 eighths.
 *                 Taps act on the lift.
 *   hold a pad    picks the column's pitch, as a held sequencer step does:
 *                 the pads turn into the melodic surface with the lane's
 *                 note pulsing white; touch any pad for its note (circle +
 *                 triangle backs out). The balls keep bouncing meanwhile.
 *   circle + "-"/"+"  scroll one column: 12 lanes, 6 shown. All pads flash
 *                 pink at either end.
 *   "-" / "+"     stop / start as in the sequencer ("-" twice drops every
 *                 ball back to its top; "+" with no clock coming in starts
 *                 at once and restarts the clock; following a DAW's clock,
 *                 every ball lands on its next beat, so they bounce on its
 *                 grid).
 *   circle        tap tempo.
 *   square        click: Real gravity on/off (its LED lit while on); held:
 *                 the Feel menu, as in every mode.
 *
 * Real gravity, the expressive version: a tap throws the column's ball as
 * it lifts (the pad's row sets the base height, how hard you hit adds up
 * to two pads, off the top of the grid and back; a hold only picks the
 * pitch), and every landing keeps 70% of
 * the height, so it bounces lower, faster and softer until it rests: the
 * accelerating rhythm of a dropped ball, in free time scaled by the tempo
 * (bounce_core.h). Each tap throws again.
 * "-" catches every ball, "+" throws every lane again as it last was.
 * Switching either way keeps every ball where it is and the notes sounding:
 * gravity takes the plain balls from their height and point in the bounce,
 * and back, each ball takes the plain height nearest its apex.
 *
 * Lane pitches: the scale (circle + triangle) from the key note up, lane 1 the
 * key note itself, at the octave set elsewhere; chromatic = 12 semitones. A
 * picked pitch stays put (key and octave changes don't move it) but is
 * fitted to the scale, like a sequencer step's.
 * Notes on MIDI channel 11 (shared with the games, which never run at the
 * same time). The balls keep bouncing when you switch modes, like the
 * sequencer; "-" stops them.
 *
 * Lights: a lane's drop height dim, its ball bright and gliding between
 * pads on a parabola (timed by the clock between steps), a landing flashes
 * white and fades;
 * each lane its own hue, deep blue on lane 1 through violet and magenta to
 * hot pink on lane 12, so scrolling shows where you are; an empty lane shows a faint
 * bottom pad. The underglow is lit from above by the balls, over a faint
 * lime. */

#include "midi_clock.h"

#include <stdbool.h>
#include <stdint.h>

/* The mode's color (menu pad, underglow): lime, a bouncing ball. */
#define TILES_BOUNCE_COLOR_R 0.6f
#define TILES_BOUNCE_COLOR_G 1.0f
#define TILES_BOUNCE_COLOR_B 0.0f

void tiles_bounce_mode_init(void);
/* Entering: a finger already down isn't a tap. Leaving: nothing stops. */
void tiles_bounce_mode_enter(void);

/* Every scan (and op_mode's playback ticks), shown or not: steps the balls
 * on the shared clock. `shown`: the balls are on the pads, so landings are
 * felt; in another mode (or under a menu) they only sound. */
void tiles_bounce_mode_advance(tiles_midi_clock_state_t clock, bool shown);

/* Every scan while the mode is shown. */
void tiles_bounce_mode_handle_input(uint32_t now_ms);
void tiles_bounce_mode_render(uint32_t now_ms, float beat_flash, float diamond_led, bool clock_running);

/* Transport, for op_mode.c's "-"/"+". start(true): every ball lands now (beat
 * one) and bounces from there; start(false): carry on. */
bool tiles_bounce_mode_is_running(void);
void tiles_bounce_mode_start(bool from_floor);
/* "+" under an external clock: start(true) on the clock's next beat. */
void tiles_bounce_mode_start_on_beat(void);
void tiles_bounce_mode_pause(void);
void tiles_bounce_mode_rewind(void);
/* Real gravity (a square click while shown, via op_mode.c). */
bool tiles_bounce_mode_is_gravity(void);
void tiles_bounce_mode_toggle_gravity(void);

/* Ends any sounding notes (a panic, or a mode that needs the channel). */
void tiles_bounce_mode_end_all_notes(void);

/* The hold-to-pick pitch view (bounce.h's pads). op_mode.c cancels it on
 * circle + triangle and keeps standby off while it's open. */
bool tiles_bounce_mode_is_picking(void);
void tiles_bounce_mode_cancel_pick(void);

/* Circle + "-"/"+": one column left (-1) or right (+1). */
void tiles_bounce_mode_scroll(int8_t direction);
