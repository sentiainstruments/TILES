#pragma once

/* Operation modes and their menus: what the pad grid and the triangle,
 * diamond, "-"/"+" and circle buttons do in each mode.
 *
 * ---- Modes (triangle opens the mode menu: one pad per mode) --------------
 *   1 Melodic      (magenta)  default. Notes via services/expression.c.
 *   2 Sequencer    (red)      4 lanes x 24 steps (below).
 *   3 Drums        (lime)     16/32-step drum sequencer on MIDI channel
 *                             10, Drum Rack note order; circle + triangle
 *                             flips its step page (services/drum_seq.h).
 *   4 Chord        (blue)     columns 1-2 play chords (op_mode.c), columns
 *                             3-6 are a melody grid (expression.c).
 *   5 Song         (yellow)   BETA, untested on hardware: a 24-slot
 *                             looper, 128 steps per pattern.
 *   6 Ableton      (teal)     Scene Launch: clip grid for Live's Session
 *                             View (daw-integration/).
 *   7 Bass guitar  (amber)    rows = strings, columns = frets; "-"/"+"
 *                             shift the fret window (note_map.h). First
 *                             pad of row 2: row 1 is full.
 * The current mode pulses white in the menu. A pick (press past half)
 * takes effect once the finger lifts, so it can't play a note in the new
 * mode. Melodic, bass guitar and chord pass notes through expression.c;
 * sequencer, Song, Ableton and drums draw the whole grid themselves.
 *
 * ---- Buttons ---------------------------------------------------------------
 *   triangle            mode menu (toggle)
 *   circle + triangle   scale picker (one global scale; drums: the other
 *                       step page), or cancel an open
 *                       step edit / capture
 *   diamond             Live transport: click = play/stop, hold 2 s =
 *                       record (CCs on the DAW port). Sequencer: pattern
 *                       bank. Song step editor: back.
 *   circle + diamond    capture: sequencer capture in the sequencer; Song
 *                       capture from melodic/chord/bass/Song; stop all
 *                       clips in Ableton mode; end a Live recording that
 *                       opened melodic mode.
 *   "-" / "+"           sequencer: stop/start the viewed lane ("-" twice
 *                       rewinds); with no clock coming in "+" restarts the
 *                       clock, step 1 now; circle first = length. Drums: stop/start the same way,
 *                       circle first = previous/next 8 drums. Bass guitar:
 *                       frets. Ableton: pan the 5-track window.
 *   circle              tap tempo (sequencer, drums, or during Song
 *                       capture) when no external clock; flashes the beat.
 *
 * ---- Sequencer ---------------------------------------------------------------
 * 4 lanes play at once, each on its own fixed channel
 * (services/midi_channels.h), each choosing one of 6 alternative patterns
 * in the pattern bank (row = lane, column = alternative; circle + tap =
 * save to flash, circle + hold 3 s = delete). Step view: tap = arm/disarm;
 * hold = step pitch, keep holding = probability; circle + step = ratchet.
 * Up to 4 notes per step. 1 step = a sixteenth (6 clocks) from
 * services/midi_clock.h (external clock or tap tempo); starts are
 * quantized to the beat. A 16-step pattern shows as a 4x4 block. Lanes
 * keep playing in the background in every mode and menu. Stored notes are
 * fitted to the current scale as they play; storage never changes.
 * Capture (circle + diamond) records what you play into the viewed lane
 * at the running tempo, additively.
 *
 * ---- Persistence -------------------------------------------------------------
 * Sequencer patterns and the Song library are saved to flash
 * (storage/flash_map.h). Mode, lane patterns and running lanes survive a
 * crash reboot (__uninitialized_ram); see tiles_op_mode_init(). */

#include <stdbool.h>
#include <stdint.h>

/* Call once at boot with watchdog_enable_caused_reboot()'s value. When
 * true, the active mode, each lane's chosen pattern and its running state
 * are kept from before the crash (pattern content always reloads from
 * flash). Scale/octave/key are note_map.h's job. */
