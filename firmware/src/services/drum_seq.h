#pragma once

/* Drum sequencer mode: a 16-step drum machine on the grid, playing a Drum
 * Rack (or any General MIDI drum kit) on MIDI channel 10
 * (TILES_MIDI_CH_DRUMS). The layout, note map and step engine are in
 * services/drum_pattern.h; this file is the hardware side. services/op_mode.c
 * owns the mode switch, the buttons' shared gestures and the clock.
 *
 *   columns 1-4   the selected drum's 16 steps. Tap: on / off. Hold: chance
 *                 (push in for more). Circle + step: repeats within the step
 *                 (push in for more), like the sequencer.
 *   columns 5-6   8 drums (C1-G1 in Live's names on the first bank). Tap:
 *                 select (its steps show on the left). Push: play it, with
 *                 the strike's velocity. Circle + hold 3 s: clear its steps.
 *   "-" / "+"     stop / start, like the sequencer ("-" twice rewinds).
 *   circle + "-"/"+"  the previous / next 8 drums.
 *   circle        tap tempo (no external clock).
 *   diamond       DAW transport, as in the other play modes.
 *
 * Colors: lime (the mode's color, its underglow too); the playhead white;
 * a drum flashes white as it plays. The beat keeps playing in the
 * background in every other mode, like the sequencer's lanes. The pattern
 * lives in RAM: it's lost at power-off. */

#include "midi_clock.h"

#include <stdbool.h>
#include <stdint.h>

/* The mode's color (menu pad, underglow, armed steps): lime, not used by
 * any other mode or indicator. */
#define TILES_DRUM_SEQ_COLOR_R 0.5f
#define TILES_DRUM_SEQ_COLOR_G 1.0f
#define TILES_DRUM_SEQ_COLOR_B 0.0f

void tiles_drum_seq_init(void);

/* Entering / leaving the mode (services/op_mode.c set_active_mode()). */
void tiles_drum_seq_enter(void);
void tiles_drum_seq_leave(void);

/* Every scan, whatever is shown, so the beat plays in the background.
 * `shown`: drum mode is on screen (sequenced hits pulse the step pads). */
void tiles_drum_seq_advance(tiles_midi_clock_state_t clock, bool shown);

/* Pads, while the mode is shown and no menu is open. */
void tiles_drum_seq_handle_input(uint32_t now_ms);

/* Draws the grid, button LEDs and underglow through the standby setters.
 * `beat_flash` lights circle, `diamond_led` is the transport LED level. */
void tiles_drum_seq_render(uint32_t now_ms, float beat_flash, float diamond_led, bool clock_running);

/* Transport, driven by op_mode's "-"/"+" handling (which also runs the
 * shared clock). start: at the nearest beat, from step 1 if `restart`. */
bool tiles_drum_seq_is_running(void);
void tiles_drum_seq_start(bool restart);
void tiles_drum_seq_pause(void);
void tiles_drum_seq_rewind(void);

/* Circle + "-"/"+": -1 / +1 bank of 8 drums. */
void tiles_drum_seq_bank_step(int direction);

/* A step's chance / repeat dial is open (it owns the grid). */
bool tiles_drum_seq_edit_is_open(void);
void tiles_drum_seq_edit_cancel(void);

/* Ends every sounding note, sequenced or played (another feature took the
 * board). */
void tiles_drum_seq_end_all_notes(void);
