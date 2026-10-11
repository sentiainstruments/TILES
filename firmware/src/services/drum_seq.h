#pragma once

/* Drum sequencer mode: a 16-step drum machine on the grid, playing a Drum
 * Rack (or any General MIDI drum kit) on MIDI channel 10
 * (TILES_MIDI_CH_DRUMS). The layout, note map and step engine are in
 * services/drum_pattern.h; this file is the hardware side. services/op_mode.c
 * owns the mode switch, the buttons' shared gestures and the clock.
 *
 *   columns 1-4   the selected drum's steps, one page of 16. Touch: on /
 *                 off, at once (no hold gesture). Circle + a step: the
 *                 playhead jumps there now and every drum on that step
 *                 rolls while held, 2, 3, 4 or 6 hits per step by
 *                 pressure, louder with it; on release the pattern
 *                 carries on from that step. Nothing is recorded.
 *   columns 5-6   8 drums (C1-G1 in Live's names on the first bank). Tap:
 *                 select (its steps show on the left). Push: play it, with
 *                 the strike's velocity. Circle + push: a roll, its rate
 *                 (1/8 to 1/32 triplets) and loudness following pressure,
 *                 in time with the clock. Circle + hold 1 s without pushing:
 *                 clear its steps; on to 3 s: clear the whole pattern.
 *   circle + triangle  the other page (steps 17-32). Anything on page 2
 *                 makes the pattern 32 steps; an empty page 2, 16. (Drums
 *                 follow no scale, so this isn't the scale picker here.)
 *   "-" / "+"     stop / start ("-" twice rewinds). With no clock coming
 *                 in, "+" restarts the clock: step 1 now, not quantized.
 *   circle + "-"/"+"  the previous / next 8 drums.
 *   circle        tap tempo (no external clock).
 *   diamond       DAW transport, as in the other play modes.
 *
 * Colors: lime is the mode's (underglow, steps); each bank of drums has its
 * own green on the drum pads; the selected drum is blue, and so is the
 * playhead on a step that sounds; a drum flashes white as it plays; triangle
 * lights on page 2. The beat keeps playing in the background in every other
 * mode, like the sequencer's lanes. The pattern saves itself after every
 * change (its own flash region, through storage/log_store.h, which never
 * erases while the clock runs) and comes back at power-on. */

#include "kv_store.h"
#include "midi_clock.h"

#include <stdbool.h>
#include <stdint.h>

/* The mode's color (menu pad, underglow, armed steps): lime, not used by
 * any other mode or indicator. */
#define TILES_DRUM_SEQ_COLOR_R 0.5f
#define TILES_DRUM_SEQ_COLOR_G 1.0f
#define TILES_DRUM_SEQ_COLOR_B 0.0f

/* Loads the saved pattern. ops NULL: no flash region, nothing saved. */
void tiles_drum_seq_init(const tiles_kv_ops_t *ops);

/* Every scan: writes a changed pattern to flash at a safe moment (see
 * DRUM_SAVE_QUIET_MS in drum_seq.c). */
void tiles_drum_seq_persist_service(uint32_t now_ms, bool clock_running);

typedef struct {
    bool storage;
    bool pending; /* changed, not saved yet */
    uint16_t saved_bytes;
    uint32_t saves; /* since boot */
} tiles_drum_seq_store_info_t;
tiles_drum_seq_store_info_t tiles_drum_seq_get_store_info(void);

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

/* Circle + triangle: shows the other page of 16 steps. */
void tiles_drum_seq_flip_page(void);

/* Transport, driven by op_mode's "-"/"+" handling (which also runs the
 * shared clock). start: at the nearest beat, from step 1 if `restart`. */
bool tiles_drum_seq_is_running(void);
void tiles_drum_seq_start(bool restart);
void tiles_drum_seq_pause(void);
void tiles_drum_seq_rewind(void);

/* Circle + "-"/"+": -1 / +1 bank of 8 drums. */
void tiles_drum_seq_bank_step(int direction);

/* Ends every sounding note, sequenced or played (another feature took the
 * board). */
void tiles_drum_seq_end_all_notes(void);
