#pragma once

/* Bouncing notes mode, hardware side (physics: bounce_core.h). Every column
 * is a lane whose ball plays the lane's note on landing.
 *   tap             drop the ball from that height (1-4: every 1, 2, 4, 6
 *                   eighths); the same height again stops it
 *   hold a pad      pick the lane's pitch on the melodic surface
 *   circle + "-"/"+"  scroll (12 lanes, 6 shown)
 *   "-" / "+"       stop / start, as in the sequencer; circle: tap tempo
 *   square          Real gravity on/off: throws that lose 30% height each
 *                   landing, in free time
 * Lanes climb the scale from the key note (lane 1 = key note); a picked
 * pitch stays put. Channel 11. Keeps playing in other modes. Each lane its
 * own hue, deep blue to hot pink; underglow lit from above, over lime. */

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
