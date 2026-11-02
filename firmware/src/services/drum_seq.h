#pragma once

/* Drum sequencer mode, hardware side (engine: drum_pattern.h), channel 10.
 *   columns 1-4    the selected drum's 16 steps: touch toggles; circle +
 *                  step rolls every drum on it (harder = more hits), slip
 *                  style, recording nothing
 *   columns 5-6    8 drums: tap selects, push plays; a resting finger
 *                  feels its steps; circle + hold 1 s clears it, 3 s all
 *   circle + triangle  page 2 (steps 17-32)
 *   "-" / "+"      stop / start; circle + "-"/"+" previous / next 8 drums
 *   circle         tap tempo; diamond: DAW transport
 * Orange; the selected drum and sounding playhead blue. Plays on in other
 * modes; saves itself after every change (log_store, never erasing while
 * the clock runs). */

#include "kv_store.h"
#include "midi_clock.h"

#include <stdbool.h>
#include <stdint.h>

/* The mode's color (menu pad, underglow, armed steps): orange, between the
 * Sequencer's scarlet and Song's amber (op_mode.c's sequencer family). */
#define TILES_DRUM_SEQ_COLOR_R 1.0f
#define TILES_DRUM_SEQ_COLOR_G 0.32f
#define TILES_DRUM_SEQ_COLOR_B 0.0f

/* Volca mode: this same drum mode (same pattern, gestures and saving) for a
 * Volca Sample on multi-channel firmware (one part per MIDI channel): drum
 * k (the k-th from C1: the first bank's eight, then the next bank's) plays
 * C (TILES_VOLCA_NOTE) on MIDI channel k, 1-16, instead of its Drum Rack
 * note on channel 10. Drums past channel 16 send nothing. Cyan where drum
 * mode is orange. op_mode.c's menu turns it on and off. */
#define TILES_VOLCA_NOTE 60u /* C4: the sample at its own pitch */
#define TILES_VOLCA_COLOR_R 0.0f /* cyan: the hardware family, with Ableton's teal */
#define TILES_VOLCA_COLOR_G 0.75f
#define TILES_VOLCA_COLOR_B 1.0f

/* Switches the drum output between Drum Rack (false) and Volca (true).
 * Notes still sounding are ended first, on the routing they started on. */
void tiles_drum_seq_set_volca(bool volca);
bool tiles_drum_seq_is_volca(void);

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
/* Phase lock (setting seq.lock via op_mode.h): drum_pattern.h's player. */
void tiles_drum_seq_set_phase_lock(bool on);
void tiles_drum_seq_pause(void);
void tiles_drum_seq_rewind(void);

/* Circle + "-"/"+": -1 / +1 bank of 8 drums. */
void tiles_drum_seq_bank_step(int direction);

/* Ends every sounding note, sequenced or played (another feature took the
 * board). */
void tiles_drum_seq_end_all_notes(void);