void tiles_op_mode_init(bool crash_recovered);

/* Runs menus, button gestures, the active mode's input and rendering, and
 * advances every sequencer lane and Song track. Call every main-loop pass,
 * after the button/touch scans and tiles_midi_clock_scan(), before
 * anything that reads the ownership accessors below. */
void tiles_op_mode_scan(void);

/* True while a menu is open or the mode draws the whole grid itself
 * (sequencer, Ableton, Song except while capturing or picking a pitch).
 * Other grid owners (games, expression menu, transpose, standby) are
 * mutually exclusive with this. Chord mode claims only its strip, via
 * tiles_op_mode_owns_pad(). */
bool tiles_op_mode_owns_pad_grid(void);

/* Wider than owns_pad_grid(): also true in bass guitar mode, which takes
 * "-"/"+" from octave_control.c while notes keep playing. */
bool tiles_op_mode_owns_octave_buttons(void);

/* Per pad: chord mode's 8 strip pads (op_mode.c plays them directly), or
 * the owns_pad_grid() answer for every other mode and whenever a menu is
 * open. services/expression.c checks this before starting a note. */
bool tiles_op_mode_owns_pad(uint8_t logical_pad);

/* True while the sequencer or Song mode is shown, or a lane/track is
 * playing in the background. services/standby.h then uses its longer
 * timeouts. */
bool tiles_op_mode_is_sequencer_active(void);

/* True in melodic or chord mode: where melodic harmonics may play
 * (services/expression.c). Chord strip pads never reach harmonics anyway;
 * bass guitar mode is excluded. */
bool tiles_op_mode_melodic_harmonics_may_play(void);

/* True while a sub-view is open (mode menu, scale picker, pattern bank,
 * step edit, sequencer capture). services/standby.h holds its timer off,
 * since reading a menu takes no touch. */
bool tiles_op_mode_has_menu_open(void);


/* True while a Song capture is recording. services/lighting.c shows the
 * amber recording underglow. */
bool tiles_op_mode_song_capture_is_active(void);

/* True if `note` is sounding on the slot a Song capture is recording, so
 * lighting.c can flash that pad over the current mode's grid. Chord strip
 * pads can't be highlighted. (No "current step" marker for Song's 128
 * steps yet.) */
bool tiles_op_mode_song_capture_is_note_sounding(uint8_t note);

/* Melodic echo layers: 0 = the main TILES DISPLAY (any channel but 2),
 * 1 = a second one (MIDI channel 2, drawn soft red). */
#define TILES_OP_MODE_ECHO_LAYERS 2u

/* True if `note` is held by an incoming Note-On on echo `layer` and
 * melodic mode is active. services/lighting.c checks each pad's note. The
 * notes come from the TILES DISPLAY Max for Live device
 * (daw-integration/ableton/TILES_DISPLAY/). Not shown in chord mode; notes
 * with no pad in the current layout aren't shown. */
bool tiles_op_mode_incoming_note_is_sounding(uint8_t layer, uint8_t note);

/* Ms since `note`'s latest incoming Note-On on `layer` (for lighting's
 * onset flash). Only meaningful while it's sounding. */
uint32_t tiles_op_mode_incoming_note_age_ms(uint8_t layer, uint8_t note);

/* True while the pattern bank's save/delete confirmation shows, with the
 * underglow color to use now. services/lighting.c ranks it above debug
 * mode's pulse. */
bool tiles_op_mode_pattern_flash_underglow_color(float *out_r, float *out_g, float *out_b);

/* Bench/diagnostics (the settings shell's TEST MODE): switches straight to
 * a mode by name (melodic, chord, sequencer, bass, song, ableton, drums),
 * closing any menu. False for an unknown name. */
bool tiles_op_mode_test_set_mode(const char *name);
