#pragma once

/* Operation modes and their menus: what the pads and the triangle,
 * diamond, "-"/"+" and circle keys do in each mode. The full map of modes
 * and keys: services/README.md, "Modes and keys". */

#include <stdbool.h>
#include <stdint.h>

/* Call once at boot with watchdog_enable_caused_reboot()'s value. True
 * keeps the mode and each lane's pattern and running state across a crash. */
void tiles_op_mode_init(bool crash_recovered);

/* Menus, gestures, the mode's input and drawing. Every main-loop pass,
 * after the button/touch scans and tiles_midi_clock_scan(). */
void tiles_op_mode_scan(void);

/* Steps playback on the clock. main.c calls it after the slower services,
 * so a Clock arriving mid-pass plays within a fraction of a pass. */
void tiles_op_mode_playback_tick(void);

/* True while a menu is open or the mode draws the whole grid itself.
 * Exclusive with the other grid owners; chord mode claims only its strip. */
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
 * yellow recording pulse on the underglow. */
bool tiles_op_mode_song_capture_is_active(void);

/* True if `note` sounds on the slot a Song capture records, so lighting.c
 * can flash its pad. */
bool tiles_op_mode_song_capture_is_note_sounding(uint8_t note);

/* Melodic echo layers: 0 = the main TILES DISPLAY (any channel but 2),
 * 1 = a second one (MIDI channel 2, drawn soft red). */
#define TILES_OP_MODE_ECHO_LAYERS 2u

/* True if `note` is held by an incoming Note-On on echo `layer` in melodic
 * mode (from the TILES DISPLAY Max for Live device). */
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

/* The menu entries the companion app can add or remove (settings modes.*).
 * Melodic is always on. */
typedef enum {
    TILES_MENU_DUET = 0,
    TILES_MENU_SEQUENCER,
    TILES_MENU_DRUMS,
    TILES_MENU_SONG,
    TILES_MENU_PRISM,
    TILES_MENU_BASS,
    TILES_MENU_ABLETON,
    TILES_MENU_VOLCA,
    TILES_MENU_BOUNCE,
    TILES_MENU_CRADLE,
    TILES_MENU_SAND,
    TILES_MENU_RIPPLES,
    TILES_MENU_ORBITS,
    TILES_MENU_ENTRY_COUNT
} tiles_op_mode_menu_entry_t;

/* Phase lock (setting seq.lock, off by default): a lane started while
 * others play joins on the step it would be on from the song's top. */
void tiles_op_mode_set_phase_lock(bool on);

/* The sequencer's autosave, for INFO (seq.*). */
typedef struct {
    bool storage;   /* its flash region is usable */
    bool pending;   /* a change not saved yet */
    bool truncated; /* the last save didn't fit every pattern */
    uint16_t saved_bytes;
    uint32_t saves;
} tiles_op_mode_seq_store_info_t;
tiles_op_mode_seq_store_info_t tiles_op_mode_get_seq_store_info(void);
bool tiles_op_mode_get_phase_lock(void);

/* Off: the pad goes dark and can't be picked; if that mode is in use, back
 * to melodic. */
void tiles_op_mode_set_menu_entry_enabled(tiles_op_mode_menu_entry_t entry, bool enabled);
bool tiles_op_mode_get_menu_entry_enabled(tiles_op_mode_menu_entry_t entry);

/* A square click outside the Feel menu. True if the mode took it (Bouncing
 * notes toggles Real gravity); false: it toggles pitch bend. */
bool tiles_op_mode_handle_square_click(void);
