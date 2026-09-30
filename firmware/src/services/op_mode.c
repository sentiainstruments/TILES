#include "op_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "cv_gate.h"
#include "debug_mode.h"
#include "expression.h"
#include "expression_control.h"
#include "game_mode.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_clock.h"
#include "midi_in.h"
#include "midi_channels.h"
#include "midi_out.h"
#include "note_map.h"
#include "octave_control.h"
#include "product_identity.h"
#include "standby.h"
#include "touch.h"

#include "flash_map.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "pico/platform/sections.h"
#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Modes, in the order they were added. (An arpeggiator stub was replaced
 * by bass guitar mode; there is no arp.) */
/* SONG is a separate sequencer-style mode (see "Song mode"); SCENE_LAUNCH
 * is Ableton mode (see "Scene Launch mode"). */
typedef enum {
    OP_MODE_MELODIC = 0,
    OP_MODE_CHORD,
    OP_MODE_SEQUENCER,
    OP_MODE_GUITAR,
    OP_MODE_SONG,
    OP_MODE_SCENE_LAUNCH,
} tiles_op_mode_t;

/* Mode menu: one pad per mode on row 1 (nearest the buttons), each its own
 * color, like the game menu. Column order: melodic, sequencer, bass
 * guitar, chord, Song, Ableton. */
#define OP_MENU_ROW 1u
#define OP_MENU_COL_MELODIC 1u
#define OP_MENU_COL_SEQUENCER 2u
#define OP_MENU_COL_GUITAR 3u
#define OP_MENU_COL_CHORD 4u
#define OP_MENU_COL_SONG 5u
#define OP_MENU_COL_SCENE_LAUNCH 6u

/* Mode colors: melodic = Sentia magenta, sequencer = red, bass guitar =
 * amber (matches its fretboard), chord = blue (matches its chord strip). */
#define OP_MENU_MELODIC_R 1.0f
#define OP_MENU_MELODIC_G 0.0f
#define OP_MENU_MELODIC_B 1.0f
#define OP_MENU_CHORD_R 0.0f
#define OP_MENU_CHORD_G 0.0f
#define OP_MENU_CHORD_B 1.0f
#define OP_MENU_SEQUENCER_R 1.0f
#define OP_MENU_SEQUENCER_G 0.0f
#define OP_MENU_SEQUENCER_B 0.0f
#define OP_MENU_GUITAR_R 1.0f
#define OP_MENU_GUITAR_G 0.5f
#define OP_MENU_GUITAR_B 0.0f
/* Song = yellow (like the capture underglow); each track's pad gets a
 * random hue in a yellow-green-to-orange band. */
#define OP_MENU_SONG_R 1.0f
#define OP_MENU_SONG_G 1.0f
#define OP_MENU_SONG_B 0.0f
/* Ableton (Scene Launch) = teal, matching its idle underglow. */
#define OP_MENU_SCENE_LAUNCH_R 0.0f
#define OP_MENU_SCENE_LAUNCH_G 0.5f
#define OP_MENU_SCENE_LAUNCH_B 0.5f

/* Triangle LED while the mode menu is open (monochrome, so brightness
 * only). Off otherwise. */
#define OP_TRIANGLE_LED_MENU_LEVEL 1.0f

/* Diamond LED (Ableton transport): stopped = off, playing = on, record
 * armed = double blink + pause, recording = slow pulse (like circle's
 * deep-sleep pulse). */
#define OP_TRANSPORT_LED_STOPPED_LEVEL 0.0f
#define OP_TRANSPORT_LED_PLAYING_LEVEL 1.0f
/* Armed: on, gap, on, pause (860 ms cycle). Unmeasured. */
#define OP_TRANSPORT_ARMED_BLINK_ON_MS 120u
#define OP_TRANSPORT_ARMED_BLINK_GAP_MS 120u
#define OP_TRANSPORT_ARMED_PAUSE_MS 500u
/* Recording: the same breathing shape as standby's deep-sleep pulse,
 * duplicated (standby's constants are file-local). */
#define OP_TRANSPORT_RECORDING_PULSE_PERIOD_MS 3000.0f
#define OP_TRANSPORT_RECORDING_PULSE_MIN 0.03f
#define OP_TRANSPORT_RECORDING_PULSE_MAX 0.35f
/* "-"/"+" LEDs as a two-state start/stop indicator in sequencer mode. */
#define OP_TRANSPORT_LED_LEVEL 0.8f

/* Menu standard: the selected/active item is bright pulsing white;
 * available items a readable dim color; unavailable items off. Same shape
 * as the expression menu (expression_control.c), redefined per file. */
#define OP_MENU_SELECTED_PULSE_PERIOD_MS 900.0f
#define OP_MENU_SELECTED_PULSE_MIN 0.5f
#define OP_MENU_SELECTED_PULSE_MAX 1.0f
#define OP_MODE_PI 3.14159265358979323846f
/* Scale picker's "available" level (magenta). Raised from 0.35. */
#define OP_SCALE_AVAILABLE_LEVEL 0.5f

static float menu_selected_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / OP_MENU_SELECTED_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
    return OP_MENU_SELECTED_PULSE_MIN + (OP_MENU_SELECTED_PULSE_MAX - OP_MENU_SELECTED_PULSE_MIN) * raw;
}

/* "Something is running in the background" pulse, one period per beat of
 * the current tempo (tiles_midi_clock_get_ms_per_beat()): calmer than the
 * selection pulse. Also used by the diamond LED. */
#define OP_BACKGROUND_PULSE_MIN 0.0f
#define OP_BACKGROUND_PULSE_MAX 1.0f

static float background_pattern_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / tiles_midi_clock_get_ms_per_beat();
    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
    return OP_BACKGROUND_PULSE_MIN + (OP_BACKGROUND_PULSE_MAX - OP_BACKGROUND_PULSE_MIN) * raw;
}

/* Menus (mode and scale picker) select on a real press, more than half
 * travel (900 is roughly full scale on this hardware), not on touch. */
#define OP_MENU_SELECT_DEPTH_THRESHOLD 450.0f
/* Menu haptics use tiles_haptics_trigger_touch_pulse() (self-ending), not
 * trigger_kick(), whose sustain never got stopped for menu touches and
 * left pads buzzing. Only real notes (expression.c) and sequencer steps
 * (paired with seq_end_current_note()) use kick(). */

/* ---- Tap tempo (circle) + beat flash ------------------------------------
 * In the sequencer, with no external clock, circle presses are tap-tempo
 * taps (handle_circle_tap() decides what counts; services/midi_clock.h
 * does the math). The beat flash follows the shared pulse count, so it
 * shows external clock and tap tempo alike. */
#define OP_BEAT_FLASH_DURATION_MS 100u
#define OP_BEAT_FLASH_LEVEL 1.0f
/* 24 pulses per quarter note (MIDI spec): one flash per beat. */
#define OP_CLOCK_PULSES_PER_BEAT 24u

/* Survives a watchdog reset (__uninitialized_ram, like note_map.c), along
 * with s_seq_lane_running[]/s_seq_active_alt[]: mode, which lanes play and
 * which pattern each lane uses come back after a crash. NOT the Ableton
 * transport belief (s_transport_*): Live may have changed meanwhile, so it
 * resets to "stopped" and the next diamond click resyncs. No initializer
 * on purpose; tiles_op_mode_init() sets defaults unless recovering. */
static tiles_op_mode_t __uninitialized_ram(s_active_mode);
static bool s_menu_visible;
/* A mode was picked (press past the threshold) but the pad isn't released
 * yet. Leaving the menu and switching mode both wait for every pad to
 * lift, so the picking pad can't start a note in the new mode. */
static bool s_menu_pending;
static tiles_op_mode_t s_menu_pending_mode;

/* Triangle's LED sometimes showed lit right after boot despite being set
 * off at init (cause not found; likely a PCA9685 power-on glitch). So the
 * "off" write repeats every scan for OP_BOOT_RELIGHT_GUARD_MS after boot.
 * A workaround, not a root-cause fix. */
#define OP_BOOT_RELIGHT_GUARD_MS 1000u
static uint32_t s_boot_relight_guard_until_ms;

static bool s_triangle_was_held;
/* Diamond was also down during this triangle press: part of the game-mode
 * 4-button combo, not a click. Checked on release. */
static bool s_triangle_press_had_conflict;
/* Circle was also held during this triangle press (shift: scale picker).
 * Sticky once seen, since the two rarely release in the same tick. */
static bool s_triangle_press_was_shift;

static bool s_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Scale picker (triangle + circle) -----------------------------------
 * The one per-mode sub-menu: the scale picker, in every mode that uses a
 * scale. */
static bool s_scale_menu_visible;
/* Closing the scale picker waits for every pad to lift (like
 * s_menu_pending); the scale itself changes immediately (no MIDI side
 * effect). */
static bool s_scale_menu_pending_exit;
static bool s_diamond_was_held;
static bool s_diamond_press_had_conflict; /* same as s_triangle_press_had_conflict, for diamond */
static bool s_scale_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Diamond: Ableton transport ---------------------------------------
 * See handle_diamond_transport(). s_diamond_press_start_ms times the 2 s
 * record-arm hold; s_diamond_record_armed fires once per hold.
 * s_transport_playing/_recording are this device's belief about Live's
 * transport (MIDI can't query it); recording implies playing. */
static uint32_t s_diamond_press_start_ms;
static bool s_diamond_record_armed;
static bool s_transport_playing;
static bool s_transport_recording;
/* Circle was also held during this diamond press. In the sequencer:
 * circle+diamond = capture, diamond alone = pattern bank. Sticky, like
 * s_triangle_press_was_shift. */
static bool s_diamond_press_was_shift;

/* Tap presses register on the PRESS edge (timing matters), so
 * handle_circle_tap() checks the other combo buttons at that moment
 * instead of latching a conflict flag. */
static bool s_circle_was_held;
/* Beat flash state, computed every scan, drawn by render_sequencer(). */
static uint32_t s_last_beat_index = 0xFFFFFFFFu;
static uint32_t s_beat_flash_start_ms;

/* ---- Sequencer state ------------------------------------------------- */

#define OP_SEQ_NUM_STEPS TILES_NUM_PADS /* 24: one step per pad */
/* 6 clocks per step = one sixteenth note (24 per quarter / 4). */
#define OP_SEQ_CLOCKS_PER_STEP 6u
/* Clock-triggered notes have no strike, so a fixed, unaccented velocity. */
#define OP_SEQ_VELOCITY 100u

#define OP_SEQ_DIM_RED_LEVEL 0.3f
/* Playhead on an UNARMED step: dim white, so the current step always
 * shows. */
#define OP_SEQ_CURSOR_LEVEL 0.15f
/* Playhead on an ARMED step: blue, whether or not it sounds this pass
 * (probability can skip it), so it never looks like an empty step. */
#define OP_SEQ_CURSOR_ARMED_R 0.0f
#define OP_SEQ_CURSOR_ARMED_G 0.3f
#define OP_SEQ_CURSOR_ARMED_B 1.0f

/* ---- Pattern bank: 4 lanes x 6 alternatives --------------------------------
 * All 4 LANES play at once, each with its own fixed MIDI channel
 * (s_seq_lane_channel[]). Each lane has 6 alternative patterns (a row of
 * the bank); picking another one swaps only that lane's pattern. All lanes
 * go out over MIDI on their own channels (routing lanes to CV/gate is a
 * companion-app idea, not built). */
#define OP_SEQ_NUM_LANES 4u
#define OP_SEQ_ALTS_PER_LANE 6u
#define OP_SEQ_MIN_LENGTH 1u
#define OP_SEQ_MAX_LENGTH OP_SEQ_NUM_STEPS
/* Magenta flash on the pads showing the new length after circle + "-"/"+". */
#define OP_SEQ_LENGTH_FLASH_DURATION_MS 400u
/* Up to 4 hits per step, each at least one clock pulse apart. */
#define OP_SEQ_MAX_RATCHET 4u

/* Up to 4 notes per step, enough to capture a chord (bass, root, fifth,
 * third; OP_CHORD_NUM_VOICES). Fits the one-sector pattern store thanks to
 * the packed flash layout (tiles_pattern_flash_t); the _Static_assert
 * after tiles_pattern_store_t keeps it from regressing. This runtime
 * struct stays unpacked.
 * step_note_count[i] == 0 means no note, even if armed (tap-to-arm arms an
 * empty step until a pitch is assigned); seq_fire_note() treats it as
 * silent. */
#define OP_SEQ_MAX_NOTES_PER_STEP 4u

typedef struct {
    bool step_armed[OP_SEQ_NUM_STEPS];
    bool step_pitch_override[OP_SEQ_NUM_STEPS];
    /* The step's notes, used only if step_pitch_override[i]. */
    uint8_t step_notes[OP_SEQ_NUM_STEPS][OP_SEQ_MAX_NOTES_PER_STEP];
    uint8_t step_note_count[OP_SEQ_NUM_STEPS];
    uint8_t step_probability_percent[OP_SEQ_NUM_STEPS]; /* 0-100, default 100; used only if probability_enabled */
    uint8_t step_ratchet_count[OP_SEQ_NUM_STEPS];    /* 1..OP_SEQ_MAX_RATCHET, default 1 (none) */
    /* Per-pattern probability switch: while false, every step fires as if at
     * 100%. Turned on when any step's probability is edited
     * (handle_edit_mode()); nothing turns it back off. */
    bool probability_enabled;
    uint8_t length;  /* 1..24 active steps */
    /* No channel (that's a lane property) and no scale: patterns follow the
     * one global scale, applied when a note plays (tiles_note_map_quantize_to_
     * scale() in seq_fire_note()), without changing stored notes. */
} op_seq_pattern_t;

/* [lane][alternative] */
static op_seq_pattern_t s_seq_pattern[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
/* True once this slot has been saved to flash (see "Pattern
 * persistence"). Not the same as having content: a slot can hold unsaved
 * notes, or be saved and since cleared. */
static bool s_pattern_slot_saved[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
/* Which alternative (0..5) each lane is PLAYING, independent of which lane
 * is being viewed (s_seq_edit_lane). */
/* Survives a crash reboot (see s_active_mode). */
static uint8_t __uninitialized_ram(s_seq_active_alt)[OP_SEQ_NUM_LANES];
/* Which lane the step view shows and edits; the others keep playing.
 * active_pattern() resolves through this. */
static uint8_t s_seq_edit_lane;
/* One fixed channel per lane (services/midi_channels.h,
 * TILES_MIDI_CH_SEQ_LANE_0.._3), never shared. */
static uint8_t s_seq_lane_channel[OP_SEQ_NUM_LANES];
/* See OP_SEQ_LENGTH_FLASH_DURATION_MS. 0 at boot can flash for at most one
 * frame. */
static uint32_t s_seq_length_flash_ms;

/* ---- Per-lane playback state ---------------------------------------------
 * Indexed by lane. s_seq_edit_lane is only about what is shown; see
 * active_pattern() vs. pattern_for_lane(). */
/* Each lane starts and stops on its own ("-"/"+" act on the viewed lane);
 * only the tempo is global. This is a per-lane gate inside
 * seq_advance_clock(), on top of midi_clock's single running flag, which
 * turns on when any lane starts and off when every lane has stopped. */
/* Survives a crash reboot (see s_active_mode). tiles_op_mode_init() also
 * restarts the clock for restored running lanes. */
static bool __uninitialized_ram(s_seq_lane_running)[OP_SEQ_NUM_LANES];
static uint8_t s_seq_current_step[OP_SEQ_NUM_LANES]; /* 0..23 */
static bool s_seq_note_sounding[OP_SEQ_NUM_LANES];
static uint8_t s_seq_sounding_pad[OP_SEQ_NUM_LANES]; /* 1..24, valid iff s_seq_note_sounding[lane] */
static uint8_t s_seq_sounding_channel[OP_SEQ_NUM_LANES];
/* The step's fired notes, [0..s_seq_sounding_note_count[lane]-1]. */
static uint8_t s_seq_sounding_notes[OP_SEQ_NUM_LANES][OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_sounding_note_count[OP_SEQ_NUM_LANES];
/* Whether this lane's current note kicked a haptic. Only the lane shown on
 * the grid gets haptics (background lanes play silently to the touch).
 * Captured at fire time, like the channel and notes, so ending the note
 * undoes exactly what starting it did even if the view changed meanwhile. */
static bool s_seq_sounding_haptics[OP_SEQ_NUM_LANES];
static uint32_t s_seq_step_started_at_pulse[OP_SEQ_NUM_LANES];
static bool s_seq_prev_pad_touched[TILES_NUM_PADS];
/* Quantized (re)start: a manual start waits for the next beat boundary
 * instead of jumping in mid-phrase. Set by seq_start() and by "+";
 * consumed by seq_advance_clock(). s_seq_pending_restart picks what happens
 * at the boundary: restart from step 0 ("+" while playing, or entering the
 * mode) vs. resume where the playhead stopped ("+" while stopped). Per
 * lane, since seq_advance_clock() handles each lane separately. */
static bool s_seq_pending_start[OP_SEQ_NUM_LANES];
static bool s_seq_pending_restart[OP_SEQ_NUM_LANES];

/* Ratchet state for the CURRENT step, reset on each new step:
 * s_seq_ratchet_remaining counts extra hits still owed after the first. */
static uint8_t s_seq_ratchet_remaining[OP_SEQ_NUM_LANES];
static uint32_t s_seq_ratchet_interval_pulses[OP_SEQ_NUM_LANES];
static uint32_t s_seq_next_ratchet_pulse[OP_SEQ_NUM_LANES];

/* ---- Per-step editing: pitch, probability, ratchet ---------------------
 *   - Hold a step: pitch edit (OP_SEQ_PITCH_ASSIGN_HOLD_MS); keep holding:
 *     probability (OP_SEQ_PROBABILITY_HOLD_MS). Two tiers only; three
 *     felt like too many to land reliably.
 *   - Circle held first, then touch a step: ratchet edit, immediately.
 * Pitch edit stays open after release until a pad is tapped (that pad's
 * note becomes the step's note). Probability and ratchet are live dials:
 * the held pad's depth sets the value, committed at release.
 * Triangle + circle cancels any of them.
 * A plain tap toggles the step's armed state on RELEASE, so holding an
 * armed step to edit it doesn't disarm it first. */
typedef enum {
    OP_SEQ_EDIT_NONE = 0,
    OP_SEQ_EDIT_PITCH,
    OP_SEQ_EDIT_PROBABILITY,
    OP_SEQ_EDIT_RATCHET,
} op_seq_edit_mode_t;

#define OP_SEQ_PITCH_ASSIGN_HOLD_MS 350u
#define OP_SEQ_PROBABILITY_HOLD_MS 1200u

static uint32_t s_seq_step_touch_started_ms[TILES_NUM_PADS]; /* 0 = not timing a hold */
static op_seq_edit_mode_t s_seq_edit_mode;
static uint8_t s_seq_edit_step;         /* 0..23, valid iff s_seq_edit_mode != OP_SEQ_EDIT_NONE */
static uint32_t s_seq_edit_started_ms;  /* original touch-down time; escalation is timed from here */
static bool s_pitch_edit_prev_pad_touched[TILES_NUM_PADS]; /* only used in OP_SEQ_EDIT_PITCH */
/* Pitch edit sets ONE note (a tapped pad commits it and closes). Two
 * multi-note versions were tried and rejected on hardware. Steps still
 * hold up to 4 notes, for chords recorded by capture. */
/* Depth dials (probability, ratchet): lifting a finger passes back
 * through every lower depth, which dragged the value down on release.
 * Depth is written live in both directions, except below
 * OP_SEQ_EDIT_RELEASE_GUARD_DEPTH, the tail of a lift, where writes stop
 * and the last real value stays. (Tracking only the peak made it
 * impossible to dial back down.) So the lowest value reachable while
 * holding is just above 0; disarm the step for "never". The guard value
 * is a first guess. */
#define OP_SEQ_EDIT_RELEASE_GUARD_DEPTH 60u

/* ---- Pattern/channel picker: REMOVED -------------------------------------
 * Triangle + circle opens the scale picker in every mode; the sequencer's
 * old picker is gone. Lanes and alternatives are chosen in the pattern
 * bank now. */

/* ---- Transport + length ("-"/"+", sequencer only) -------------------------
 * Plain press = transport, resolved on release (like octave_control.c).
 * Circle held FIRST, then "-"/"+" = length -1/+1 (only that order).
 * octave_control.c ignores "-"/"+" while the sequencer owns the grid. */
static bool s_minus_was_held;
static bool s_plus_was_held;
static bool s_minus_used_as_combo;
static bool s_plus_used_as_combo;

/* ---- Chord mode (columns 1-2) -------------------------------------------
 * Melody columns 3-6 play through expression.c unchanged (remapped by
 * note_map). The 8 chord-strip pads are claimed here
 * (tiles_op_mode_owns_pad()) and play chords directly. State is per pad,
 * since several chord pads can be held at once. */
/* Chord mode's fixed channel (services/midi_channels.h), outside the live
 * MPE range, so a melody note can never take it. */
#define OP_CHORD_CHANNEL TILES_MIDI_CH_CHORD
static bool s_chord_pad_touched[TILES_NUM_PADS];
static bool s_chord_pad_sounding[TILES_NUM_PADS];

/* ---- Chord voicing: one fixed shape, velocity from strike speed --------
 * Each chord pad always plays the same voicing: bass + root + fifth +
 * open third + seventh. Strike speed sets loudness only (the same velocity
 * curve as melodic notes, tiles_expression_velocity_from_strike()), never
 * which notes play. Adaptive voicing (drifted around the range) and
 * pressure tiers (triad -> jazz voicing) were both tried and dropped for
 * this predictable shape. Chord quality comes from the actual intervals
 * tiles_note_map_get_chord_notes() returns, so it follows the scale. */

/* Bass voice: the root one more octave down, below the chord register. */
#define OP_CHORD_BASS_EXTRA_OCTAVE_SEMITONES 12
/* The seventh (already computed by note_map) is the fifth voice: maj7,
 * dom7, m7 or half-diminished by scale degree. A live chord plays all 5;
 * a chord captured into a sequencer or Song step keeps 4 (step storage is
 * OP_SEQ_MAX_NOTES_PER_STEP / OP_SONG_MAX_NOTES_PER_STEP = 4, changing it
 * means migrating the flash format), dropping the seventh. */
#define OP_CHORD_NUM_VOICES 5u /* bass + root + fifth + open third + seventh */

static uint8_t s_chord_pad_notes[TILES_NUM_PADS][OP_CHORD_NUM_VOICES];
/* Velocity of this pad's last strike, so capture records the dynamics
 * actually played. Set only in chord_pad_strike(). */
static uint8_t s_chord_pad_last_velocity[TILES_NUM_PADS];
/* Strike tracking per chord pad (touch start, peak depth), enough to feed
 * the melodic velocity curve; see handle_chord_pad_taps(). */
static uint32_t s_chord_pad_touch_start_ms[TILES_NUM_PADS];
static float s_chord_pad_peak_depth[TILES_NUM_PADS];
static bool s_chord_pad_awaiting_strike[TILES_NUM_PADS];

static uint8_t clamp_midi_note(int note) {
    if (note < 0) {
        return 0u;
    }
    if (note > 127) {
        return 127u;
    }
    return (uint8_t)note;
}

/* The fixed 5-voice shape. */
static void build_chord_voicing(const uint8_t raw[TILES_NOTE_MAP_CHORD_NUM_NOTES],
                                 uint8_t out_notes[OP_CHORD_NUM_VOICES]) {
    uint8_t root = raw[0];
    uint8_t third = raw[1];
    uint8_t fifth = raw[2];
    uint8_t seventh = raw[3];

    uint8_t bass = clamp_midi_note((int)root - OP_CHORD_BASS_EXTRA_OCTAVE_SEMITONES);
    uint8_t open_third = clamp_midi_note((int)third + 12);

    out_notes[0] = bass;
    out_notes[1] = root;
    out_notes[2] = fifth;
    out_notes[3] = open_third;
    out_notes[4] = seventh;
}

static void chord_pad_note_off(uint8_t pad) {
    if (!s_chord_pad_sounding[pad - 1u]) {
        return;
    }
    for (uint8_t i = 0; i < OP_CHORD_NUM_VOICES; i++) {
        tiles_midi_note_off(OP_CHORD_CHANNEL, s_chord_pad_notes[pad - 1u][i], 0u); /* chord ends on touch release: release velocity 0 */
        tiles_cv_gate_note_off(s_chord_pad_notes[pad - 1u][i]);
    }
    tiles_haptics_stop(pad);
    s_chord_pad_sounding[pad - 1u] = false;
}

/* Strikes `pad` at `velocity` once a strike has been measured. */
static void chord_pad_strike(uint8_t pad, uint8_t velocity) {
    chord_pad_note_off(pad);
    uint8_t raw[TILES_NOTE_MAP_CHORD_NUM_NOTES];
    tiles_note_map_get_chord_notes(pad, raw);
    build_chord_voicing(raw, s_chord_pad_notes[pad - 1u]);
    s_chord_pad_last_velocity[pad - 1u] = velocity;
    for (uint8_t i = 0; i < OP_CHORD_NUM_VOICES; i++) {
        tiles_midi_note_on(OP_CHORD_CHANNEL, s_chord_pad_notes[pad - 1u][i], velocity);
        tiles_cv_gate_note_on(s_chord_pad_notes[pad - 1u][i], velocity);
    }
    tiles_haptics_trigger_kick(pad, velocity);
    s_chord_pad_sounding[pad - 1u] = true;
}

/* Releases every sounding chord pad and clears in-flight strikes. Called
 * when chord mode stops being the active mode. */
static void chord_end_all_notes(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        chord_pad_note_off(pad);
        s_chord_pad_awaiting_strike[pad - 1u] = false;
    }
}

/* Strike detection like expression.c (touch start, peak depth until it
 * crosses TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA), simplified: no
 * follow-through wait (the threshold is crossed within a few ms of
 * contact, so chords stay immediate), and a touch that never crosses it
 * stays silent. */
static void handle_chord_pad_taps(uint32_t now_ms) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (!tiles_note_map_is_chord_region_pad(pad)) {
            continue;
        }
        bool touched = tiles_touch_is_touched(pad);
        if (touched && !s_chord_pad_touched[pad - 1u]) {
            s_chord_pad_touch_start_ms[pad - 1u] = now_ms;
            s_chord_pad_peak_depth[pad - 1u] = (float)tiles_hall_get_depth(pad);
            s_chord_pad_awaiting_strike[pad - 1u] = true;
        } else if (touched && s_chord_pad_awaiting_strike[pad - 1u]) {
            float depth = (float)tiles_hall_get_depth(pad);
            if (depth > s_chord_pad_peak_depth[pad - 1u]) {
                s_chord_pad_peak_depth[pad - 1u] = depth;
            }
            if (s_chord_pad_peak_depth[pad - 1u] >= TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) {
                uint32_t strike_time_ms = now_ms - s_chord_pad_touch_start_ms[pad - 1u];
                uint8_t velocity =
                    tiles_expression_velocity_from_strike(strike_time_ms, s_chord_pad_peak_depth[pad - 1u]);
                chord_pad_strike(pad, velocity);
                s_chord_pad_awaiting_strike[pad - 1u] = false;
            }
        } else if (!touched && s_chord_pad_touched[pad - 1u]) {
            chord_pad_note_off(pad);
            s_chord_pad_awaiting_strike[pad - 1u] = false;
        }
        s_chord_pad_touched[pad - 1u] = touched;
    }
}

static bool other_feature_owns_input(void) {
    return tiles_game_mode_is_active() || tiles_expression_control_owns_pad_grid() ||
           tiles_octave_control_is_transpose_active() || tiles_standby_is_active() || tiles_standby_is_deep_sleep();
}

/* The pattern being VIEWED/edited (step view, edits, pattern bank,
 * capture, length). Playback uses pattern_for_lane() instead, so changing
 * the view never interferes with what each lane plays. */
static op_seq_pattern_t *active_pattern(void) {
    return &s_seq_pattern[s_seq_edit_lane][s_seq_active_alt[s_seq_edit_lane]];
}

/* The pattern a lane is PLAYING (playback engine only). Same as
 * active_pattern() when lane == s_seq_edit_lane. */
static op_seq_pattern_t *pattern_for_lane(uint8_t lane) {
    return &s_seq_pattern[lane][s_seq_active_alt[lane]];
}

/* A 16-step pattern snaps to a 4x4 block on the left; every other length
 * uses the plain linear pad = step + 1 layout (partial last row). These two
 * functions are the only place that decides, so what lights and what a
 * touch arms can't disagree. They take the pattern explicitly: playback
 * needs each lane's own layout. */
static bool seq_uses_4x4_layout(const op_seq_pattern_t *pat) {
    return pat->length == 16u;
}

/* Pad (1..24) showing `step` in `pat`'s layout. */
static uint8_t seq_pad_for_step(const op_seq_pattern_t *pat, uint8_t step) {
    /* step < 16: generic loops walk all 24 steps even in the 4x4 layout, and
     * steps 16-23 would compute row 5, an invalid pad number. */
    if (seq_uses_4x4_layout(pat) && step < 16u) {
        uint8_t row = (uint8_t)(step / 4u);
        uint8_t col = (uint8_t)(step % 4u);
        return board_pad_for_row_col((uint8_t)(row + 1u), (uint8_t)(col + 1u));
    }
    return (uint8_t)(step + 1u);
}

/* Step that `pad` represents in `pat`'s layout. False only in the 4x4
 * layout, for the two dark right-hand columns. Pads beyond the length
 * still map to a step (drawn dark). */
static bool seq_step_for_pad(const op_seq_pattern_t *pat, uint8_t pad, uint8_t *out_step) {
    if (seq_uses_4x4_layout(pat)) {
        uint8_t idx = (uint8_t)(pad - 1u);
        uint8_t row = (uint8_t)(idx / 6u);
        uint8_t col = (uint8_t)(idx % 6u);
        if (col >= 4u) {
            return false;
        }
        *out_step = (uint8_t)(row * 4u + col);
        return true;
    }
    *out_step = (uint8_t)(pad - 1u);
    return true;
}

static void edit_enter(uint8_t step, uint32_t started_ms); /* defined below; used for hold detection */
static void edit_enter_ratchet(uint8_t step); /* defined below; used for circle + touch */

/* True if `lane` is what the grid shows (sequencer mode, viewed lane). */
static bool seq_lane_haptics_visible(uint8_t lane) {
    return s_active_mode == OP_MODE_SEQUENCER && lane == s_seq_edit_lane;
}

/* Ends the note using the channel and notes captured at note-on, so it's
 * correct even if the pattern bank switched the lane's pattern meanwhile. */
static void seq_end_current_note(uint8_t lane) {
    if (!s_seq_note_sounding[lane]) {
        return;
    }
    /* One Note-Off per note fired for the step. */
    for (uint8_t i = 0; i < s_seq_sounding_note_count[lane]; i++) {
        tiles_midi_note_off(s_seq_sounding_channel[lane], s_seq_sounding_notes[lane][i], 0u); /* clock-fired: release velocity 0 */
        tiles_cv_gate_note_off(s_seq_sounding_notes[lane][i]);
    }
    /* Stop the haptic only if this note started one. One kick/stop per step,
     * even for a chord. */
    if (s_seq_sounding_haptics[lane]) {
        tiles_haptics_stop(s_seq_sounding_pad[lane]);
    }
    s_seq_note_sounding[lane] = false;
    s_seq_sounding_note_count[lane] = 0u;
}

/* Fires one hit of `step` on `lane`: the first hit (seq_enter_step()) and
 * any ratchet hits (seq_advance_clock()) share it. Probability and the
 * current step are seq_enter_step()'s job. */
static void seq_fire_note(uint8_t lane, uint8_t step) {
    seq_end_current_note(lane);
    op_seq_pattern_t *pat = pattern_for_lane(lane);
    /* This lane's own layout, for the haptic pad (only felt for the viewed
     * lane) and for the fallback pitch of a step with no stored note. */
    uint8_t pad = seq_pad_for_step(pat, step);
    /* Stored notes are quantized to the current global scale as they play
     * (tiles_note_map_quantize_to_scale()); storage never changes, so a scale
     * change is instant and reversible. A step with no stored note resolves
     * live from its pad. */
    uint8_t channel = s_seq_lane_channel[lane];
    /* Fire every note stored for the step (count clamped against corrupt
     * flash data); otherwise one live-resolved note. */
    uint8_t count;
    if (pat->step_pitch_override[step]) {
        count = pat->step_note_count[step];
        if (count > OP_SEQ_MAX_NOTES_PER_STEP) {
            count = OP_SEQ_MAX_NOTES_PER_STEP;
        }
        for (uint8_t i = 0; i < count; i++) {
            uint8_t note = tiles_note_map_quantize_to_scale(pat->step_notes[step][i]);
            tiles_midi_note_on(channel, note, OP_SEQ_VELOCITY);
            tiles_cv_gate_note_on(note, OP_SEQ_VELOCITY);
            s_seq_sounding_notes[lane][i] = note;
        }
    } else {
        uint8_t note = tiles_note_map_get_note(pad);
        tiles_midi_note_on(channel, note, OP_SEQ_VELOCITY);
        tiles_cv_gate_note_on(note, OP_SEQ_VELOCITY);
        s_seq_sounding_notes[lane][0] = note;
        count = 1u;
    }
    /* Haptics only for the lane on screen; the MIDI always plays. One kick
     * per step. */
    s_seq_sounding_haptics[lane] = seq_lane_haptics_visible(lane);
    if (s_seq_sounding_haptics[lane]) {
        tiles_haptics_trigger_kick(pad, OP_SEQ_VELOCITY);
    }
    s_seq_note_sounding[lane] = true;
    s_seq_sounding_pad[lane] = pad;
    s_seq_sounding_channel[lane] = channel;
    s_seq_sounding_note_count[lane] = count;
}

/* Rolls the step's probability once per occurrence (a step fires or not,
 * like "trig probability" on hardware sequencers) and, if it fires, arms
 * its ratchet hits for seq_advance_clock(). */
static void seq_enter_step(uint8_t lane, uint8_t step) {
    seq_end_current_note(lane);
    s_seq_current_step[lane] = step;
    s_seq_ratchet_remaining[lane] = 0u;
    op_seq_pattern_t *pat = pattern_for_lane(lane);
    if (!pat->step_armed[step]) {
        return;
    }
    if (pat->probability_enabled && (uint32_t)(rand() % 100) >= pat->step_probability_percent[step]) {
        /* Skipped this time: the cursor moves, nothing sounds. */
        return;
    }
    uint8_t ratchet_total = pat->step_ratchet_count[step];
    if (ratchet_total < 1u) {
        ratchet_total = 1u;
    }
    seq_fire_note(lane, step);
    if (ratchet_total > 1u) {
        s_seq_ratchet_remaining[lane] = (uint8_t)(ratchet_total - 1u);
        s_seq_ratchet_interval_pulses[lane] = OP_SEQ_CLOCKS_PER_STEP / ratchet_total;
        if (s_seq_ratchet_interval_pulses[lane] < 1u) {
            s_seq_ratchet_interval_pulses[lane] = 1u;
        }
        s_seq_next_ratchet_pulse[lane] = s_seq_step_started_at_pulse[lane] + s_seq_ratchet_interval_pulses[lane];
    }
}

static void seq_reset(uint8_t lane, uint32_t now_pulse) {
    s_seq_step_started_at_pulse[lane] = now_pulse;
    s_seq_pending_start[lane] = false;
    seq_enter_step(lane, 0u);
}

/* Resume on the step the playhead was parked on (a fresh occurrence: new
 * probability roll and ratchet), instead of step 0. */
static void seq_resume_current_step(uint8_t lane, uint32_t now_pulse) {
    s_seq_step_started_at_pulse[lane] = now_pulse;
    seq_enter_step(lane, s_seq_current_step[lane]);
}

/* Called on (re)entering sequencer mode. Never clears patterns (only boot
 * does). Arms a quantized start so entering mid-phrase lands on the beat. */
static void seq_start(void) {
    /* The sequencer keeps running in the background, so only reset the viewed
     * lane's transport if that lane is stopped; otherwise just glancing back
     * at the sequencer would restart it. */
    if (!s_seq_lane_running[s_seq_edit_lane]) {
        s_seq_current_step[s_seq_edit_lane] = 0u;
        s_seq_note_sounding[s_seq_edit_lane] = false;
        s_seq_step_started_at_pulse[s_seq_edit_lane] = 0u;
        s_seq_pending_start[s_seq_edit_lane] = true;
        s_seq_pending_restart[s_seq_edit_lane] = true; /* fresh entry starts from step 0 */
    }
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    /* seq_pad_for_step(), not i + 1 (these arrays are step-indexed). */
    op_seq_pattern_t *layout_pat = active_pattern();
    for (uint8_t step = 0; step < OP_SEQ_NUM_STEPS; step++) {
        s_seq_prev_pad_touched[step] = tiles_touch_is_touched(seq_pad_for_step(layout_pat, step));
        s_seq_step_touch_started_ms[step] = 0u;
    }
}

/* Tap = toggle armed (on release); hold past OP_SEQ_PITCH_ASSIGN_HOLD_MS
 * = per-step edit; touch with circle already held = ratchet edit. */
static void seq_handle_step_taps(uint32_t now_ms) {
    op_seq_pattern_t *pat = active_pattern();
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t step;
        /* In the 4x4 layout the two right-hand columns aren't steps: skip them. */
        if (!seq_step_for_pad(pat, pad, &step)) {
            continue;
        }
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_seq_prev_pad_touched[step];
        if (touched && !was_touched) {
            if (tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID)) {
                edit_enter_ratchet(step);
                return; /* grid ownership changed under this loop: stop */
            }
            s_seq_step_touch_started_ms[step] = now_ms;
        } else if (touched) {
            if (s_seq_step_touch_started_ms[step] != 0u &&
                (now_ms - s_seq_step_touch_started_ms[step]) >= OP_SEQ_PITCH_ASSIGN_HOLD_MS) {
                edit_enter(step, s_seq_step_touch_started_ms[step]);
                return; /* grid ownership changed under this loop: stop */
            }
        } else {
            /* Released: a real tap commits the arm toggle here, not on press. A
             * started time of 0 means this loop never tracked the touch (e.g. the pad
             * that just committed a pitch edit), so it doesn't toggle anything. */
            if (s_seq_step_touch_started_ms[step] != 0u) {
                pat->step_armed[step] = !pat->step_armed[step];
                /* Only give a step a note the first time it's ever armed. A step toggled
                 * off and back on keeps the pitch it had (hold the step to change it). */
                if (pat->step_armed[step] && !pat->step_pitch_override[step]) {
                    /* Freeze the note at arm time (pitch override on), so later octave or
                     * key changes elsewhere don't move programmed steps. Playback still fits
                     * it to the current scale (seq_fire_note()). */
                    /* One note: a tap is a toggle; chords come from capture. */
                    pat->step_notes[step][0] = tiles_note_map_get_note(pad);
                    pat->step_note_count[step] = 1u;
                    pat->step_pitch_override[step] = true;
                }
            }
            s_seq_step_touch_started_ms[step] = 0u;
        }
        s_seq_prev_pad_touched[step] = touched;
    }
}

/* Takes the clock snapshot as a parameter: tiles_midi_clock_get_state()
 * clears start_edge, and scan also needs it for the beat flash. Called
 * once per lane with the same snapshot; all lanes share one tempo and
 * transport, each tracking its own phase. */
static void seq_advance_clock(uint8_t lane, tiles_midi_clock_state_t clock) {
    /* Stopped lanes first, before start_edge: a stopped lane must not fire
     * step 0 because another lane (or an external Start) started. Starting it
     * later with "+" arms its pending start. */
    if (!s_seq_lane_running[lane]) {
        seq_end_current_note(lane);
        return;
    }

    if (clock.start_edge) {
        seq_reset(lane, clock.pulse_count);
        return;
    }

    if (!clock.running) {
        /* The shared tempo stopped (e.g. external Stop): silence this lane but
         * keep its running flag, so it resumes with the tempo, like a slaved
         * hardware sequencer remembering which tracks were on. */
        seq_end_current_note(lane);
        return;
    }

    if (s_seq_pending_start[lane]) {
        /* Pending (quantized) start: snap to the NEAREST beat. In the first half
         * of a beat, start now (snapped to the beat just passed); past halfway,
         * wait for the next one. (Waiting for an exact boundary meant up to a
         * beat of dead air.) A fresh Start/tap-tempo start is already at phase 0
         * and took the branch above. s_seq_pending_restart decides restart
         * (step 0) vs. resume. */
        uint32_t phase_in_beat = clock.pulse_count % OP_CLOCK_PULSES_PER_BEAT;
        if (phase_in_beat != 0u && (phase_in_beat * 2u) < OP_CLOCK_PULSES_PER_BEAT) {
            return;
        }
        s_seq_pending_start[lane] = false;
        if (s_seq_pending_restart[lane]) {
            seq_reset(lane, clock.pulse_count);
        } else {
            seq_resume_current_step(lane, clock.pulse_count);
        }
        return;
    }

    /* Fire ratchet hits due within the current step BEFORE the step-boundary
     * check, so a hit at the edge isn't skipped. Bounded loop. */
    uint32_t ratchet_guard = 0u;
    while (s_seq_ratchet_remaining[lane] > 0u && clock.pulse_count >= s_seq_next_ratchet_pulse[lane] &&
           ratchet_guard < OP_SEQ_MAX_RATCHET) {
        seq_fire_note(lane, s_seq_current_step[lane]);
        s_seq_ratchet_remaining[lane]--;
        s_seq_next_ratchet_pulse[lane] += s_seq_ratchet_interval_pulses[lane];
        ratchet_guard++;
    }

    uint32_t elapsed = clock.pulse_count - s_seq_step_started_at_pulse[lane];
    if (elapsed < OP_SEQ_CLOCKS_PER_STEP) {
        return;
    }
    /* Handles several steps' worth of pulses between scans. */
    uint32_t steps_to_advance = elapsed / OP_SEQ_CLOCKS_PER_STEP;
    s_seq_step_started_at_pulse[lane] += steps_to_advance * OP_SEQ_CLOCKS_PER_STEP;
    uint8_t length = pattern_for_lane(lane)->length;
    uint8_t new_step = (uint8_t)((s_seq_current_step[lane] + steps_to_advance) % length);
    seq_enter_step(lane, new_step);
}

static void render_sequencer(float beat_flash_level, bool transport_running) {
    op_seq_pattern_t *pat = active_pattern();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    bool length_flashing = (now_ms - s_seq_length_flash_ms) < OP_SEQ_LENGTH_FLASH_DURATION_MS;
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t step;
            /* 4x4 layout: the two right-hand columns are dark, not steps. */
            if (!seq_step_for_pad(pat, pad, &step)) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
                continue;
            }
            bool is_current = (step == s_seq_current_step[s_seq_edit_lane]);
            if (length_flashing) {
                /* Length-change flash replaces the normal step colors briefly. */
                if (step < pat->length) {
                    tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R, OP_MENU_MELODIC_G, OP_MENU_MELODIC_B);
                } else {
                    tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
                }
                continue;
            }
            if (step >= pat->length) {
                /* Outside the loop length: off. */
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            } else if (is_current && pat->step_armed[step]) {
                /* Playhead on an armed step: blue, sounding or not (see
                 * OP_SEQ_CURSOR_ARMED_*). */
                tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_CURSOR_ARMED_R, OP_SEQ_CURSOR_ARMED_G, OP_SEQ_CURSOR_ARMED_B);
            } else if (is_current) {
                /* Playhead on an unarmed step (or paused / waiting to start): dim white. */
                tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_CURSOR_LEVEL, OP_SEQ_CURSOR_LEVEL, OP_SEQ_CURSOR_LEVEL);
            } else if (!pat->step_armed[step]) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            } else {
                /* Armed steps are dim red, tinted toward amber if probability may skip
                 * them (only while probability_enabled) and toward blue if they ratchet. */
                float green = 0.0f;
                float blue = 0.0f;
                if (pat->probability_enabled && pat->step_probability_percent[step] < 100u) {
                    green = OP_SEQ_DIM_RED_LEVEL * (float)(100u - pat->step_probability_percent[step]) / 100.0f;
                }
                if (pat->step_ratchet_count[step] > 1u) {
                    blue = OP_SEQ_DIM_RED_LEVEL * (float)(pat->step_ratchet_count[step] - 1u) / (float)(OP_SEQ_MAX_RATCHET - 1u);
                }
                tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_DIM_RED_LEVEL, green, blue);
            }
        }
    }
    /* Button row in the step view. Diamond isn't drawn here; its LED is
     * handle_diamond_transport()'s override. */
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_CIRCLE_BUTTON_COL) {
            /* circle flashes on the beat */
            level = beat_flash_level;
        } else if (col == TILES_MINUS_BUTTON_COL || col == TILES_PLUS_BUTTON_COL) {
            /* "-"/"+" LEDs for the viewed lane: "+" solid while it plays, pulsing
             * while the clock runs but this lane doesn't, off otherwise. "-" (only
             * while stopped): solid when rewound to step 0, pulsing when paused
             * mid-pattern (a second "-" rewinds). */
            bool lane_running = s_seq_lane_running[s_seq_edit_lane];
            if (col == TILES_MINUS_BUTTON_COL) {
                if (!lane_running) {
                    if (s_seq_current_step[s_seq_edit_lane] == 0u) {
                        level = OP_TRANSPORT_LED_LEVEL;
                    } else {
                        float phase = (float)now_ms / OP_TRANSPORT_RECORDING_PULSE_PERIOD_MS;
                        float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
                        level = OP_TRANSPORT_RECORDING_PULSE_MIN +
                                (OP_TRANSPORT_RECORDING_PULSE_MAX - OP_TRANSPORT_RECORDING_PULSE_MIN) * raw;
                    }
                }
            } else {
                if (transport_running && lane_running) {
                    level = OP_TRANSPORT_LED_LEVEL;
                } else if (transport_running && !lane_running) {
                    level = background_pattern_pulse_level(now_ms);
                }
            }
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Per-step editing ------------------------------------------------------
 * (Design in the state section above.) Opening an edit silences the
 * lane's current note; playback keeps advancing underneath and the view
 * catches up on return. */
static void edit_enter(uint8_t step, uint32_t started_ms) {
    seq_end_current_note(s_seq_edit_lane);
    s_seq_edit_mode = OP_SEQ_EDIT_PITCH;
    s_seq_edit_step = step;
    s_seq_edit_started_ms = started_ms;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pitch_edit_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
}

/* Ratchet's own entry (circle held, then touch the step): no hold wait. */
static void edit_enter_ratchet(uint8_t step) {
    seq_end_current_note(s_seq_edit_lane);
    s_seq_edit_mode = OP_SEQ_EDIT_RATCHET;
    s_seq_edit_step = step;
}

static void edit_exit(void) {
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    /* Resync step-tap tracking so a pad still touched when the edit closes
     * isn't read as a new tap. The arrays are indexed by STEP, hence
     * seq_pad_for_step() (correct in the 4x4 layout too). */
    op_seq_pattern_t *layout_pat = active_pattern();
    for (uint8_t step = 0; step < OP_SEQ_NUM_STEPS; step++) {
        s_seq_prev_pad_touched[step] = tiles_touch_is_touched(seq_pad_for_step(layout_pat, step));
        s_seq_step_touch_started_ms[step] = 0u;
    }
}

static uint8_t probability_percent_from_depth(uint16_t depth) {
    /* ~900 = full-scale depth (as OP_MENU_SELECT_DEPTH_THRESHOLD). */
    float fraction = (float)depth / 900.0f;
    if (fraction < 0.0f) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    return (uint8_t)(fraction * 100.0f + 0.5f);
}

static uint8_t ratchet_count_from_depth(uint16_t depth) {
    float fraction = (float)depth / 900.0f;
    if (fraction < 0.0f) {
        fraction = 0.0f;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    uint8_t count = (uint8_t)(1u + (uint32_t)(fraction * (float)(OP_SEQ_MAX_RATCHET - 1u) + 0.5f));
    if (count > OP_SEQ_MAX_RATCHET) {
        count = OP_SEQ_MAX_RATCHET;
    }
    return count;
}

/* Pitch: once open it stays open (the held pad can be released); the
 * first pad touched, the step's own included, becomes the step's single
 * note and the edit closes. Triangle + circle backs out unchanged.
 * Probability/ratchet: a live dial on the held pad's depth (see the
 * *_from_depth() helpers); release keeps the last value. */
static void handle_edit_mode(uint32_t now_ms) {
    /* The pad the step shows on (seq_pad_for_step(), 4x4-aware), so release
     * is detected on the right sensor. */
    uint8_t edit_pad = seq_pad_for_step(active_pattern(), s_seq_edit_step);
    bool edit_pad_touched = tiles_touch_is_touched(edit_pad);

    if (s_seq_edit_mode == OP_SEQ_EDIT_PITCH) {
        if (edit_pad_touched && (now_ms - s_seq_edit_started_ms) >= OP_SEQ_PROBABILITY_HOLD_MS) {
            /* Held long enough: escalate from pitch to probability. */
            s_seq_edit_mode = OP_SEQ_EDIT_PROBABILITY;
            /* Editing a probability turns the pattern's probability switch on;
             * otherwise the dialed value would never be used. */
            active_pattern()->probability_enabled = true;
            return;
        }
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            bool touched = tiles_touch_is_touched(pad);
            bool was_touched = s_pitch_edit_prev_pad_touched[pad - 1u];
            if (touched && !was_touched) {
                /* The first pad touched commits its note as the step's only note. */
                op_seq_pattern_t *pat = active_pattern();
                pat->step_notes[s_seq_edit_step][0] = tiles_note_map_get_note(pad);
                pat->step_note_count[s_seq_edit_step] = 1u;
                pat->step_pitch_override[s_seq_edit_step] = true;
                tiles_haptics_trigger_touch_pulse(pad);
                edit_exit();
                return; /* grid ownership changed under this loop: stop */
            }
            s_pitch_edit_prev_pad_touched[pad - 1u] = touched;
        }
        return;
    }

    if (s_seq_edit_mode == OP_SEQ_EDIT_PROBABILITY) {
        if (!edit_pad_touched) {
            edit_exit();
            return;
        }
        uint16_t depth = tiles_hall_get_depth(edit_pad);
        if (depth >= OP_SEQ_EDIT_RELEASE_GUARD_DEPTH) {
            /* Live up and down above the guard (see
             * OP_SEQ_EDIT_RELEASE_GUARD_DEPTH). */
            active_pattern()->step_probability_percent[s_seq_edit_step] = probability_percent_from_depth(depth);
        }
        return;
    }

    if (s_seq_edit_mode == OP_SEQ_EDIT_RATCHET) {
        if (!edit_pad_touched) {
            edit_exit();
            return;
        }
        uint16_t depth = tiles_hall_get_depth(edit_pad);
        if (depth >= OP_SEQ_EDIT_RELEASE_GUARD_DEPTH) {
            active_pattern()->step_ratchet_count[s_seq_edit_step] = ratchet_count_from_depth(depth);
        }
        return;
    }
}

/* The step view's "-"/"+" LED logic (see render_sequencer()), for the
 * edit views; duplicated because render_sequencer() also draws the beat
 * flash in the same loop. */
static void render_transport_toggle_leds(uint32_t now_ms, bool transport_running) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_MINUS_BUTTON_COL) {
            bool lane_running = s_seq_lane_running[s_seq_edit_lane];
            if (!lane_running) {
                if (s_seq_current_step[s_seq_edit_lane] == 0u) {
                    level = OP_TRANSPORT_LED_LEVEL;
                } else {
                    float phase = (float)now_ms / OP_TRANSPORT_RECORDING_PULSE_PERIOD_MS;
                    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
                    level = OP_TRANSPORT_RECORDING_PULSE_MIN +
                            (OP_TRANSPORT_RECORDING_PULSE_MAX - OP_TRANSPORT_RECORDING_PULSE_MIN) * raw;
                }
            }
        } else if (col == TILES_PLUS_BUTTON_COL) {
            bool lane_running = s_seq_lane_running[s_seq_edit_lane];
            if (transport_running && lane_running) {
                level = OP_TRANSPORT_LED_LEVEL;
            } else if (transport_running && !lane_running) {
                level = background_pattern_pulse_level(now_ms);
            }
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
}

/* Pitch-pick view: melodic idle coloring (root magenta, naturals white,
 * sharps off) so it reads like playing melodic mode; the step's current
 * note(s) pulse white. */
static void render_pitch_edit(uint32_t now_ms, bool transport_running) {
    float pulse = menu_selected_pulse_level(now_ms);
    op_seq_pattern_t *pat = active_pattern();
    /* Highlights every note stored on the step; a step with none previews its
     * live fallback note. */
    uint8_t cluster_count = pat->step_pitch_override[s_seq_edit_step] ? pat->step_note_count[s_seq_edit_step] : 1u;
    uint8_t fallback_note = tiles_note_map_get_note(seq_pad_for_step(pat, s_seq_edit_step));

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t note = tiles_note_map_get_note(pad);
        bool in_cluster = false;
        if (pat->step_pitch_override[s_seq_edit_step]) {
            for (uint8_t i = 0; i < cluster_count; i++) {
                if (pat->step_notes[s_seq_edit_step][i] == note) {
                    in_cluster = true;
                    break;
                }
            }
        } else {
            in_cluster = (note == fallback_note);
        }
        if (in_cluster) {
            tiles_lighting_set_standby_pad_rgb(pad, pulse, pulse, pulse);
        } else if (tiles_note_map_is_root_pad(pad)) {
            tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R * OP_SCALE_AVAILABLE_LEVEL,
                                                OP_MENU_MELODIC_G * OP_SCALE_AVAILABLE_LEVEL,
                                                OP_MENU_MELODIC_B * OP_SCALE_AVAILABLE_LEVEL);
        } else if (tiles_note_map_is_natural_pad(pad)) {
            tiles_lighting_set_standby_pad_rgb(pad, OP_SCALE_AVAILABLE_LEVEL, OP_SCALE_AVAILABLE_LEVEL, OP_SCALE_AVAILABLE_LEVEL);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    render_transport_toggle_leds(now_ms, transport_running);
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* A meter across all 24 pads: the lit count follows the live value. */
static void render_value_meter(uint32_t now_ms, uint8_t lit_count, float r, float g, float b, bool transport_running) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (pad <= lit_count) {
            tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    render_transport_toggle_leds(now_ms, transport_running);
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void render_edit_mode(uint32_t now_ms, bool transport_running) {
    switch (s_seq_edit_mode) {
    case OP_SEQ_EDIT_PITCH: {
        render_pitch_edit(now_ms, transport_running);
        break;
    }
    case OP_SEQ_EDIT_PROBABILITY: {
        uint8_t percent = active_pattern()->step_probability_percent[s_seq_edit_step];
        uint8_t lit = (uint8_t)((uint32_t)percent * TILES_NUM_PADS / 100u);
        render_value_meter(now_ms, lit, 1.0f, 0.8f, 0.0f, transport_running); /* amber */
        break;
    }
    case OP_SEQ_EDIT_RATCHET: {
        uint8_t count = active_pattern()->step_ratchet_count[s_seq_edit_step];
        uint8_t lit = (uint8_t)((uint32_t)count * TILES_NUM_PADS / OP_SEQ_MAX_RATCHET);
        render_value_meter(now_ms, lit, 0.0f, 0.4f, 1.0f, transport_running); /* blue */
        break;
    }
    default:
        break;
    }
}

/* Forward declaration: mode_owns_standby_grid() reads it directly. Defined
 * in "Song mode: capture". */
static bool s_song_capture_active;
/* Same, for Song's step-edit pitch pick (defined in "Song mode: step-edit
 * screen"). */
static bool s_song_edit_pick_active;

/* Which modes draw the whole grid through the standby setters (which do
 * nothing unless standby is claimed): sequencer, Ableton mode, and Song
 * mode, except while Song captures or picks a step's pitch, when the grid
 * shows live melodic coloring and plays live. The single place that
 * decides this: menu_exit(), scale_menu_exit(), set_active_mode() and
 * tiles_op_mode_owns_pad_grid() all use it. */
static bool mode_owns_standby_grid(tiles_op_mode_t mode) {
    if (mode == OP_MODE_SONG) {
        return !s_song_capture_active && !s_song_edit_pick_active;
    }
    /* Ableton mode's grid is all launch pads; no live-play exception. */
    return mode == OP_MODE_SEQUENCER || mode == OP_MODE_SCENE_LAUNCH;
}

/* ---- Mode menu ------------------------------------------------------------ */

static void menu_enter(void) {
    s_menu_visible = true;
    /* Defensive: never inherit a stale pending selection (a forced close
     * elsewhere could leave one set and block every later pick). */
    s_menu_pending = false;
    /* The mode menu replaces an open scale picker. */
    s_scale_menu_visible = false;
    s_scale_menu_pending_exit = false;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void menu_exit(void) {
    s_menu_visible = false;
    if (!mode_owns_standby_grid(s_active_mode)) {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    /* Triangle's LED has a permanent override, so buttons.c's refresh skips
     * it and nothing turns it off after cancelling the menu; do it here. The
     * menu is closed back to the current mode, where triangle is off. */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
}

/* Only available modes light up and respond (all six are available now). */
static bool col_is_available(uint8_t col) {
    switch (col) {
    case OP_MENU_COL_MELODIC:
    case OP_MENU_COL_SEQUENCER:
    case OP_MENU_COL_GUITAR:
    case OP_MENU_COL_CHORD:
    case OP_MENU_COL_SONG:
    case OP_MENU_COL_SCENE_LAUNCH:
        return true;
    default: /* outside the menu */
        return false;
    }
}

static void render_menu_col_color(uint8_t col, float *r, float *g, float *b) {
    switch (col) {
    case OP_MENU_COL_MELODIC:
        *r = OP_MENU_MELODIC_R;
        *g = OP_MENU_MELODIC_G;
        *b = OP_MENU_MELODIC_B;
        break;
    case OP_MENU_COL_SEQUENCER:
        *r = OP_MENU_SEQUENCER_R;
        *g = OP_MENU_SEQUENCER_G;
        *b = OP_MENU_SEQUENCER_B;
        break;
    case OP_MENU_COL_CHORD:
        *r = OP_MENU_CHORD_R;
        *g = OP_MENU_CHORD_G;
        *b = OP_MENU_CHORD_B;
        break;
    case OP_MENU_COL_SONG:
        *r = OP_MENU_SONG_R;
        *g = OP_MENU_SONG_G;
        *b = OP_MENU_SONG_B;
        break;
    case OP_MENU_COL_SCENE_LAUNCH:
        *r = OP_MENU_SCENE_LAUNCH_R;
        *g = OP_MENU_SCENE_LAUNCH_G;
        *b = OP_MENU_SCENE_LAUNCH_B;
        break;
    default: /* only OP_MENU_COL_GUITAR is left (only called for available columns) */
        *r = OP_MENU_GUITAR_R;
        *g = OP_MENU_GUITAR_G;
        *b = OP_MENU_GUITAR_B;
        break;
    }
}

/* Column -> "is this the current mode", for the pulsing slot. Every case
 * explicit (called for every column, including non-menu ones). Also true
 * for a pending pick, so the chosen slot pulses while the finger is still
 * down. */
static bool col_is_current_mode(uint8_t col) {
    tiles_op_mode_t mode = s_menu_pending ? s_menu_pending_mode : s_active_mode;
    switch (col) {
    case OP_MENU_COL_MELODIC:
        return mode == OP_MODE_MELODIC;
    case OP_MENU_COL_SEQUENCER:
        return mode == OP_MODE_SEQUENCER;
    case OP_MENU_COL_GUITAR:
        return mode == OP_MODE_GUITAR;
    case OP_MENU_COL_CHORD:
        return mode == OP_MODE_CHORD;
    case OP_MENU_COL_SONG:
        return mode == OP_MODE_SONG;
    case OP_MENU_COL_SCENE_LAUNCH:
        return mode == OP_MODE_SCENE_LAUNCH;
    default:
        return false;
    }
}

/* Current mode pulses white; other available modes show their color at
 * the dim "available" level; everything else is off. */
static void render_menu(uint32_t now_ms) {
    float pulse = menu_selected_pulse_level(now_ms);
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (row == OP_MENU_ROW) {
                if (col_is_current_mode(col)) {
                    r = g = b = pulse;
                } else if (col_is_available(col)) {
                    render_menu_col_color(col, &r, &g, &b);
                    r *= OP_SCALE_AVAILABLE_LEVEL;
                    g *= OP_SCALE_AVAILABLE_LEVEL;
                    b *= OP_SCALE_AVAILABLE_LEVEL;
                }
            }
            tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), r, g, b);
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = (col == TILES_TRIANGLE_BUTTON_COL) ? OP_TRIANGLE_LED_MENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Scale picker ----------------------------------------------------------
 * Push-style: one pad per scale across the grid (note_map.h picker slots).
 * Selected = pulsing white, available = dim magenta, reserved custom slots
 * = off and not selectable. */
/* The selected pad also gets a haptic tick every
 * OP_MENU_SELECTED_PULSE_PERIOD_MS, in time with its light. */
static uint32_t s_scale_menu_haptic_pulse_ms;

static void render_scale_menu(uint32_t now_ms) {
    float pulse = menu_selected_pulse_level(now_ms);
    tiles_scale_mode_t current = tiles_note_map_get_scale();
    uint8_t selected_pad = 0u;

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        tiles_scale_mode_t slot_scale = tiles_note_map_scale_for_grid_slot(pad);
        if (!tiles_note_map_scale_is_defined(slot_scale)) {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        } else if (slot_scale == current) {
            tiles_lighting_set_standby_pad_rgb(pad, pulse, pulse, pulse);
            selected_pad = pad;
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R * OP_SCALE_AVAILABLE_LEVEL,
                                                OP_MENU_MELODIC_G * OP_SCALE_AVAILABLE_LEVEL,
                                                OP_MENU_MELODIC_B * OP_SCALE_AVAILABLE_LEVEL);
        }
    }
    if (selected_pad != 0u && (now_ms - s_scale_menu_haptic_pulse_ms) >= (uint32_t)OP_MENU_SELECTED_PULSE_PERIOD_MS) {
        s_scale_menu_haptic_pulse_ms = now_ms;
        tiles_haptics_trigger_touch_pulse(selected_pad);
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        /* Triangle lit while the picker is open. */
        float level = (col == TILES_TRIANGLE_BUTTON_COL) ? OP_TRIANGLE_LED_MENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void scale_menu_exit(void);

/* A touch only clicks (haptic); selecting needs depth past
 * OP_MENU_SELECT_DEPTH_THRESHOLD. The scale changes at once (no MIDI side
 * effect), but the picker only closes once every pad is released
 * (s_scale_menu_pending_exit): closing while the finger was still down
 * handed it to real play and fired a note. */
static void handle_scale_menu_taps(void) {
    bool any_touched = false;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        if (touched) {
            any_touched = true;
        }
        if (touched && !s_scale_menu_prev_pad_touched[pad - 1u]) {
            tiles_haptics_trigger_touch_pulse(pad);
        }
        if (!s_scale_menu_pending_exit && touched &&
            (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD) {
            tiles_scale_mode_t slot_scale = tiles_note_map_scale_for_grid_slot(pad);
            if (tiles_note_map_scale_is_defined(slot_scale)) {
                if (slot_scale != tiles_note_map_get_scale()) {
                    printf("[op_mode] scale -> %d\n", (int)slot_scale);
                }
                tiles_note_map_set_scale(slot_scale);
                s_scale_menu_pending_exit = true;
            }
            /* Reserved slots are not selectable. */
        }
        s_scale_menu_prev_pad_touched[pad - 1u] = touched;
    }
    if (s_scale_menu_pending_exit && !any_touched) {
        s_scale_menu_pending_exit = false;
        scale_menu_exit();
    }
}

/* One global scale for melodic, chord and every sequencer pattern
 * (per-pattern scales were tried and dropped). Stored patterns are never
 * rewritten; seq_fire_note() fits them to the scale as they play. */
static void scale_menu_enter(void) {
    s_scale_menu_visible = true;
    s_scale_menu_pending_exit = false; /* defensive; see menu_enter() */
    s_scale_menu_haptic_pulse_ms = to_ms_since_boot(get_absolute_time());
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_scale_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void scale_menu_exit(void) {
    s_scale_menu_visible = false;
    /* Keep standby claimed if the current mode draws through it (sequencer,
     * Ableton, Song; see mode_owns_standby_grid()); releasing it broke the
     * step view when the picker closed. */
    if (!mode_owns_standby_grid(s_active_mode)) {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    /* Triangle's override LED isn't repainted by buttons.c: turn it off
     * explicitly (it stayed lit after choosing a scale). */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
}

/* ---- Pattern persistence (flash) -------------------------------------------
 * Saving and deleting happen in the pattern bank (handle_pattern_bank_
 * taps(): circle + pattern = save, hold 3 s = delete); this is the flash
 * layer underneath.
 *
 * Everything (which slots are saved + all 24 patterns) fits one 4 KB
 * sector (~2.9 KB used), the last sector of flash (storage/flash_map.h).
 * A magic number and version reject erased flash (all 0xFF) and old
 * layouts.
 *
 * Code runs from flash (XIP), so erase/program need interrupts off for
 * the whole span (the SDK doesn't do it); pattern_store_write_all() does
 * that (single core). The watchdog is fed before and after, not during
 * (watchdog_update() is in flash); a 4 KB erase + program finishes well
 * inside the timeout. A save pauses everything for tens of ms, which is
 * fine for an occasional deliberate action. */
#define TILES_PATTERN_STORE_MAGIC 0x454c4954u /* "TILE" */
/* Bumped for each layout change (multi-note steps, then the packed 4-note
 * format). A mismatch loads as "nothing saved": old patterns are lost,
 * never misread. */
#define TILES_PATTERN_STORE_VERSION 3u
#define TILES_PATTERN_FLASH_OFFSET TILES_FLASH_PATTERN_OFFSET /* storage/flash_map.h */

/* On-flash layout of ONE pattern, separate from op_seq_pattern_t (which
 * the rest of the file uses unchanged). Packing that makes 4 notes/step
 * fit the sector:
 *   - step_armed[] and step_pitch_override[] become 32-bit masks.
 *   - step_note_count[] is dropped: unused note slots hold 0xFF (never a
 *     MIDI note), so the count is the number of non-0xFF entries.
 * pack_pattern_to_flash()/unpack_pattern_from_flash() convert. */
typedef struct {
    uint32_t step_armed_mask;
    uint32_t step_pitch_override_mask;
    uint8_t step_notes[OP_SEQ_NUM_STEPS][OP_SEQ_MAX_NOTES_PER_STEP]; /* 0xFF = unused */
    uint8_t step_probability_percent[OP_SEQ_NUM_STEPS];
    uint8_t step_ratchet_count[OP_SEQ_NUM_STEPS];
    bool probability_enabled;
    uint8_t length;
} tiles_pattern_flash_t;

static void pack_pattern_to_flash(const op_seq_pattern_t *pat, tiles_pattern_flash_t *out) {
    out->step_armed_mask = 0u;
    out->step_pitch_override_mask = 0u;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if (pat->step_armed[i]) {
            out->step_armed_mask |= (uint32_t)1u << i;
        }
        if (pat->step_pitch_override[i]) {
            out->step_pitch_override_mask |= (uint32_t)1u << i;
        }
        uint8_t count = pat->step_note_count[i];
        for (uint8_t j = 0; j < OP_SEQ_MAX_NOTES_PER_STEP; j++) {
            out->step_notes[i][j] = (j < count) ? pat->step_notes[i][j] : 0xFFu;
        }
        out->step_probability_percent[i] = pat->step_probability_percent[i];
        out->step_ratchet_count[i] = pat->step_ratchet_count[i];
    }
    out->probability_enabled = pat->probability_enabled;
    out->length = pat->length;
}

static void unpack_pattern_from_flash(const tiles_pattern_flash_t *in, op_seq_pattern_t *pat) {
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        pat->step_armed[i] = (in->step_armed_mask & ((uint32_t)1u << i)) != 0u;
        pat->step_pitch_override[i] = (in->step_pitch_override_mask & ((uint32_t)1u << i)) != 0u;
        uint8_t count = 0u;
        for (uint8_t j = 0; j < OP_SEQ_MAX_NOTES_PER_STEP; j++) {
            uint8_t note = in->step_notes[i][j];
            if (note != 0xFFu) {
                pat->step_notes[i][count] = note;
                count++;
            }
        }
        pat->step_note_count[i] = count;
        pat->step_probability_percent[i] = in->step_probability_percent[i];
        pat->step_ratchet_count[i] = in->step_ratchet_count[i];
    }
    pat->probability_enabled = in->probability_enabled;
    pat->length = in->length;
}

typedef struct {
    uint32_t magic;
    uint32_t version;
    /* One bit per [lane][alt] slot (24 of 32 used), packed to save header
     * space in the sector; s_pattern_slot_saved stays a plain array at
     * runtime (see pattern_store_pack/_unpack_slot_saved()). */
    uint32_t slot_saved_mask;
    tiles_pattern_flash_t pattern[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
} tiles_pattern_store_t;

/* Must fit one flash sector. A compile error beats a silent memcpy past
 * the write buffer, which happened once while building this. */
_Static_assert(sizeof(tiles_pattern_store_t) <= FLASH_SECTOR_SIZE, "tiles_pattern_store_t no longer fits in one flash sector");

static uint32_t pattern_store_pack_slot_saved(void) {
    uint32_t mask = 0u;
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            if (s_pattern_slot_saved[lane][alt]) {
                mask |= (uint32_t)1u << (lane * OP_SEQ_ALTS_PER_LANE + alt);
            }
        }
    }
    return mask;
}

static void pattern_store_unpack_slot_saved(uint32_t mask) {
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            s_pattern_slot_saved[lane][alt] = (mask & ((uint32_t)1u << (lane * OP_SEQ_ALTS_PER_LANE + alt))) != 0u;
        }
    }
}

/* Rewrites the whole store from RAM every time: flash erases whole sectors
 * anyway, and RAM is the complete picture. Static buffer: 4 KB is too much
 * for the main loop's stack. */
static void pattern_store_write_all(void) {
    static uint8_t s_write_buf[FLASH_SECTOR_SIZE];
    memset(s_write_buf, 0, sizeof(s_write_buf));
    tiles_pattern_store_t *store = (tiles_pattern_store_t *)s_write_buf;
    store->magic = TILES_PATTERN_STORE_MAGIC;
    store->version = TILES_PATTERN_STORE_VERSION;
    store->slot_saved_mask = pattern_store_pack_slot_saved();
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            pack_pattern_to_flash(&s_seq_pattern[lane][alt], &store->pattern[lane][alt]);
        }
    }

    watchdog_update();
    uint32_t prev_interrupts = save_and_disable_interrupts();
    flash_range_erase(TILES_PATTERN_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TILES_PATTERN_FLASH_OFFSET, s_write_buf, sizeof(s_write_buf));
    restore_interrupts(prev_interrupts);
    watchdog_update();
}

/* Save/delete confirmation: the saved (green) or deleted (red) cell and
 * the underglow blink twice (see render_pattern_bank()). */
static bool s_pattern_flash_active;
static bool s_pattern_flash_is_delete; /* false = green (saved), true = red (deleted) */
static uint32_t s_pattern_flash_start_ms;
static uint8_t s_pattern_flash_pad; /* 1..24, the cell just saved/deleted */

static void pattern_store_save_slot(uint8_t lane, uint8_t alt) {
    s_pattern_slot_saved[lane][alt] = true;
    pattern_store_write_all();
    printf("[op_mode] saved lane %u pattern %u to flash\n", (unsigned)lane, (unsigned)alt);
    s_pattern_flash_active = true;
    s_pattern_flash_is_delete = false;
    s_pattern_flash_start_ms = to_ms_since_boot(get_absolute_time());
    s_pattern_flash_pad =
        board_pad_for_row_col((uint8_t)(lane + TILES_GRID_MIN_ROW + 1u), (uint8_t)(alt + TILES_GRID_MIN_COL));
}

static void pattern_store_clear_slot(uint8_t lane, uint8_t alt) {
    memset(&s_seq_pattern[lane][alt], 0, sizeof(op_seq_pattern_t));
    s_seq_pattern[lane][alt].length = OP_SEQ_NUM_STEPS;
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        s_seq_pattern[lane][alt].step_probability_percent[i] = 100u;
        s_seq_pattern[lane][alt].step_ratchet_count[i] = 1u;
    }
    s_pattern_slot_saved[lane][alt] = false;
    pattern_store_write_all();
    printf("[op_mode] cleared lane %u pattern %u\n", (unsigned)lane, (unsigned)alt);
    s_pattern_flash_active = true;
    s_pattern_flash_is_delete = true;
    s_pattern_flash_start_ms = to_ms_since_boot(get_absolute_time());
    s_pattern_flash_pad =
        board_pad_for_row_col((uint8_t)(lane + TILES_GRID_MIN_ROW + 1u), (uint8_t)(alt + TILES_GRID_MIN_COL));
}

/* Called once from tiles_op_mode_init(), after the defaults loop: only
 * overwrites slots the flash marks as saved. A plain memory-mapped read. */
static void pattern_store_load_all(void) {
    const tiles_pattern_store_t *store = (const tiles_pattern_store_t *)(XIP_BASE + TILES_PATTERN_FLASH_OFFSET);
    if (store->magic != TILES_PATTERN_STORE_MAGIC || store->version != TILES_PATTERN_STORE_VERSION) {
        return; /* never saved on this board, or a different layout version */
    }
    pattern_store_unpack_slot_saved(store->slot_saved_mask);
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            if (s_pattern_slot_saved[lane][alt]) {
                unpack_pattern_from_flash(&store->pattern[lane][alt], &s_seq_pattern[lane][alt]);
            }
        }
    }
    printf("[op_mode] loaded saved patterns from flash\n");
}

/* ---- Pattern bank (diamond, sequencer only) --------------------------------
 * Row = lane, column = one of that lane's 6 alternatives; all 24 cells are
 * selectable. Pressing a cell makes it that lane's PLAYING pattern
 * (s_seq_active_alt[lane]) and makes that lane the one shown in the step
 * view (s_seq_edit_lane), so one gesture picks both.
 * Cell colors: see render_pattern_bank(). */
#define OP_PATTERN_BANK_FLASH_MS 300u
/* Four distinct lane colors, avoiding red (selection) and magenta
 * (capture playhead and length flash). Unmeasured. */
#define OP_SEQ_LANE_0_R OP_SCALE_AVAILABLE_LEVEL
#define OP_SEQ_LANE_0_G (OP_SCALE_AVAILABLE_LEVEL * 0.5f)
#define OP_SEQ_LANE_0_B 0.0f
#define OP_SEQ_LANE_1_R 0.0f
#define OP_SEQ_LANE_1_G OP_SCALE_AVAILABLE_LEVEL
#define OP_SEQ_LANE_1_B 0.0f
#define OP_SEQ_LANE_2_R 0.0f
#define OP_SEQ_LANE_2_G (OP_SCALE_AVAILABLE_LEVEL * 0.6f)
#define OP_SEQ_LANE_2_B OP_SCALE_AVAILABLE_LEVEL
#define OP_SEQ_LANE_3_R (OP_SCALE_AVAILABLE_LEVEL * 0.7f)
#define OP_SEQ_LANE_3_G 0.0f
#define OP_SEQ_LANE_3_B OP_SCALE_AVAILABLE_LEVEL

static bool s_pattern_bank_visible;
static bool s_pattern_bank_prev_pad_touched[TILES_NUM_PADS];
/* Save/delete tracking. Whether circle was down is recorded at touch-down
 * (like every modifier here), so letting go of circle mid-hold can't turn
 * a save/delete into a plain select. */
static bool s_pattern_bank_touch_started_with_shift[TILES_NUM_PADS];
static uint32_t s_pattern_bank_touch_started_ms[TILES_NUM_PADS];
static bool s_pattern_bank_delete_fired[TILES_NUM_PADS];
/* Circle + hold a cell this long = delete. */
#define OP_PATTERN_DELETE_HOLD_MS 3000u

static void pattern_bank_exit(void);

static void lane_color(uint8_t lane, float *r, float *g, float *b) {
    switch (lane) {
    case 0u:
        *r = OP_SEQ_LANE_0_R;
        *g = OP_SEQ_LANE_0_G;
        *b = OP_SEQ_LANE_0_B;
        break;
    case 1u:
        *r = OP_SEQ_LANE_1_R;
        *g = OP_SEQ_LANE_1_G;
        *b = OP_SEQ_LANE_1_B;
        break;
    case 2u:
        *r = OP_SEQ_LANE_2_R;
        *g = OP_SEQ_LANE_2_G;
        *b = OP_SEQ_LANE_2_B;
        break;
    default:
        *r = OP_SEQ_LANE_3_R;
        *g = OP_SEQ_LANE_3_G;
        *b = OP_SEQ_LANE_3_B;
        break;
    }
}

static bool pattern_has_content(const op_seq_pattern_t *pat) {
    for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
        if (pat->step_armed[i]) {
            return true;
        }
    }
    return false;
}

/* Cell colors, most specific first:
 *   - the viewed lane's current pick: red flash (even if empty, so you see
 *     where you're about to record);
 *   - another lane's current pick while that lane plays: white flash;
 *   - any cell with content: its lane's dim color;
 *   - otherwise off. */
/* Save/delete confirmation: exactly two on/off blinks, then back to
 * normal. */
#define OP_PATTERN_FLASH_BLINK_MS 150u
#define OP_PATTERN_FLASH_COUNT 2u
#define OP_PATTERN_FLASH_TOTAL_MS (OP_PATTERN_FLASH_BLINK_MS * 2u * OP_PATTERN_FLASH_COUNT)

/* Underglow color for the save/delete confirmation, for
 * services/lighting.c, which ranks it above debug mode's pulse (that pulse
 * used to hide it). Read-only: expiry is cleared by
 * render_pattern_bank(), which runs whenever the bank is visible. */
bool tiles_op_mode_pattern_flash_underglow_color(float *out_r, float *out_g, float *out_b) {
    if (!s_pattern_flash_active) {
        return false;
    }
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now_ms - s_pattern_flash_start_ms;
    if (elapsed >= OP_PATTERN_FLASH_TOTAL_MS) {
        return false;
    }
    float level = ((elapsed / OP_PATTERN_FLASH_BLINK_MS) % 2u) == 0u ? 1.0f : 0.0f;
    if (s_pattern_flash_is_delete) {
        *out_r = level;
        *out_g = 0.0f;
    } else {
        *out_r = 0.0f;
        *out_g = level;
    }
    *out_b = 0.0f;
    return true;
}

static void render_pattern_bank(uint32_t now_ms) {
    bool flash_on = ((now_ms / OP_PATTERN_BANK_FLASH_MS) % 2u) == 0u;

    /* Fresh timestamp, not the scan's `now_ms`: that was taken BEFORE the
     * flash write in this scan, so now_ms - start underflowed and expired the
     * confirmation before it was ever drawn. */
    bool save_flash_showing = false;
    bool save_flash_on = false;
    if (s_pattern_flash_active) {
        uint32_t flash_now_ms = to_ms_since_boot(get_absolute_time());
        uint32_t elapsed = flash_now_ms - s_pattern_flash_start_ms;
        if (elapsed >= OP_PATTERN_FLASH_TOTAL_MS) {
            s_pattern_flash_active = false;
        } else {
            save_flash_showing = true;
            save_flash_on = ((elapsed / OP_PATTERN_FLASH_BLINK_MS) % 2u) == 0u;
        }
    }
    /* Log when the confirmation starts showing (rising edge only). */
    static bool s_debug_last_save_flash_showing = false;
    if (save_flash_showing && !s_debug_last_save_flash_showing) {
        printf("[op_mode] pattern flash showing: pad=%u is_delete=%u\n", (unsigned)s_pattern_flash_pad,
               (unsigned)s_pattern_flash_is_delete);
    }
    s_debug_last_save_flash_showing = save_flash_showing;

    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t lane = (uint8_t)(row - (TILES_GRID_MIN_ROW + 1u));
        float lr, lg, lb;
        lane_color(lane, &lr, &lg, &lb);
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t alt = (uint8_t)(col - TILES_GRID_MIN_COL);
            uint8_t pad = board_pad_for_row_col(row, col);
            bool is_active = (alt == s_seq_active_alt[lane]);
            if (save_flash_showing && pad == s_pattern_flash_pad) {
                /* The confirmation overrides the cell's normal color. */
                float level = save_flash_on ? 1.0f : 0.0f;
                if (s_pattern_flash_is_delete) {
                    tiles_lighting_set_standby_pad_rgb(pad, level, 0.0f, 0.0f);
                } else {
                    tiles_lighting_set_standby_pad_rgb(pad, 0.0f, level, 0.0f);
                }
            } else if (is_active && lane == s_seq_edit_lane) {
                float level = flash_on ? 1.0f : 0.0f;
                tiles_lighting_set_standby_pad_rgb(pad, level, 0.0f, 0.0f);
            } else if (is_active && s_seq_lane_running[lane]) {
                float level = flash_on ? 1.0f : 0.0f;
                tiles_lighting_set_standby_pad_rgb(pad, level, level, level);
            } else if (pattern_has_content(&s_seq_pattern[lane][alt])) {
                tiles_lighting_set_standby_pad_rgb(pad, lr, lg, lb);
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            }
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        /* Diamond lit (the button that opened the bank). */
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? OP_TRIANGLE_LED_MENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    /* Underglow: this loop only writes the idle (off) state. The confirmation
     * colors come from services/lighting.c's priority chain, the single
     * writer; writing them here too was overwritten every pass by debug
     * mode's pulse. */
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (!save_flash_showing) {
            tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
        }
    }
}

/* Touch clicks, pressing past half selects (like every picker here),
 * unless circle was already down at touch-down: then a quick touch saves
 * the slot to flash on release, and holding past OP_PATTERN_DELETE_HOLD_MS
 * deletes it (once per hold, s_pattern_bank_delete_fired). */
static void handle_pattern_bank_taps(uint32_t now_ms) {
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t lane = (uint8_t)(row - (TILES_GRID_MIN_ROW + 1u));
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t alt = (uint8_t)(col - TILES_GRID_MIN_COL);
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            bool was_touched = s_pattern_bank_prev_pad_touched[pad - 1u];
            if (touched && !was_touched) {
                tiles_haptics_trigger_touch_pulse(pad);
                s_pattern_bank_touch_started_with_shift[pad - 1u] = circle_held;
                s_pattern_bank_touch_started_ms[pad - 1u] = now_ms;
                s_pattern_bank_delete_fired[pad - 1u] = false;
            }
            if (s_pattern_bank_touch_started_with_shift[pad - 1u]) {
                if (touched) {
                    uint32_t held_ms = now_ms - s_pattern_bank_touch_started_ms[pad - 1u];
                    if (held_ms >= OP_PATTERN_DELETE_HOLD_MS && !s_pattern_bank_delete_fired[pad - 1u]) {
                        pattern_store_clear_slot(lane, alt);
                        s_pattern_bank_delete_fired[pad - 1u] = true;
                    }
                } else if (was_touched && !s_pattern_bank_delete_fired[pad - 1u]) {
                    pattern_store_save_slot(lane, alt);
                }
                s_pattern_bank_prev_pad_touched[pad - 1u] = touched;
                continue;
            }
            if (touched && (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD) {
                if (alt != s_seq_active_alt[lane]) {
                    seq_end_current_note(lane);
                    /* drop pending ratchet hits from the old pattern */
                    s_seq_ratchet_remaining[lane] = 0u;
                    s_seq_active_alt[lane] = alt;
                    /* Reset the playhead for the new pattern: a stopped lane never runs
                     * seq_advance_clock()'s normalization, so a later "+" would resume the
                     * new pattern at the old one's step. */
                    s_seq_current_step[lane] = 0u;
                    /* Quantize the switch to the next beat via the pending-start path (no
                     * extra get_state() call, which would eat start_edge). Inert while the
                     * lane is stopped. */
                    s_seq_pending_start[lane] = true;
                    s_seq_pending_restart[lane] = true;
                    printf("[op_mode] lane %u pattern -> %u\n", (unsigned)lane, (unsigned)alt);
                }
                s_seq_edit_lane = lane;
                pattern_bank_exit();
                return; /* grid ownership changed under this loop: stop */
            }
            s_pattern_bank_prev_pad_touched[pad - 1u] = touched;
        }
    }
}

static void pattern_bank_enter(void) {
    /* Close an open per-step edit first (it can sit open untouched);
     * otherwise it would reappear when the bank closes. */
    if (s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
        edit_exit();
    }
    /* Silence the viewed lane's note while the bank is open; the other lanes
     * keep playing. */
    seq_end_current_note(s_seq_edit_lane);
    s_pattern_bank_visible = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pattern_bank_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        /* Defensive reset of save/delete tracking. */
        s_pattern_bank_touch_started_with_shift[i] = false;
        s_pattern_bank_delete_fired[i] = false;
    }
}

static void pattern_bank_exit(void) {
    s_pattern_bank_visible = false;
    /* Resync step-tap tracking (step-indexed, 4x4-aware), so the finger that
     * picked a pattern isn't read as a step tap on it. */
    op_seq_pattern_t *layout_pat = active_pattern();
    for (uint8_t step = 0; step < OP_SEQ_NUM_STEPS; step++) {
        s_seq_prev_pad_touched[step] = tiles_touch_is_touched(seq_pad_for_step(layout_pat, step));
        s_seq_step_touch_started_ms[step] = 0u;
    }
    /* Keep standby claimed (the step view draws through it). Turn triangle's
     * override LED off explicitly, as in scale_menu_exit(). */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
}

/* Forward declarations for set_active_mode()'s safety checks; defined
 * with capture mode below. */
static bool s_seq_capture_mode_active;
static void seq_capture_mode_exit(void);
/* Forward declarations (defined in "Song mode: capture"): used by
 * set_active_mode() and handle_diamond_transport(). */
static void song_capture_enter(void);
static void song_capture_exit(void);
/* Forward declarations for the Song step-edit screen. */
static bool s_song_edit_active;
static void song_edit_exit(void);
static void song_edit_pick_cancel(void); /* used by handle_diamond_transport() */
static void scene_send_stop_all(void); /* used by handle_diamond_transport() */
static void scene_send_track_offset(uint8_t offset); /* used by handle_transport_and_length() */
static void scene_launch_enter(void); /* used by set_active_mode() */
static void scene_launch_leave(void); /* used by set_active_mode() */
static void song_stop_all_running(void); /* used by set_active_mode() */

/* Ableton armed a track and started recording into an empty clip slot
 * (scene_on_sysex() OPEN_MELODIC): switch to melodic once every pad is
 * released, so the finger that clicked the slot doesn't play a note.
 * Cleared by any mode change. */
static bool s_scene_pending_melodic;

/* True while recording a new Live clip from melodic mode (set via
 * s_scene_pending_melodic), until circle + diamond ends it
 * (scene_end_capture()) or the player goes to Ableton, sequencer or Song
 * mode. Meanwhile circle + diamond belongs to Ableton, not Song capture:
 * this flow is self-contained. */
static bool s_ableton_capture_active;
static void scene_end_capture(void); /* used by handle_diamond_transport() */

/* Ableton mode's track window, declared early for set_active_mode() and
 * handle_transport_and_length(): column c (OP_SCENE_TRACK_COL_MIN..MAX)
 * shows track s_scene_track_offset + c - OP_SCENE_TRACK_COL_MIN. */
#define OP_SCENE_MAX_TRACKS 64u
#define OP_SCENE_TRACK_COL_MIN 1u
#define OP_SCENE_TRACK_COL_MAX 5u
static uint8_t s_scene_track_offset;

static void set_active_mode(tiles_op_mode_t mode) {
    if (s_song_capture_active && mode != s_active_mode) {
        /* Any real mode change ends Song capture. */
        song_capture_exit();
    } else if (s_seq_capture_mode_active && mode != OP_MODE_SEQUENCER) {
        /* Leaving the sequencer (e.g. via the mode menu) ends sequencer capture. */
        seq_capture_mode_exit();
    }
    if (s_song_edit_active && mode != s_active_mode) {
        /* A separate `if`: the Song step-edit screen can be open under an active
         * Song capture, and must close on a mode change too. */
        song_edit_exit();
    }
    /* Leaving the sequencer does NOT stop it: patterns keep playing in the
     * background (seq_advance_clock() runs every scan). Chord notes do end
     * when leaving chord mode. */
    if (s_active_mode == OP_MODE_CHORD && mode != OP_MODE_CHORD) {
        chord_end_all_notes();
    }
    /* Ableton mode and Song mode can't both drive Live, so entering Ableton
     * mode from ANY mode stops Song's running tracks (they play in the
     * background). */
    if (mode == OP_MODE_SCENE_LAUNCH) {
        song_stop_all_running();
    }
    if (mode != OP_MODE_MELODIC) {
        /* The scale picker can't stay open in another mode; just close the view. */
        s_scale_menu_visible = false;
    }
    if (mode != OP_MODE_SEQUENCER) {
        /* Same for the sequencer's step edit and pattern bank. */
        s_seq_edit_mode = OP_SEQ_EDIT_NONE;
        s_pattern_bank_visible = false;
    }
    if (s_active_mode == OP_MODE_SCENE_LAUNCH && mode != OP_MODE_SCENE_LAUNCH) {
        scene_launch_leave();
    }
    s_active_mode = mode;
    if (mode == OP_MODE_SEQUENCER) {
        seq_start();
    }
    /* Modes that draw the whole grid claim standby (mode_owns_standby_grid()). */
    if (mode_owns_standby_grid(mode)) {
        tiles_lighting_set_standby_active(true);
        tiles_buttons_set_standby_active(true);
    } else {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    /* Bass guitar mode uses melodic's pipeline; this flag makes note_map play
     * fret notes and lighting draw fret markers. */
    tiles_note_map_set_guitar_mode(mode == OP_MODE_GUITAR);
    /* Chord mode's flag for note_map (which pads are chord pads). Seed touch
     * state from what's touched now, so a finger resting on a chord pad when
     * the mode starts doesn't fire a chord. */
    tiles_note_map_set_chord_mode(mode == OP_MODE_CHORD);
    if (mode == OP_MODE_CHORD) {
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            s_chord_pad_touched[pad - 1u] = tiles_touch_is_touched(pad);
        }
    }
    if (mode == OP_MODE_GUITAR) {
        /* Bass guitar mode takes over "-"/"+" without claiming standby, so turn
         * their LEDs off (octave_control.c repaints them when it gets them back). */
        tiles_buttons_set_override_led(TILES_MINUS_BUTTON_ID, 0.0f);
        tiles_buttons_set_override_led(TILES_PLUS_BUTTON_ID, 0.0f);
    }
    /* Triangle is lit only while the mode menu is open. */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
    s_scene_pending_melodic = false;
    if (mode == OP_MODE_SCENE_LAUNCH || mode == OP_MODE_SEQUENCER || mode == OP_MODE_SONG) {
        s_ableton_capture_active = false;
    }
    if (mode == OP_MODE_SCENE_LAUNCH) {
        scene_launch_enter();
    }
    printf("[op_mode] active mode -> %d\n", (int)mode);
}

/* ---- Sequencer capture (circle + diamond, in the sequencer) --------------
 * Records what you play into the viewed lane's steps, quantized to the
 * running tempo (needs tap tempo or external clock). The scale switches to
 * chromatic while capturing and is restored after. The current step's
 * pad lights magenta; diamond glows. Circle + diamond again ends it.
 *
 * Touches are played directly here (fixed OP_SEQ_VELOCITY) rather than
 * through expression.c: the sequencer still owns the grid. Overlap is
 * allowed: a new touch ends the previous note and becomes the note being
 * recorded. */
/* Capture enabled (it was disabled once over a freeze that turned out to
 * be unrelated printf flooding). */
#define OP_SEQ_CAPTURE_MODE_ENABLED 1
static tiles_scale_mode_t s_seq_capture_prev_scale;
static bool s_seq_capture_prev_pad_touched[TILES_NUM_PADS];
/* Chord pads are captured on their strike edge (s_chord_pad_sounding[]
 * turning true), not the touch edge; this tracks last scan's state. */
static bool s_seq_capture_prev_chord_sounding[TILES_NUM_PADS];
/* Notes collected for the step being recorded, committed to the pattern
 * at the next step boundary (seq_capture_advance_clock()): a touch's
 * timing only picks WHICH step. Several touches aimed at the same step
 * build a cluster (up to OP_SEQ_MAX_NOTES_PER_STEP); a touch aimed at a
 * different step starts a new one. count == 0 = nothing recorded. */
static uint8_t s_seq_capture_armed_notes[OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_capture_armed_count;
/* Nearest-step quantization: set at the first touch of a cluster. Past
 * the halfway point of the current step, the touch belongs to the NEXT
 * step (early hits are common). */
static uint8_t s_seq_capture_target_step;
/* Live notes played during capture (a performance, kept separate from
 * scheduled playback state). Several at once, so releasing one finger
 * ends only its own note. */
static uint8_t s_seq_capture_live_pads[OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_capture_live_notes[OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_capture_live_count;

/* Ends the live note `pad` owns, if any. */
static void seq_capture_end_one_sounding_note(uint8_t pad) {
    for (uint8_t i = 0; i < s_seq_capture_live_count; i++) {
        if (s_seq_capture_live_pads[i] != pad) {
            continue;
        }
        tiles_midi_note_off(s_seq_lane_channel[s_seq_edit_lane], s_seq_capture_live_notes[i], 0u); /* ended by release/exit: release velocity 0 */
        tiles_cv_gate_note_off(s_seq_capture_live_notes[i]);
        tiles_haptics_stop(pad);
        for (uint8_t j = i; (uint8_t)(j + 1u) < s_seq_capture_live_count; j++) {
            s_seq_capture_live_pads[j] = s_seq_capture_live_pads[j + 1u];
            s_seq_capture_live_notes[j] = s_seq_capture_live_notes[j + 1u];
        }
        s_seq_capture_live_count--;
        return;
    }
}

/* Ends every live capture note (exit, or a clean slate). */
static void seq_capture_end_all_sounding_notes(void) {
    for (uint8_t i = 0; i < s_seq_capture_live_count; i++) {
        tiles_midi_note_off(s_seq_lane_channel[s_seq_edit_lane], s_seq_capture_live_notes[i], 0u); /* ended by exit: release velocity 0 */
        tiles_cv_gate_note_off(s_seq_capture_live_notes[i]);
        tiles_haptics_stop(s_seq_capture_live_pads[i]);
    }
    s_seq_capture_live_count = 0u;
}

/* Only called in sequencer mode (handle_diamond_transport() checks). */
static void seq_capture_mode_enter(void) {
    /* Close the pattern bank first; the dispatch checks it before capture,
     * so it would otherwise stay on screen over a running capture. */
    if (s_pattern_bank_visible) {
        pattern_bank_exit();
    }
    /* Same for the scale picker (via scale_menu_exit(), to unwind its LED and
     * standby state before capture switches the scale). */
    if (s_scale_menu_visible) {
        scale_menu_exit();
    }
    /* Same for an open per-step edit. */
    if (s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
        edit_exit();
    }
    /* What's already playing on the lane keeps playing: capture layers on top
     * in real time rather than cutting the lane off on entry. */
    s_seq_capture_mode_active = true;
    s_seq_capture_prev_scale = tiles_note_map_get_scale();
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);
    s_seq_capture_armed_count = 0u;
    s_seq_capture_live_count = 0u;
    /* Mark the lane running, so the recorded pattern keeps looping after
     * capture ends (capture's own clock only checks the shared clock). */
    s_seq_lane_running[s_seq_edit_lane] = true;
    tiles_midi_clock_set_running(true);
    /* Quantized start at the next beat, as in seq_start(). */
    s_seq_pending_start[s_seq_edit_lane] = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_seq_capture_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        /* A chord pad already sounding before capture isn't a new strike. */
        s_seq_capture_prev_chord_sounding[i] = s_chord_pad_sounding[i];
    }
    printf("[op_mode] sequencer capture mode -> on (lane %u)\n", (unsigned)s_seq_edit_lane);
}

static void seq_capture_mode_exit(void) {
    if (!s_seq_capture_mode_active) {
        return;
    }
    seq_capture_end_all_sounding_notes();
    s_seq_capture_mode_active = false;
    tiles_note_map_set_scale(s_seq_capture_prev_scale);
    printf("[op_mode] sequencer capture mode -> off\n");
}

static void seq_capture_handle_taps(tiles_midi_clock_state_t clock) {
    uint8_t lane = s_seq_edit_lane;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_seq_capture_prev_pad_touched[pad - 1u];
        bool is_chord_pad = tiles_note_map_is_chord_mode_active() && tiles_note_map_is_chord_region_pad(pad);
        /* Chord pads are captured when they actually STRIKE (s_chord_pad_sounding[]
         * turns true), reading the notes and velocity handle_chord_pad_taps()
         * resolved, so the capture matches what was played (it used to record on
         * raw touch-down with a flat velocity). Other pads record on touch-down.
         * note_count takes the smaller of the two note limits. */
        bool chord_strike_edge = is_chord_pad && s_chord_pad_sounding[pad - 1u] && !s_seq_capture_prev_chord_sounding[pad - 1u];
        if ((touched && !was_touched && !is_chord_pad) || chord_strike_edge) {
            /* A new touch sounds alongside held notes (a cluster), not cutting them
             * off. */
            uint8_t notes[OP_SEQ_MAX_NOTES_PER_STEP];
            uint8_t note_count;
            uint8_t velocity;
            if (is_chord_pad) {
                note_count = (OP_SEQ_MAX_NOTES_PER_STEP < OP_CHORD_NUM_VOICES) ? OP_SEQ_MAX_NOTES_PER_STEP
                                                                                 : OP_CHORD_NUM_VOICES;
                for (uint8_t i = 0; i < note_count; i++) {
                    notes[i] = s_chord_pad_notes[pad - 1u][i];
                }
                velocity = s_chord_pad_last_velocity[pad - 1u];
            } else {
                notes[0] = tiles_note_map_get_note(pad);
                note_count = 1u;
                velocity = OP_SEQ_VELOCITY;
            }
            tiles_haptics_trigger_kick(pad, velocity);
            for (uint8_t i = 0; i < note_count; i++) {
                tiles_midi_note_on(s_seq_lane_channel[lane], notes[i], velocity);
                tiles_cv_gate_note_on(notes[i], velocity);
                if (s_seq_capture_live_count < OP_SEQ_MAX_NOTES_PER_STEP) {
                    s_seq_capture_live_pads[s_seq_capture_live_count] = pad;
                    s_seq_capture_live_notes[s_seq_capture_live_count] = notes[i];
                    s_seq_capture_live_count++;
                }
            }
            /* Nearest-step target (see s_seq_capture_target_step). Before the
             * quantized start resolves the timing is meaningless, but that pending
             * cluster is discarded when the start resolves anyway. */
            uint8_t length = active_pattern()->length;
            if (length < 1u) {
                length = 1u;
            }
            uint32_t elapsed_in_step = clock.pulse_count - s_seq_step_started_at_pulse[lane];
            bool nearest_is_next_step = (elapsed_in_step * 2u) >= OP_SEQ_CLOCKS_PER_STEP;
            uint8_t target =
                nearest_is_next_step ? (uint8_t)((s_seq_current_step[lane] + 1u) % length) : s_seq_current_step[lane];
            /* Same target: add to the cluster. New target: start a fresh cluster. */
            if (s_seq_capture_armed_count == 0u || s_seq_capture_target_step != target) {
                s_seq_capture_target_step = target;
                s_seq_capture_armed_count = 0u;
            }
            for (uint8_t i = 0; i < note_count; i++) {
                if (s_seq_capture_armed_count < OP_SEQ_MAX_NOTES_PER_STEP) {
                    s_seq_capture_armed_notes[s_seq_capture_armed_count] = notes[i];
                    s_seq_capture_armed_count++;
                }
            }
        } else if (!touched && was_touched) {
            seq_capture_end_one_sounding_note(pad);
        }
        s_seq_capture_prev_pad_touched[pad - 1u] = touched;
        s_seq_capture_prev_chord_sounding[pad - 1u] = is_chord_pad && s_chord_pad_sounding[pad - 1u];
    }
}

/* Capture's own clock handling, separate from seq_advance_clock() (which
 * replays steps; this records them). Uses the same per-lane playhead
 * fields for the viewed lane, so no parallel copy exists. */
static void seq_capture_advance_clock(tiles_midi_clock_state_t clock) {
    uint8_t lane = s_seq_edit_lane;
    if (clock.start_edge) {
        s_seq_current_step[lane] = 0u;
        s_seq_step_started_at_pulse[lane] = clock.pulse_count;
        s_seq_capture_armed_count = 0u;
        s_seq_pending_start[lane] = false;
        return;
    }
    /* No early return on !clock.running here: with external clock arriving
     * but the DAW stopped, set_running(true) is ignored and running stays
     * false, which blocked every commit while touches still sounded.
     * Quantization only needs pulse_count, which advances regardless. */
    if (s_seq_pending_start[lane]) {
        /* Nearest-beat pending start, as in seq_advance_clock(). */
        uint32_t phase_in_beat = clock.pulse_count % OP_CLOCK_PULSES_PER_BEAT;
        if (phase_in_beat != 0u && (phase_in_beat * 2u) < OP_CLOCK_PULSES_PER_BEAT) {
            return;
        }
        s_seq_pending_start[lane] = false;
        s_seq_current_step[lane] = 0u;
        s_seq_step_started_at_pulse[lane] = clock.pulse_count;
        s_seq_capture_armed_count = 0u;
        return;
    }

    uint32_t elapsed = clock.pulse_count - s_seq_step_started_at_pulse[lane];
    if (elapsed < OP_SEQ_CLOCKS_PER_STEP) {
        return;
    }
    uint32_t steps_to_advance = elapsed / OP_SEQ_CLOCKS_PER_STEP;
    s_seq_step_started_at_pulse[lane] += steps_to_advance * OP_SEQ_CLOCKS_PER_STEP;

    /* Commit the cluster only into the step it was aimed at (a late touch
     * targets the next step and waits). Additive: steps nobody played this
     * pass keep what they had; capture never clears the rest of the pattern. */
    op_seq_pattern_t *pat = active_pattern();
    if (s_seq_capture_armed_count > 0u && s_seq_capture_target_step == s_seq_current_step[lane]) {
        uint8_t step = s_seq_current_step[lane];
        uint8_t count = s_seq_capture_armed_count;
        if (count > OP_SEQ_MAX_NOTES_PER_STEP) {
            count = OP_SEQ_MAX_NOTES_PER_STEP;
        }
        pat->step_armed[step] = true;
        pat->step_pitch_override[step] = true;
        for (uint8_t i = 0; i < count; i++) {
            pat->step_notes[step][i] = s_seq_capture_armed_notes[i];
        }
        pat->step_note_count[step] = count;
        s_seq_capture_armed_count = 0u;
    }

    uint8_t length = pat->length;
    if (length < 1u) {
        length = 1u;
    }
    uint8_t new_step = (uint8_t)((s_seq_current_step[lane] + steps_to_advance) % length);
    /* Play the lane while capturing: seq_enter_step() ends the previous note
     * and fires whatever the new step holds (probability and ratchet
     * included), so earlier passes are audible under the new take. */
    seq_enter_step(lane, new_step);
}

/* Capture view: melodic-style grid (root magenta, naturals white, sharps
 * off; the scale is chromatic here) so it plays like melodic mode.
 * Recorded steps show dim red (as in the step view, without
 * probability/ratchet tints), taking priority over note coloring; the
 * playhead pulses magenta. */
static void render_seq_capture(uint32_t now_ms) {
    float pulse = menu_selected_pulse_level(now_ms);
    op_seq_pattern_t *pat = active_pattern();
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        /* In the 4x4 layout, pads outside the step grid still play notes and get
         * note coloring; only the step highlights need a real step. */
        uint8_t step = 0u;
        bool has_step = seq_step_for_pad(pat, pad, &step);
        bool is_current_step = has_step && (step == s_seq_current_step[s_seq_edit_lane]) && step < pat->length;
        bool is_armed = has_step && step < pat->length && pat->step_armed[step];
        bool is_live = false;
        for (uint8_t i = 0; i < s_seq_capture_live_count; i++) {
            if (s_seq_capture_live_pads[i] == pad) {
                is_live = true;
                break;
            }
        }
        if (is_live) {
            tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 1.0f, 1.0f);
        } else if (is_current_step) {
            tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R * pulse, OP_MENU_MELODIC_G * pulse,
                                                OP_MENU_MELODIC_B * pulse);
        } else if (is_armed) {
            tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_DIM_RED_LEVEL, 0.0f, 0.0f);
        } else if (tiles_note_map_is_root_pad(pad)) {
            tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R, OP_MENU_MELODIC_G, OP_MENU_MELODIC_B);
        } else if (tiles_note_map_is_natural_pad(pad)) {
            tiles_lighting_set_standby_pad_rgb(pad, OP_SCALE_AVAILABLE_LEVEL, OP_SCALE_AVAILABLE_LEVEL,
                                                OP_SCALE_AVAILABLE_LEVEL);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        tiles_buttons_set_standby_led(board_button_for_col(col), 0.0f);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* Mode menu taps: a touch clicks (any pad); pressing a mode slot past
 * OP_MENU_SELECT_DEPTH_THRESHOLD picks it. The pick is recorded at once
 * (s_menu_pending/_mode, which also makes the slot pulse), but the menu
 * closes and the mode switches only once every pad is released; switching
 * while the finger was down let that touch play a note in the new mode. */
static void handle_menu_taps(void) {
    bool any_touched = false;
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            if (touched) {
                any_touched = true;
            }
            if (touched && !s_menu_prev_pad_touched[pad - 1u]) {
                tiles_haptics_trigger_touch_pulse(pad);
            }
            if (!s_menu_pending && row == OP_MENU_ROW && touched &&
                (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD && col_is_available(col)) {
                tiles_op_mode_t mode = OP_MODE_MELODIC;
                if (col == OP_MENU_COL_CHORD) {
                    mode = OP_MODE_CHORD;
                } else if (col == OP_MENU_COL_SEQUENCER) {
                    mode = OP_MODE_SEQUENCER;
                } else if (col == OP_MENU_COL_GUITAR) {
                    mode = OP_MODE_GUITAR;
                } else if (col == OP_MENU_COL_SONG) {
                    mode = OP_MODE_SONG;
                } else if (col == OP_MENU_COL_SCENE_LAUNCH) {
                    mode = OP_MODE_SCENE_LAUNCH;
                }
                s_menu_pending_mode = mode;
                s_menu_pending = true;
            }
            s_menu_prev_pad_touched[pad - 1u] = touched;
        }
    }
    if (s_menu_pending && !any_touched) {
        tiles_op_mode_t mode = s_menu_pending_mode;
        s_menu_pending = false;
        menu_exit();
        set_active_mode(mode);
    }
}

/* ---- Triangle (+ circle), diamond transport, top-level scan ------------- */

/* Triangle click: toggles the mode menu. Triangle + circle: the scale
 * picker (one global scale, every mode), or, if a sub-state owns the grid,
 * cancels it (per-step edit, sequencer capture, Song capture).
 *
 * Circle is latched once seen during the press (they rarely release
 * together). Square joining too makes it a conflict: triangle + circle +
 * square is heading for the game-mode combo. */
static void handle_triangle_click(void) {
    bool held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);

    if (held && !s_triangle_was_held) {
        s_triangle_press_had_conflict = false;
        s_triangle_press_was_shift = false;
    }
    if (held && tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID)) {
        /* part of the game-mode 4-button combo, not a click */
        s_triangle_press_had_conflict = true;
    }
    if (held && circle_held) {
        if (tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID)) {
            s_triangle_press_had_conflict = true;
        } else {
            s_triangle_press_was_shift = true;
        }
    }

    if (!held && s_triangle_was_held) {
        if (!s_triangle_press_had_conflict) {
            if (s_triangle_press_was_shift) {
                if (!s_menu_visible) {
                    /* Triangle + circle: the scale picker in every mode. */
                    if (s_active_mode == OP_MODE_SEQUENCER && s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
                        /* Cancel an open per-step edit (its escape hatch) instead of opening the
                         * picker over it. */
                        edit_exit();
                    } else if (s_song_capture_active) {
                        /* End a running Song capture instead of opening the picker over it (the
                         * picker would take the grid and freeze the capture). */
                        song_capture_exit();
                    } else if (s_seq_capture_mode_active) {
                        /* Same for sequencer capture. */
                        seq_capture_mode_exit();
                    } else if (s_scale_menu_visible) {
                        scale_menu_exit();
                    } else {
                        /* Open the scale picker (meaningful from every mode that plays notes). */
                        scale_menu_enter();
                    }
                }
            } else if (s_menu_visible) {
                menu_exit();
            } else {
                /* Plain click opens the menu from any mode. */
                menu_enter();
            }
        }
    }

    s_triangle_was_held = held;
}

/* Diamond: DAW transport outside the sequencer.
 *   - Click: play/stop toggle. Sends the Play or Stop CC on the DAW port
 *     (plus System Realtime Start/Stop unless TILES is following an
 *     external clock).
 *   - Hold >= OP_TRANSPORT_RECORD_ARM_HOLD_MS: arms (LED double-blinks);
 *     on release sends the Record CC. Live's count-in preference handles
 *     the count-in.
 * Transport uses CCs because Realtime Start/Stop only drive Live when the
 * port is in external sync with a clock behind it; controllers like the
 * Launchkey use plain CCs, which Live's control surface script (or MIDI
 * Map mode in any DAW) binds.
 *
 * In the sequencer diamond does other things: click = pattern bank,
 * circle + diamond = capture. Elsewhere circle + diamond = Song capture
 * (or Ableton's own capture/stop-all, see the release branch). */
#define OP_TRANSPORT_RECORD_ARM_HOLD_MS 2000u
/* Momentary Play/Stop/Record triggers (127 then 0) on channel 1, DAW port
 * only. 102-104 are in the MIDI spec's undefined CC range (102-119), the
 * range transport hardware conventionally uses. CCs, not notes, so an
 * unmapped receiver can never sound them. The Ableton script
 * (daw-integration/) listens for these; other DAWs need a one-time MIDI
 * map of all three. */
#define OP_TRANSPORT_PLAY_CC 102u
#define OP_TRANSPORT_STOP_CC 103u
#define OP_TRANSPORT_RECORD_CC 104u

/* Song mode's slot count and capture slot, declared early for
 * handle_diamond_transport() and scan. */
#define OP_SONG_NUM_SLOTS TILES_NUM_PADS
static uint8_t s_song_capture_slot; /* 1..24: the slot being recorded into */

/* Diamond's LED level for the transport state. While a mode draws through
 * standby (sequencer, Song, Ableton), override writes are ignored, so
 * those modes' render functions write this level themselves (they used to
 * blank diamond, freezing the transport LED in Ableton mode). */
static float transport_led_level(uint32_t now_ms) {
    float led_level;
    if (s_diamond_record_armed) {
        uint32_t cycle_ms =
            OP_TRANSPORT_ARMED_BLINK_ON_MS * 2u + OP_TRANSPORT_ARMED_BLINK_GAP_MS + OP_TRANSPORT_ARMED_PAUSE_MS;
        uint32_t t = now_ms % cycle_ms;
        bool on = (t < OP_TRANSPORT_ARMED_BLINK_ON_MS) ||
                  (t >= OP_TRANSPORT_ARMED_BLINK_ON_MS + OP_TRANSPORT_ARMED_BLINK_GAP_MS &&
                   t < OP_TRANSPORT_ARMED_BLINK_ON_MS * 2u + OP_TRANSPORT_ARMED_BLINK_GAP_MS);
        led_level = on ? 1.0f : 0.0f;
    } else if (s_transport_recording) {
        float phase = (float)now_ms / OP_TRANSPORT_RECORDING_PULSE_PERIOD_MS;
        float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
        led_level = OP_TRANSPORT_RECORDING_PULSE_MIN +
                    (OP_TRANSPORT_RECORDING_PULSE_MAX - OP_TRANSPORT_RECORDING_PULSE_MIN) * raw;
    } else if (tiles_midi_clock_is_running()) {
        led_level = OP_TRANSPORT_LED_PLAYING_LEVEL;
    } else {
        led_level = OP_TRANSPORT_LED_STOPPED_LEVEL;
    }
    return led_level;
}

static void handle_diamond_transport(uint32_t now_ms) {
    bool held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    bool sequencer_active = (s_active_mode == OP_MODE_SEQUENCER);

    if (held && !s_diamond_was_held) {
        s_diamond_press_had_conflict = false;
        s_diamond_press_start_ms = now_ms;
        s_diamond_record_armed = false;
        s_diamond_press_was_shift = false;
    }
    if (held && tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID)) {
        s_diamond_press_had_conflict = true;
    }
    if (held && circle_held) {
        if (tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID)) {
            /* Three of the four game-combo buttons: a conflict, not a shift. */
            s_diamond_press_had_conflict = true;
        } else {
            s_diamond_press_was_shift = true;
        }
    }
    /* Record arm only outside the sequencer. */
    if (held && !sequencer_active && !s_diamond_press_had_conflict && !s_diamond_press_was_shift &&
        !s_diamond_record_armed && (now_ms - s_diamond_press_start_ms) >= OP_TRANSPORT_RECORD_ARM_HOLD_MS) {
        s_diamond_record_armed = true;
    }

    if (!held && s_diamond_was_held) {
        if (!s_diamond_press_had_conflict) {
            if (sequencer_active) {
                /* Sequencer: circle + diamond = capture (the same gesture as capture
                 * everywhere else), diamond alone = pattern bank. */
                if (s_diamond_press_was_shift) {
                    if (s_seq_capture_mode_active) {
                        seq_capture_mode_exit();
                    } else if (OP_SEQ_CAPTURE_MODE_ENABLED &&
                               (tiles_midi_clock_tap_tempo_established() || tiles_midi_clock_external_active(now_ms))) {
                        /* Capture needs a tempo (tap tempo or external clock); without one it
                         * would record nothing. Until then the click does nothing, like "+". */
                        seq_capture_mode_enter();
                    }
                } else if (s_pattern_bank_visible) {
                    pattern_bank_exit();
                } else {
                    pattern_bank_enter();
                }
            } else if (s_active_mode == OP_MODE_SCENE_LAUNCH && s_diamond_press_was_shift) {
                /* Ableton mode: circle + diamond = stop all clips (Song capture isn't
                 * used there). Checked before the generic Song-capture branch. */
                scene_send_stop_all();
            } else if (s_ableton_capture_active && s_diamond_press_was_shift) {
                /* Recording a Live clip from melodic mode (s_ableton_capture_active):
                 * circle + diamond ends it and returns to Ableton mode. */
                scene_end_capture();
            } else if (s_diamond_press_was_shift) {
                /* Circle + diamond elsewhere (melodic, chord, bass guitar, or Song mode
                 * itself): toggle Song capture, which records into the next empty Song
                 * slot. No tempo needed to enter: until one exists the capture just waits
                 * (live touches still sound), and circle taps a tempo while capturing
                 * (see handle_circle_tap()). */
                if (s_song_capture_active) {
                    song_capture_exit();
                } else {
                    song_capture_enter();
                }
            } else if (s_active_mode == OP_MODE_SONG && s_song_edit_active) {
                /* Song step-edit screen: a diamond click backs up one level (pitch pick ->
                 * step grid -> track overview) instead of sending transport. */
                if (s_song_edit_pick_active) {
                    song_edit_pick_cancel();
                } else {
                    song_edit_exit();
                }
            } else if (s_diamond_record_armed) {
                tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_RECORD_CC, 127u);
                tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_RECORD_CC, 0u);
                s_transport_playing = true;
                s_transport_recording = true;
            } else if (s_transport_playing || s_transport_recording) {
                /* Stop (a stop also ends recording). CC first. The Realtime Stop/Start
                 * byte is skipped while TILES follows an external clock: Live is then the
                 * clock master on this port, and echoing Start/Stop back confused its
                 * sync display. The CC still drives the transport either way. */
                tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_STOP_CC, 127u);
                tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_STOP_CC, 0u);
                if (!tiles_midi_clock_external_active(now_ms)) {
                    tiles_midi_send_stop();
                }
                s_transport_playing = false;
                s_transport_recording = false;
            } else {
                tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_PLAY_CC, 127u);
                tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_PLAY_CC, 0u);
                if (!tiles_midi_clock_external_active(now_ms)) {
                    tiles_midi_send_start();
                }
                s_transport_playing = true;
            }
        }
        s_diamond_record_armed = false;
        s_diamond_press_was_shift = false;
    }

    if (sequencer_active) {
        /* Sequencer: diamond pulses while capturing, dark otherwise (transport
         * state shows on "-"/"+" here). */
        float led_level = s_seq_capture_mode_active ? menu_selected_pulse_level(now_ms) : 0.0f;
        tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, led_level);
    } else {
        /* Transport LED elsewhere (see transport_led_level()): armed beats
         * recording beats playing. "Playing" follows the actual clock
         * (tiles_midi_clock_is_running()); s_transport_playing only decides what
         * the next click sends. */
        tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, transport_led_level(now_ms));
    }

    s_diamond_was_held = held;
}

/* Tap tempo on circle. A press counts only if: sequencer mode with no
 * edit open (or a Song capture is running), no external clock, and no
 * other game-combo button (triangle, diamond, square) held.
 * Committed on RELEASE, with the PRESS time as the tap time: circle is
 * also the first half of circle + "-"/"+" (length), circle + step
 * (ratchet) and circle + diamond, and those only reveal themselves after
 * the press. Any of them joining cancels the tap. */
static uint32_t s_circle_press_ms;
static bool s_circle_press_pending_tap;

static bool any_pad_touched(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_touch_is_touched(pad)) {
            return true;
        }
    }
    return false;
}

static void handle_circle_tap(uint32_t now_ms) {
    bool held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);

    if (held && !s_circle_was_held) {
        s_circle_press_ms = now_ms;
        /* Tap tempo is sequencer-only, plus while a Song capture runs (which can
         * start without a tempo and needs a way to set one). */
        bool mode_ok =
            (s_active_mode == OP_MODE_SEQUENCER && s_seq_edit_mode == OP_SEQ_EDIT_NONE) || s_song_capture_active;
        bool combo_conflict = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
        s_circle_press_pending_tap = mode_ok && !combo_conflict && !tiles_midi_clock_external_active(now_ms);
    }

    if (held && s_circle_press_pending_tap &&
        (tiles_button_is_pressed(TILES_MINUS_BUTTON_ID) || tiles_button_is_pressed(TILES_PLUS_BUTTON_ID) ||
         tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) || any_pad_touched())) {
        /* Another button or a pad joined the hold: it's a combo, not a tap. */
        s_circle_press_pending_tap = false;
    }

    if (!held && s_circle_was_held) {
        /* A plain circle release registers the tap. */
        if (s_circle_press_pending_tap) {
            tiles_midi_clock_register_tap(s_circle_press_ms);
        }
    }

    s_circle_was_held = held;
}

static bool any_lane_running(void) {
    for (uint8_t lane = 0u; lane < OP_SEQ_NUM_LANES; lane++) {
        if (s_seq_lane_running[lane]) {
            return true;
        }
    }
    return false;
}

/* Declared early for tiles_op_mode_is_sequencer_active(). Up to
 * OP_SONG_MAX_CONCURRENT slots can run at once. */
static bool s_song_slot_running[OP_SONG_NUM_SLOTS];

/* Like any_lane_running(), for Song tracks (they also run in the
 * background), so a looping Song pattern also gets standby's longer
 * timeouts. */
static bool any_song_slot_running(void) {
    for (uint8_t slot = 0u; slot < OP_SONG_NUM_SLOTS; slot++) {
        if (s_song_slot_running[slot]) {
            return true;
        }
    }
    return false;
}

/* "-"/"+": sequencer transport and length, bass guitar fret shift, and
 * Ableton track panning, in one handler with one set of press tracking
 * (the modes are exclusive). Sequencer rules: see the "Transport +
 * length" state comment. Frets and panning just step once on release. */
static void handle_transport_and_length(uint32_t now_ms) {
    bool minus_held = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID);
    bool plus_held = tiles_button_is_pressed(TILES_PLUS_BUTTON_ID);
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    /* Not during a per-step edit or capture (capture uses the same pending
     * start fields; "+" would restart the take). */
    bool active =
        (s_active_mode == OP_MODE_SEQUENCER) && s_seq_edit_mode == OP_SEQ_EDIT_NONE && !s_seq_capture_mode_active;
    bool guitar_active = (s_active_mode == OP_MODE_GUITAR);
    bool scene_launch_active = (s_active_mode == OP_MODE_SCENE_LAUNCH);

    if (active && minus_held && !s_minus_was_held && circle_held) {
        op_seq_pattern_t *pat = active_pattern();
        if (pat->length > OP_SEQ_MIN_LENGTH) {
            pat->length--;
        }
        s_minus_used_as_combo = true;
        s_seq_length_flash_ms = now_ms;
    }
    if (active && plus_held && !s_plus_was_held && circle_held) {
        op_seq_pattern_t *pat = active_pattern();
        if (pat->length < OP_SEQ_MAX_LENGTH) {
            pat->length++;
        }
        s_plus_used_as_combo = true;
        s_seq_length_flash_ms = now_ms;
    }

    if (!minus_held && s_minus_was_held) {
        if (active && !s_minus_used_as_combo) {
            /* "-" on the viewed lane only: while playing, pause in place; while
             * already stopped, rewind to step 0 (Live-style double stop). Other lanes
             * are untouched. */
            uint8_t lane = s_seq_edit_lane;
            if (s_seq_lane_running[lane]) {
                s_seq_lane_running[lane] = false;
                s_seq_pending_start[lane] = false;
                /* The shared clock stops only once every lane has stopped. */
                if (!any_lane_running()) {
                    tiles_midi_clock_set_running(false);
                }
            } else {
                s_seq_current_step[lane] = 0u;
                s_seq_note_sounding[lane] = false;
                s_seq_pending_start[lane] = false;
            }
        } else if (guitar_active) {
            /* Bass guitar: one fret per press. */
            uint8_t offset = tiles_note_map_get_guitar_fret_offset();
            if (offset > 0u) {
                tiles_note_map_set_guitar_fret_offset((uint8_t)(offset - 1u));
            }
        } else if (scene_launch_active) {
            /* Ableton mode: "-"/"+" pan the 5 visible track columns by one track. */
            if (s_scene_track_offset > 0u) {
                s_scene_track_offset--;
                scene_send_track_offset(s_scene_track_offset);
            }
        }
        s_minus_used_as_combo = false;
    }
    if (!plus_held && s_plus_was_held) {
        if (active && !s_plus_used_as_combo) {
            uint8_t lane = s_seq_edit_lane;
            if (s_seq_lane_running[lane]) {
                /* "+" while playing: restart from step 0 at the next beat. */
                s_seq_pending_start[lane] = true;
                s_seq_pending_restart[lane] = true;
            } else if (tiles_midi_clock_tap_tempo_established() || tiles_midi_clock_external_active(now_ms)) {
                /* "+" while stopped (needs a tempo, else nothing would advance): resume
                 * where the playhead is (step 0 after a double stop), quantized to the
                 * next beat. Also starts the shared clock (no-op if already running). */
                s_seq_lane_running[lane] = true;
                tiles_midi_clock_set_running(true);
                s_seq_pending_start[lane] = true;
                s_seq_pending_restart[lane] = false;
            }
        } else if (guitar_active) {
            /* set_guitar_fret_offset() clamps. */
            tiles_note_map_set_guitar_fret_offset((uint8_t)(tiles_note_map_get_guitar_fret_offset() + 1u));
        } else if (scene_launch_active) {
            if (s_scene_track_offset < OP_SCENE_MAX_TRACKS - OP_SCENE_TRACK_COL_MAX) {
                s_scene_track_offset++;
                scene_send_track_offset(s_scene_track_offset);
            }
        }
        s_plus_used_as_combo = false;
    }

    s_minus_was_held = minus_held;
    s_plus_was_held = plus_held;
}

/* One flash per quarter note, whichever source drives the clock. */
static float compute_beat_flash_level(uint32_t now_ms, tiles_midi_clock_state_t clock) {
    if (!clock.running) {
        return 0.0f;
    }
    uint32_t beat_index = clock.pulse_count / OP_CLOCK_PULSES_PER_BEAT;
    if (beat_index != s_last_beat_index) {
        s_last_beat_index = beat_index;
        s_beat_flash_start_ms = now_ms;
    }
    if ((now_ms - s_beat_flash_start_ms) < OP_BEAT_FLASH_DURATION_MS) {
        return OP_BEAT_FLASH_LEVEL;
    }
    return 0.0f;
}

/* Song mode functions, defined in its section below. */
static void song_store_load_all(void);
static void handle_song_overview_taps(uint32_t now_ms);
static void render_song_overview(uint32_t now_ms);
static void render_song_underglow(void);
static void song_advance_clock(uint8_t slot, tiles_midi_clock_state_t clock);
static void song_capture_handle_taps(tiles_midi_clock_state_t clock);
static void song_capture_advance_clock(tiles_midi_clock_state_t clock);
/* Song step-edit functions, defined below (song_capture_enter/_exit,
 * song_edit_exit and song_edit_pick_cancel are declared earlier still). */
static void song_edit_enter(uint8_t pad);
static void handle_song_edit_taps(uint32_t now_ms);
static void render_song_edit(uint32_t now_ms);
static void handle_song_edit_pick_taps(uint32_t now_ms);

/* Ableton mode functions, defined below. scene_launch_init() registers its
 * SysEx callback (see tiles_op_mode_init()). */
static void scene_launch_init(void);
static bool handle_scene_launch_taps(uint32_t now_ms);
static void render_scene_launch(uint32_t now_ms);

/* ---- Melodic echo: showing an incoming melody on the pads ------------------
 * The TILES DISPLAY Max for Live device (daw-integration/ableton/
 * TILES_DISPLAY/) sends a track's notes to TILES through the control
 * surface's MIDI output (plain track routing can't: after an instrument a
 * track outputs audio). This is midi_in.h's note callback listener.
 *
 * Two layers, by channel: channel 2 = layer 1 (a second TILES DISPLAY,
 * drawn soft red), every other channel = layer 0 (green). Layers are
 * independent, so a Note-Off on one never clears the same pitch held on
 * the other.
 *
 * Notes are tracked in every mode (so a Note-Off always clears its
 * Note-On, even across mode switches), but only shown in melodic mode
 * (tiles_op_mode_incoming_note_is_sounding()), read per pad by
 * services/lighting.c. Not in chord mode's melody grid. A note with no
 * pad in the current layout simply isn't shown. */
static bool s_incoming_note_sounding[TILES_OP_MODE_ECHO_LAYERS][128];
/* Time of each note's latest Note-On (for lighting's onset flash). Valid
 * while the note is sounding. */
static uint32_t s_incoming_note_on_ms[TILES_OP_MODE_ECHO_LAYERS][128];

/* MIDI channel 2 (nibble 1) = the second TILES DISPLAY. */
#define OP_ECHO_SECONDARY_CHANNEL 1u

static void melodic_echo_on_midi_note(uint8_t channel, uint8_t note, uint8_t velocity, bool note_on,
                                       uint32_t now_ms) {
    (void)velocity;
    uint8_t layer = (channel == OP_ECHO_SECONDARY_CHANNEL) ? 1u : 0u;
    if (note_on) {
        s_incoming_note_on_ms[layer][note] = now_ms;
    }
    s_incoming_note_sounding[layer][note] = note_on;
}

static void melodic_echo_init(void) {
    for (uint8_t layer = 0; layer < TILES_OP_MODE_ECHO_LAYERS; layer++) {
        for (uint16_t i = 0; i < 128u; i++) {
            s_incoming_note_sounding[layer][i] = false;
        }
    }
    tiles_midi_in_register_note_callback(melodic_echo_on_midi_note);
}

/* Includes the melodic-mode check itself (the only use), rather than
 * exporting a separate accessor. */
bool tiles_op_mode_incoming_note_is_sounding(uint8_t layer, uint8_t note) {
    return s_active_mode == OP_MODE_MELODIC && layer < TILES_OP_MODE_ECHO_LAYERS && s_incoming_note_sounding[layer][note];
}

/* Unsigned subtraction wraps correctly. */
uint32_t tiles_op_mode_incoming_note_age_ms(uint8_t layer, uint8_t note) {
    return to_ms_since_boot(get_absolute_time()) - s_incoming_note_on_ms[layer][note];
}

void tiles_op_mode_init(bool crash_recovered) {
    if (!crash_recovered) {
        s_active_mode = OP_MODE_MELODIC;
    }
    s_menu_visible = false;
    s_menu_pending = false;
    s_triangle_was_held = false;
    s_triangle_press_had_conflict = false;
    s_triangle_press_was_shift = false;
    s_scale_menu_visible = false;
    s_scale_menu_pending_exit = false;
    s_pattern_bank_visible = false;
    s_diamond_was_held = false;
    s_diamond_press_had_conflict = false;
    s_diamond_press_was_shift = false;
    s_diamond_record_armed = false;
    s_transport_playing = false;
    s_transport_recording = false;
    s_seq_capture_mode_active = false;
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            op_seq_pattern_t *pat = &s_seq_pattern[lane][alt];
            for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
                pat->step_armed[i] = false;
                pat->step_pitch_override[i] = false;
                pat->step_note_count[i] = 0u;
                pat->step_probability_percent[i] = 100u;
                pat->step_ratchet_count[i] = 1u;
            }
            pat->probability_enabled = false;
            pat->length = OP_SEQ_NUM_STEPS;
            s_pattern_slot_saved[lane][alt] = false;
        }
        if (!crash_recovered) {
            s_seq_active_alt[lane] = 0u;
            s_seq_lane_running[lane] = false;
        }
        /* One fixed channel per lane (services/midi_channels.h). */
        static const uint8_t SEQ_LANE_CHANNELS[OP_SEQ_NUM_LANES] = {TILES_MIDI_CH_SEQ_LANE_0, TILES_MIDI_CH_SEQ_LANE_1,
                                                                    TILES_MIDI_CH_SEQ_LANE_2, TILES_MIDI_CH_SEQ_LANE_3};
        s_seq_lane_channel[lane] = SEQ_LANE_CHANNELS[lane];
        s_seq_current_step[lane] = 0u;
        s_seq_note_sounding[lane] = false;
        s_seq_step_started_at_pulse[lane] = 0u;
        s_seq_pending_start[lane] = false;
        s_seq_pending_restart[lane] = false;
        s_seq_ratchet_remaining[lane] = 0u;
    }
    s_seq_edit_lane = 0u;
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    s_minus_was_held = false;
    s_plus_was_held = false;
    s_minus_used_as_combo = false;
    s_plus_used_as_combo = false;
    s_circle_was_held = false;
    s_circle_press_pending_tap = false;
    s_last_beat_index = 0xFFFFFFFFu;
    s_beat_flash_start_ms = 0u;
    tiles_buttons_set_override_active(TILES_TRIANGLE_BUTTON_ID, true);
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
    /* Diamond's LED is the transport indicator (a permanent override, like
     * triangle's). */
    tiles_buttons_set_override_active(TILES_DIAMOND_BUTTON_ID, true);
    tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, OP_TRANSPORT_LED_STOPPED_LEVEL);
    s_boot_relight_guard_until_ms = to_ms_since_boot(get_absolute_time()) + OP_BOOT_RELIGHT_GUARD_MS;
    /* After the defaults loop: only overwrites slots flash marks as saved. A
     * plain read, fine on every boot. */
    pattern_store_load_all();
    /* Song library: same. Song tracks always come up stopped after a reboot,
     * crash included (their running state isn't in __uninitialized_ram, unlike
     * the sequencer's lanes). */
    song_store_load_all();

    /* Register Ableton mode's SysEx callback (harmless if never used). */
    scene_launch_init();
    /* Register the melodic echo's note callback. */
    melodic_echo_init();

    if (crash_recovered) {
        /* Crash recovery: mode, patterns per lane and running lanes survived in
         * __uninitialized_ram. Re-running set_active_mode() with the same mode
         * re-syncs note_map's flags and standby ownership (the "leaving" branches
         * don't fire when old == new). */
        set_active_mode(s_active_mode);
        for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
            if (s_seq_lane_running[lane]) {
                /* Restart the shared clock too: the lane flags survived but the clock's
                 * running flag didn't, and nothing would ever start it. */
                tiles_midi_clock_set_running(true);
                break;
            }
        }
    }
}

void tiles_op_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Boot guard: keep triangle's LED off for a moment (see
     * s_boot_relight_guard_until_ms). A no-op while a menu owns the LED. */
    if (now_ms < s_boot_relight_guard_until_ms) {
        tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
    }

    if (other_feature_owns_input()) {
        /* Keep edge tracking current so a held button or touch isn't read as new
         * when control returns. */
        s_triangle_was_held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
        s_diamond_was_held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
        s_circle_was_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
        s_minus_was_held = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID);
        s_plus_was_held = tiles_button_is_pressed(TILES_PLUS_BUTTON_ID);
        /* While another feature owns the board, this scan doesn't run, so a
         * sequencer note sounding at that moment would never end (and its haptic
         * would buzz at the sustain floor). End them now; harmless to repeat. */
        for (uint8_t lane = 0u; lane < OP_SEQ_NUM_LANES; lane++) {
            if (s_seq_note_sounding[lane]) {
                seq_end_current_note(lane);
            }
        }
        /* Same for chord notes. */
        if (s_active_mode == OP_MODE_CHORD) {
            chord_end_all_notes();
        }
        return;
    }

    handle_triangle_click();
    handle_diamond_transport(now_ms);
    handle_circle_tap(now_ms);
    handle_transport_and_length(now_ms);

    /* Fetched once per scan (it clears start_edge); playback and the beat
     * flash share it. */
    tiles_midi_clock_state_t clock = tiles_midi_clock_get_state();
    float beat_flash_level = compute_beat_flash_level(now_ms, clock);

    /* Tap tempo starts the viewed lane too: the first tap-tempo start
     * (start_edge from tap tempo, not an external Start) starts the lane the
     * same way "+" would, if it's stopped. Sequencer mode only; a Song capture
     * marks its own slot running. */
    if (s_active_mode == OP_MODE_SEQUENCER && clock.start_edge && clock.source_is_tap_tempo &&
        !s_seq_lane_running[s_seq_edit_lane]) {
        s_seq_lane_running[s_seq_edit_lane] = true;
        tiles_midi_clock_set_running(true);
        s_seq_pending_start[s_seq_edit_lane] = true;
        s_seq_pending_restart[s_seq_edit_lane] = false;
    }

    /* Every lane advances every scan, whatever menu or sub-view is open (they
     * used to freeze while a menu was up). Except the lane a capture is
     * recording, which seq_capture_advance_clock() handles instead. */
    for (uint8_t lane = 0u; lane < OP_SEQ_NUM_LANES; lane++) {
        if (s_seq_capture_mode_active && lane == s_seq_edit_lane) {
            continue;
        }
        /* Crash-recorder trace: which lane (0-3) is being advanced. */
        tiles_debug_trace((char)('0' + lane));
        seq_advance_clock(lane, clock);
    }
    /* Same for Song tracks, except the slot a Song capture is recording. */
    for (uint8_t slot = 0u; slot < OP_SONG_NUM_SLOTS; slot++) {
        if (s_song_capture_active && slot == (uint8_t)(s_song_capture_slot - 1u)) {
            continue;
        }
        song_advance_clock(slot, clock);
    }

    if (s_menu_visible) {
        handle_menu_taps();
        render_menu(now_ms);
        return;
    }

    if (s_scale_menu_visible) {
        handle_scale_menu_taps();
        render_scale_menu(now_ms);
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER && s_pattern_bank_visible) {
        handle_pattern_bank_taps(now_ms);
        render_pattern_bank(now_ms);
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER && s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
        handle_edit_mode(now_ms);
        render_edit_mode(now_ms, clock.running);
        return;
    }

    if (s_seq_capture_mode_active) {
        seq_capture_handle_taps(clock);
        seq_capture_advance_clock(clock);
        /* Sequencer capture has its own handling and view. */
        render_seq_capture(now_ms);
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER) {
        seq_handle_step_taps(now_ms);
        render_sequencer(beat_flash_level, clock.running);
    } else if (s_active_mode == OP_MODE_SONG) {
        if (s_song_capture_active) {
            /* Song capture from within Song mode: the grid shows live melodic
             * coloring (no overview render, no grid claim; see song_capture_enter()),
             * and the amber recording underglow comes from services/lighting.c's
             * priority chain, so it isn't drawn here. */
            song_capture_handle_taps(clock);
            song_capture_advance_clock(clock);
        } else if (s_song_edit_active) {
            if (s_song_edit_pick_active) {
                /* Step-edit pitch pick: live melodic grid, plus Song's underglow (safe to
                 * draw here; see render_song_underglow()). */
                handle_song_edit_pick_taps(now_ms);
                render_song_underglow();
            } else {
                handle_song_edit_taps(now_ms);
                render_song_edit(now_ms);
            }
        } else {
            handle_song_overview_taps(now_ms);
            render_song_overview(now_ms);
        }
    } else if (s_active_mode == OP_MODE_SCENE_LAUNCH) {
        /* True if it just switched modes (Ableton opened melodic mode for a new
         * recording; see s_scene_pending_melodic): skip this frame's render. */
        if (!handle_scene_launch_taps(now_ms)) {
            render_scene_launch(now_ms);
        }
    } else {
        /* Outside the sequencer view, triangle pulses slowly (one beat per cycle)
         * while any lane plays in the background, since nothing else shows it.
         * Calmer than the menu selection pulse. */
        float level = any_lane_running() ? background_pattern_pulse_level(now_ms) : 0.0f;
        tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, level);
    }
    /* Chord pads bypass owns_pad() and expression.c's gate, so check the
     * menus here: no chords under an open menu. */
    if (s_active_mode == OP_MODE_CHORD && !s_menu_visible && !s_scale_menu_visible) {
        handle_chord_pad_taps(now_ms);
    }
    /* Bass guitar and chord's melody columns need nothing here: they play and
     * light through expression.c and lighting.c (which read note_map). The
     * chord strip's blue comes from lighting.c too. */
}

bool tiles_op_mode_owns_pad_grid(void) {
    /* Menus, and modes that draw the whole grid (mode_owns_standby_grid()).
     * expression.c asks tiles_op_mode_owns_pad(), which defers here for every
     * mode but chord, so these pads mean steps/slots/clips, not notes. Song
     * capture and Song's pitch pick are excluded on purpose: there the grid
     * is a live melodic surface, and expression.c's live sound plays while
     * the same touch is recorded on the Song slot's channel. */
    return s_menu_visible || s_scale_menu_visible || mode_owns_standby_grid(s_active_mode);
}

/* Chord mode claims only its 8 strip pads, unless a menu is open, in which
 * case every pad defers to the blanket answer (menu touches mustn't play,
 * which they did before this check). */
bool tiles_op_mode_owns_pad(uint8_t logical_pad) {
    if (s_active_mode == OP_MODE_CHORD && !s_menu_visible && !s_scale_menu_visible) {
        return tiles_note_map_is_chord_region_pad(logical_pad);
    }
    return tiles_op_mode_owns_pad_grid();
}

/* Wider than owns_pad_grid(): also bass guitar mode, which takes "-"/"+"
 * from octave_control.c while its notes still play normally. */
bool tiles_op_mode_owns_octave_buttons(void) {
    return tiles_op_mode_owns_pad_grid() || s_active_mode == OP_MODE_GUITAR;
}

bool tiles_op_mode_melodic_harmonics_may_play(void) {
    return s_active_mode == OP_MODE_MELODIC || s_active_mode == OP_MODE_CHORD;
}

bool tiles_op_mode_is_sequencer_active(void) {
    /* True while a pattern mode is shown or a pattern is actually playing in
     * the background (a sequencer lane or a Song track), so standby uses its
     * longer timeouts. (Lane flags, not the shared clock: an external clock
     * can tick with every lane stopped.) */
    return s_active_mode == OP_MODE_SEQUENCER || any_lane_running() || s_active_mode == OP_MODE_SONG ||
           any_song_slot_running();
}

bool tiles_op_mode_has_menu_open(void) {
    /* Sub-views that can sit untouched (menus, pattern bank, step edit,
     * capture waiting for a tempo): standby must not cover them. */
    return s_menu_visible || s_scale_menu_visible || s_pattern_bank_visible || s_seq_edit_mode != OP_SEQ_EDIT_NONE ||
           s_seq_capture_mode_active;
}

/* ---- Song mode (BETA) --------------------------------------------------------
 * BETA: not yet tested on hardware ("song mode will remain not tested and
 * should be considered a beta for now", 2026-09-29), including how it
 * interacts with the MPE zone (a Song claim waits for an idle zone before
 * it is re-declared, and resets the channel's bend/pressure). Treat any
 * change here as untested until someone plays it.
 *
 * A looper-style library, like Live's Session View:
 *   - 24 slots, one per pad on the track overview (render_song_overview()/
 *     handle_song_overview_taps()). Tap = start/stop. Circle + tap = pick
 *     up; tap elsewhere = move there (or swap). Circle + hold 5 s = delete.
 *     Hold 2 s = open the step editor.
 *   - Each pattern: 128 steps (8 pages x 16), up to 4 notes per step. No
 *     probability or ratchet.
 *   - Up to OP_SONG_MAX_CONCURRENT (8) patterns PLAY at once. Channels are
 *     claimed from services/midi_channels.h's shared pool when a pattern
 *     starts and released when it stops, so a pattern's channel doesn't
 *     depend on its slot (it can differ between play sessions). A start
 *     with the pool full is refused with a red flash.
 *   - Recording is Song capture (circle + diamond, from melodic, chord,
 *     bass guitar or Song mode itself) into the next empty slot.
 *   - Color: yellow theme; each pattern gets its own hue in a
 *     yellow-green-to-orange band (hue_byte, persisted). */

/* Step editor layout: columns 1-4 x 4 rows = the page's 16 steps
 * (row-major); columns 5-6 = the 8 page pads, row 1 = pages 1-2 ... row 4
 * = pages 7-8 (column 5 the lower page of each pair). */
#define OP_SONG_STEPS_PER_PAGE 16u
#define OP_SONG_NUM_PAGES 8u
#define OP_SONG_NUM_STEPS (OP_SONG_STEPS_PER_PAGE * OP_SONG_NUM_PAGES) /* 128 */
/* OP_SONG_NUM_SLOTS is declared earlier (before handle_diamond_transport()).
 * Up to 4 notes per step; Song's flash region has room without packing
 * (see tiles_song_store_t's _Static_assert). */
#define OP_SONG_MAX_NOTES_PER_STEP 4u

/* One saved pattern. The runtime struct IS the flash layout (513 bytes x
 * 24 fits the 4-sector region with room to spare). step_notes[][] uses
 * 0xFF for an unused slot, which also means "step empty" (Song has no
 * separate armed/override flags). hue_byte: the pattern's color, 0 = warm
 * end of the band, 255 = cool end, assigned at creation (see
 * OP_SONG_HUE_STEP). */
typedef struct {
    uint8_t step_notes[OP_SONG_NUM_STEPS][OP_SONG_MAX_NOTES_PER_STEP];
    uint8_t hue_byte;
} op_song_pattern_t;

static op_song_pattern_t s_song_pattern[OP_SONG_NUM_SLOTS];
static bool s_song_slot_occupied[OP_SONG_NUM_SLOTS];
/* Runtime playback state per slot (valid while occupied), shaped like the
 * sequencer's per-lane fields. Not persisted: tracks always come up
 * stopped. s_song_slot_running[] is declared earlier. */
static uint8_t s_song_slot_channel[OP_SONG_NUM_SLOTS]; /* valid only while running */
static uint8_t s_song_current_step[OP_SONG_NUM_SLOTS];
static uint32_t s_song_step_started_at_pulse[OP_SONG_NUM_SLOTS];
static bool s_song_note_sounding[OP_SONG_NUM_SLOTS];
static uint8_t s_song_sounding_notes[OP_SONG_NUM_SLOTS][OP_SONG_MAX_NOTES_PER_STEP];
static uint8_t s_song_sounding_note_count[OP_SONG_NUM_SLOTS];

/* = services/midi_channels.h TILES_MIDI_SONG_MAX_CONCURRENT (8), aliased
 * locally. */
#define OP_SONG_MAX_CONCURRENT TILES_MIDI_SONG_MAX_CONCURRENT

/* Channels come from services/midi_channels.h
 * (tiles_midi_channels_song_claim()/_release()), claimed at play start. */

#define TILES_SONG_STORE_MAGIC 0x474e4f53u /* "SONG" */
/* v2 added next_hue_byte (a different layout, so v1 images load as
 * "nothing saved"). */
#define TILES_SONG_STORE_VERSION 2u
/* 4 sectors just below the sequencer's sector (storage/flash_map.h).
 * Written as 4 separate single-sector erase+program steps, each as safe
 * as the sequencer's save: longer overall (four short pauses), never a
 * watchdog risk. */
#define TILES_SONG_NUM_FLASH_SECTORS TILES_FLASH_SONG_SECTORS
#define TILES_SONG_FLASH_OFFSET TILES_FLASH_SONG_OFFSET

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t occupied_mask; /* one bit per slot */
    uint8_t next_hue_byte; /* OP_SONG_HUE_STEP's counter, persisted (no early repeats after a reboot) */
    op_song_pattern_t pattern[OP_SONG_NUM_SLOTS];
} tiles_song_store_t;

/* Must fit its sectors: 13 + 24 * 513 = 12325 of 16384 bytes. */
_Static_assert(sizeof(tiles_song_store_t) <= FLASH_SECTOR_SIZE * TILES_SONG_NUM_FLASH_SECTORS,
               "tiles_song_store_t no longer fits in its reserved flash region");

/* OP_SONG_HUE_STEP's counter; declared here because the store functions
 * persist it. */
static uint8_t s_song_next_hue_byte;

/* Rewrites the whole store from RAM (like pattern_store_write_all()),
 * sector by sector. Static buffers: ~12 KB is far too much for the stack. */
static void song_store_write_all(void) {
    static tiles_song_store_t s_store;
    static uint8_t s_write_buf[FLASH_SECTOR_SIZE];
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = TILES_SONG_STORE_MAGIC;
    s_store.version = TILES_SONG_STORE_VERSION;
    s_store.next_hue_byte = s_song_next_hue_byte;
    uint32_t mask = 0u;
    for (uint8_t i = 0; i < OP_SONG_NUM_SLOTS; i++) {
        if (s_song_slot_occupied[i]) {
            mask |= (uint32_t)1u << i;
        }
        s_store.pattern[i] = s_song_pattern[i];
    }
    s_store.occupied_mask = mask;

    const uint8_t *src = (const uint8_t *)&s_store;
    size_t total = sizeof(s_store);
    for (uint32_t sector = 0u; sector < TILES_SONG_NUM_FLASH_SECTORS; sector++) {
        memset(s_write_buf, 0, sizeof(s_write_buf));
        size_t offset_in_store = (size_t)sector * FLASH_SECTOR_SIZE;
        size_t remaining = (offset_in_store < total) ? (total - offset_in_store) : 0u;
        size_t copy_len = (remaining < FLASH_SECTOR_SIZE) ? remaining : FLASH_SECTOR_SIZE;
        if (copy_len > 0u) {
            memcpy(s_write_buf, src + offset_in_store, copy_len);
        }
        watchdog_update();
        uint32_t prev_interrupts = save_and_disable_interrupts();
        flash_range_erase(TILES_SONG_FLASH_OFFSET + sector * FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE);
        flash_range_program(TILES_SONG_FLASH_OFFSET + sector * FLASH_SECTOR_SIZE, s_write_buf, FLASH_SECTOR_SIZE);
        restore_interrupts(prev_interrupts);
        watchdog_update();
    }
}

/* A plain read; safe on every boot (called from tiles_op_mode_init()). */
static void song_store_load_all(void) {
    const tiles_song_store_t *store = (const tiles_song_store_t *)(XIP_BASE + TILES_SONG_FLASH_OFFSET);
    if (store->magic != TILES_SONG_STORE_MAGIC || store->version != TILES_SONG_STORE_VERSION) {
        return; /* never saved on this board, or a different layout version */
    }
    s_song_next_hue_byte = store->next_hue_byte;
    for (uint8_t i = 0; i < OP_SONG_NUM_SLOTS; i++) {
        s_song_slot_occupied[i] = (store->occupied_mask & ((uint32_t)1u << i)) != 0u;
        if (s_song_slot_occupied[i]) {
            s_song_pattern[i] = store->pattern[i];
        }
    }
    printf("[op_mode] loaded saved song patterns from flash\n");
}


/* ---- Song mode: track overview ----------------------------------------------
 * 24 pads, one per slot. Tap a stopped slot to start it (from step 1), tap
 * a playing one to stop it. Circle + tap picks a slot up (pulses green); a
 * plain tap elsewhere moves it (or swaps) with a double green flash;
 * tapping it again cancels. Circle + hold 5 s deletes (double red flash).
 * Hold 2 s opens the step editor. */

/* Start/stop is a plain touch toggle (resolved on release), no press
 * needed. */
static void song_end_current_note(uint8_t slot) {
    if (!s_song_note_sounding[slot]) {
        return;
    }
    for (uint8_t i = 0; i < s_song_sounding_note_count[slot]; i++) {
        tiles_midi_note_off(s_song_slot_channel[slot], s_song_sounding_notes[slot][i], 0u); /* clock-fired: release velocity 0 */
        tiles_cv_gate_note_off(s_song_sounding_notes[slot][i]);
    }
    s_song_note_sounding[slot] = false;
    s_song_sounding_note_count[slot] = 0u;
}

/* hue_byte -> RGB across 30 deg (orange) .. 90 deg (yellow-green), via a
 * real HSV conversion so the band can change freely. */
static void song_hue_to_rgb(uint8_t hue_byte, float sat, float val, float *out_r, float *out_g, float *out_b) {
    float hue = 30.0f + ((float)hue_byte / 255.0f) * 60.0f;
    float c = val * sat;
    float x = c * (1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f));
    float m = val - c;
    float rp = 0.0f, gp = 0.0f, bp = 0.0f;
    if (hue < 60.0f) {
        rp = c;
        gp = x;
    } else {
        rp = x;
        gp = c;
    }
    *out_r = rp + m;
    *out_g = gp + m;
    *out_b = bp + m;
}

/* Single short red flash: "that didn't work" (start with every channel
 * busy, or capture with no empty slot). */
#define OP_SONG_ERROR_FLASH_MS 300u
static bool s_song_error_flash_active;
static uint32_t s_song_error_flash_start_ms;
static uint8_t s_song_error_flash_pad;

static void song_flash_error(uint8_t pad) {
    s_song_error_flash_active = true;
    s_song_error_flash_start_ms = to_ms_since_boot(get_absolute_time());
    s_song_error_flash_pad = pad;
}

/* Move (green) / delete (red) confirmation: two blinks, the same timing as
 * the pattern bank's, as a separate copy. */
#define OP_SONG_FLASH_BLINK_MS 150u
#define OP_SONG_FLASH_COUNT 2u
#define OP_SONG_FLASH_TOTAL_MS (OP_SONG_FLASH_BLINK_MS * 2u * OP_SONG_FLASH_COUNT)
static bool s_song_flash_active;
static bool s_song_flash_is_delete; /* false = green (moved/swapped), true = red (deleted) */
static uint32_t s_song_flash_start_ms;
static uint8_t s_song_flash_pad; /* the cell just moved/deleted (for a swap, the destination) */
static uint8_t s_song_flash_pad2; /* the other cell of a swap, 0 if none */

static void song_flash_confirm(uint8_t pad, uint8_t pad2, bool is_delete) {
    s_song_flash_active = true;
    s_song_flash_is_delete = is_delete;
    s_song_flash_start_ms = to_ms_since_boot(get_absolute_time());
    s_song_flash_pad = pad;
    s_song_flash_pad2 = pad2;
}

/* 0 = nothing picked up. Placing needs no circle; a plain tap completes
 * it, and tapping the same pad cancels. */
static uint8_t s_song_picked_up_slot;

static void song_pick_up(uint8_t pad) {
    s_song_picked_up_slot = pad;
}

static void song_cancel_pick_up(void) {
    s_song_picked_up_slot = 0u;
}

/* Moves `from` to an empty `to`, or swaps with an occupied one. The whole
 * slot's state moves with the pattern; a playing pattern keeps its
 * claimed channel (channels aren't tied to slots). */
static void song_place(uint8_t from_pad, uint8_t to_pad) {
    uint8_t from = from_pad - 1u;
    uint8_t to = to_pad - 1u;

    op_song_pattern_t tmp_pattern = s_song_pattern[to];
    bool tmp_occupied = s_song_slot_occupied[to];
    bool tmp_running = s_song_slot_running[to];
    uint8_t tmp_channel = s_song_slot_channel[to];
    uint8_t tmp_step = s_song_current_step[to];
    uint32_t tmp_pulse = s_song_step_started_at_pulse[to];
    bool tmp_sounding = s_song_note_sounding[to];
    uint8_t tmp_notes[OP_SONG_MAX_NOTES_PER_STEP];
    memcpy(tmp_notes, s_song_sounding_notes[to], sizeof(tmp_notes));
    uint8_t tmp_note_count = s_song_sounding_note_count[to];

    s_song_pattern[to] = s_song_pattern[from];
    s_song_slot_occupied[to] = s_song_slot_occupied[from];
    s_song_slot_running[to] = s_song_slot_running[from];
    s_song_slot_channel[to] = s_song_slot_channel[from];
    s_song_current_step[to] = s_song_current_step[from];
    s_song_step_started_at_pulse[to] = s_song_step_started_at_pulse[from];
    s_song_note_sounding[to] = s_song_note_sounding[from];
    memcpy(s_song_sounding_notes[to], s_song_sounding_notes[from], sizeof(tmp_notes));
    s_song_sounding_note_count[to] = s_song_sounding_note_count[from];

    /* Swap: `from` gets what `to` held. Move: that's all empty, which clears
     * `from`. */
    s_song_pattern[from] = tmp_pattern;
    s_song_slot_occupied[from] = tmp_occupied;
    s_song_slot_running[from] = tmp_running;
    s_song_slot_channel[from] = tmp_channel;
    s_song_current_step[from] = tmp_step;
    s_song_step_started_at_pulse[from] = tmp_pulse;
    s_song_note_sounding[from] = tmp_sounding;
    memcpy(s_song_sounding_notes[from], tmp_notes, sizeof(tmp_notes));
    s_song_sounding_note_count[from] = tmp_note_count;

    song_store_write_all();
    song_flash_confirm(to_pad, tmp_occupied ? from_pad : 0u, false);
    s_song_picked_up_slot = 0u;
}

/* Circle + hold 5 s deletes; a playing pattern is stopped and its channel
 * released first. */
#define OP_SONG_DELETE_HOLD_MS 5000u

static void song_delete_slot(uint8_t pad) {
    uint8_t slot = pad - 1u;
    if (s_song_slot_running[slot]) {
        song_end_current_note(slot);
        tiles_midi_channels_song_release(s_song_slot_channel[slot]);
        s_song_slot_running[slot] = false;
    }
    memset(&s_song_pattern[slot], 0, sizeof(s_song_pattern[slot]));
    s_song_slot_occupied[slot] = false;
    song_store_write_all();
    song_flash_confirm(pad, 0u, true);
}

static void song_toggle_start_stop(uint8_t pad) {
    uint8_t slot = pad - 1u;
    if (s_song_slot_running[slot]) {
        song_end_current_note(slot);
        tiles_midi_channels_song_release(s_song_slot_channel[slot]);
        s_song_slot_running[slot] = false;
        return;
    }
    uint8_t channel;
    if (!tiles_midi_channels_song_claim(&channel)) {
        /* Pool full: refuse with a red flash. */
        song_flash_error(pad);
        return;
    }
    /* The channel may have just carried a live MPE note, which keeps its bend
     * and pressure after Note-Off (MPE order; see
     * tiles_midi_send_note_setup()). Song notes send neither, so reset them
     * as the channel changes hands. */
    tiles_midi_send_note_setup(channel);
    s_song_slot_running[slot] = true;
    s_song_slot_channel[slot] = channel;
    /* Always starts from step 1 (no quantized start). */
    s_song_current_step[slot] = 0u;
}

/* Entering Ableton mode stops every Song track (from any mode, since
 * tracks loop in the background): Ableton mode and Song mode both drive
 * Live's clips, and it frees the shared channel pool for the session. */
static void song_stop_all_running(void) {
    for (uint8_t slot = 0u; slot < OP_SONG_NUM_SLOTS; slot++) {
        if (s_song_slot_running[slot]) {
            song_end_current_note(slot);
            tiles_midi_channels_song_release(s_song_slot_channel[slot]);
            s_song_slot_running[slot] = false;
        }
    }
}

static bool s_song_prev_pad_touched[OP_SONG_NUM_SLOTS];
/* Circle state recorded at touch-down (as in the pattern bank), so
 * letting go of circle mid-hold doesn't change the gesture. */
static bool s_song_touch_started_with_shift[OP_SONG_NUM_SLOTS];
static uint32_t s_song_touch_started_ms[OP_SONG_NUM_SLOTS];
static bool s_song_delete_fired[OP_SONG_NUM_SLOTS];
/* Hold 2 s opens the step editor; fires once per touch and cancels that
 * touch's start/stop toggle. */
#define OP_SONG_EDIT_HOLD_MS 2000u
static bool s_song_edit_fired[OP_SONG_NUM_SLOTS];

static void handle_song_overview_taps(uint32_t now_ms) {
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    for (uint8_t pad = 1u; pad <= OP_SONG_NUM_SLOTS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_song_prev_pad_touched[pad - 1u];
        if (touched && !was_touched) {
            tiles_haptics_trigger_touch_pulse(pad);
            s_song_touch_started_with_shift[pad - 1u] = circle_held;
            s_song_touch_started_ms[pad - 1u] = now_ms;
            s_song_delete_fired[pad - 1u] = false;
            s_song_edit_fired[pad - 1u] = false;
        }

        if (s_song_touch_started_with_shift[pad - 1u]) {
            /* Circle + touch: pick up or delete. While something is picked up, circle
             * + another pad is ignored (the place tap is a plain tap). */
            if (s_song_picked_up_slot != 0u) {
                s_song_prev_pad_touched[pad - 1u] = touched;
                continue;
            }
            if (touched) {
                uint32_t held_ms = now_ms - s_song_touch_started_ms[pad - 1u];
                if (held_ms >= OP_SONG_DELETE_HOLD_MS && !s_song_delete_fired[pad - 1u] &&
                    s_song_slot_occupied[pad - 1u]) {
                    song_delete_slot(pad);
                    s_song_delete_fired[pad - 1u] = true;
                }
            } else if (was_touched && !s_song_delete_fired[pad - 1u] && s_song_slot_occupied[pad - 1u]) {
                song_pick_up(pad);
            }
            s_song_prev_pad_touched[pad - 1u] = touched;
            continue;
        }

        if (touched && !was_touched) {
            if (s_song_picked_up_slot != 0u) {
                if (pad == s_song_picked_up_slot) {
                    song_cancel_pick_up();
                } else {
                    song_place(s_song_picked_up_slot, pad);
                }
            }
            /* Start/stop waits for release: a tap and a 2 s hold start the same way. */
        } else if (touched && was_touched) {
            if (s_song_picked_up_slot == 0u && !s_song_edit_fired[pad - 1u] && s_song_slot_occupied[pad - 1u]) {
                uint32_t held_ms = now_ms - s_song_touch_started_ms[pad - 1u];
                if (held_ms >= OP_SONG_EDIT_HOLD_MS) {
                    song_edit_enter(pad);
                    s_song_edit_fired[pad - 1u] = true;
                }
            }
        } else if (!touched && was_touched) {
            if (s_song_picked_up_slot == 0u && !s_song_edit_fired[pad - 1u] && s_song_slot_occupied[pad - 1u]) {
                song_toggle_start_stop(pad);
            }
        }
        s_song_prev_pad_touched[pad - 1u] = touched;
    }
}

static void render_song_overview(uint32_t now_ms) {
    bool flash_showing = false;
    bool flash_on = false;
    if (s_song_flash_active) {
        uint32_t elapsed = now_ms - s_song_flash_start_ms;
        if (elapsed >= OP_SONG_FLASH_TOTAL_MS) {
            s_song_flash_active = false;
        } else {
            flash_showing = true;
            flash_on = ((elapsed / OP_SONG_FLASH_BLINK_MS) % 2u) == 0u;
        }
    }
    bool error_showing = false;
    if (s_song_error_flash_active) {
        uint32_t elapsed = now_ms - s_song_error_flash_start_ms;
        if (elapsed >= OP_SONG_ERROR_FLASH_MS) {
            s_song_error_flash_active = false;
        } else {
            error_showing = true;
        }
    }
    float pick_up_pulse = menu_selected_pulse_level(now_ms);

    for (uint8_t pad = 1u; pad <= OP_SONG_NUM_SLOTS; pad++) {
        uint8_t slot = pad - 1u;
        if (flash_showing && (pad == s_song_flash_pad || pad == s_song_flash_pad2)) {
            float level = flash_on ? 1.0f : 0.0f;
            if (s_song_flash_is_delete) {
                tiles_lighting_set_standby_pad_rgb(pad, level, 0.0f, 0.0f);
            } else {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, level, 0.0f);
            }
        } else if (error_showing && pad == s_song_error_flash_pad) {
            tiles_lighting_set_standby_pad_rgb(pad, 1.0f, 0.0f, 0.0f);
        } else if (pad == s_song_picked_up_slot) {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, pick_up_pulse, 0.0f);
        } else if (!s_song_slot_occupied[slot]) {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        } else {
            float r, g, b;
            song_hue_to_rgb(s_song_pattern[slot].hue_byte, 1.0f, 1.0f, &r, &g, &b);
            /* Playing slots pulse in their own hue; stopped ones sit at the dim
             * "available" level. */
            float level = s_song_slot_running[slot] ? pick_up_pulse : OP_SCALE_AVAILABLE_LEVEL;
            tiles_lighting_set_standby_pad_rgb(pad, r * level, g * level, b * level);
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        /* Diamond shows the transport LED here (see transport_led_level()). */
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? transport_led_level(now_ms) : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    render_song_underglow();
}

/* Song mode's underglow: steady yellow. (While capturing, lighting.c's
 * amber capture pulse takes over through its priority chain, so this
 * isn't called then.) Also drawn during the step editor's pitch pick,
 * where the pads show the live melodic surface. */
static void render_song_underglow(void) {
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 1.0f, 1.0f, 0.0f);
    }
}

/* ---- Song mode: step editor -------------------------------------------------
 * Opened by holding a slot 2 s on the track overview; one pattern at a
 * time.
 *   - Layout: columns 1-4 = the page's 16 steps (row-major); columns 5-6 =
 *     the 8 page pads (see OP_SONG_STEPS_PER_PAGE). Pages with notes are
 *     lit, empty ones dim.
 *   - Tap a step to pick its notes: the whole grid becomes a live
 *     chromatic note surface (standby released, like Song capture).
 *     Tapping the same step again commits, REPLACING its notes (or
 *     clearing it if nothing was picked). A diamond click cancels.
 *   - Chords: pads touched within OP_SONG_EDIT_PICK_WINDOW_MS of the first
 *     form one chord (up to 4 notes); a later touch starts a new chord
 *     rather than adding to it. 200 ms is a first guess. */
#define OP_SONG_EDIT_PICK_WINDOW_MS 200u

static uint8_t s_song_edit_slot; /* 0-based, valid iff s_song_edit_active */
static uint8_t s_song_edit_page; /* 0-7, the page shown */
static bool s_song_edit_step_prev_touched[OP_SONG_STEPS_PER_PAGE];
static bool s_song_edit_page_prev_touched[OP_SONG_NUM_PAGES];
/* Edits mark the pattern dirty; the flash write happens once, when the
 * editor closes (song_edit_exit()), not after every step. Nothing is
 * written if nothing changed. */
static bool s_song_edit_dirty;

/* Valid only while s_song_edit_pick_active. */
static uint8_t s_song_edit_pick_step;        /* 0..127: the step in the whole pattern */
static uint8_t s_song_edit_pick_confirm_pad; /* the pad that commits this pick */
static uint8_t s_song_edit_pick_notes[OP_SONG_MAX_NOTES_PER_STEP];
static uint8_t s_song_edit_pick_count;
static bool s_song_edit_pick_window_active;
static uint32_t s_song_edit_pick_window_start_ms;
static bool s_song_edit_pick_prev_touched[TILES_NUM_PADS];
static tiles_scale_mode_t s_song_edit_pick_prev_scale;

static uint8_t song_edit_step_pad(uint8_t step_in_page) {
    uint8_t row = (uint8_t)(step_in_page / 4u + 1u);
    uint8_t col = (uint8_t)(step_in_page % 4u + 1u);
    return board_pad_for_row_col(row, col);
}

static uint8_t song_edit_page_pad(uint8_t page) {
    uint8_t row = (uint8_t)(page / 2u + 1u);
    uint8_t col = (page % 2u == 0u) ? 5u : 6u;
    return board_pad_for_row_col(row, col);
}

static bool song_edit_page_has_content(uint8_t slot, uint8_t page) {
    uint8_t base = (uint8_t)(page * OP_SONG_STEPS_PER_PAGE);
    for (uint8_t s = 0u; s < OP_SONG_STEPS_PER_PAGE; s++) {
        if (s_song_pattern[slot].step_notes[base + s][0] != 0xFFu) {
            return true;
        }
    }
    return false;
}

static void song_edit_enter(uint8_t pad) {
    s_song_edit_active = true;
    s_song_edit_slot = (uint8_t)(pad - 1u);
    s_song_edit_page = 0u;
    s_song_edit_pick_active = false;
    s_song_edit_dirty = false;
    for (uint8_t s = 0u; s < OP_SONG_STEPS_PER_PAGE; s++) {
        s_song_edit_step_prev_touched[s] = false;
    }
    for (uint8_t p = 0u; p < OP_SONG_NUM_PAGES; p++) {
        s_song_edit_page_prev_touched[p] = false;
    }
    /* The step grid is drawn through standby (mode_owns_standby_grid()); only
     * the pitch pick releases it. */
    printf("[op_mode] song edit -> on (slot %u)\n", (unsigned)(s_song_edit_slot + 1u));
}

static void song_edit_pick_enter(uint8_t step_index, uint8_t confirm_pad) {
    s_song_edit_pick_active = true;
    s_song_edit_pick_step = step_index;
    s_song_edit_pick_confirm_pad = confirm_pad;
    s_song_edit_pick_count = 0u;
    s_song_edit_pick_window_active = false;
    for (uint8_t i = 0u; i < TILES_NUM_PADS; i++) {
        s_song_edit_pick_prev_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    /* Chromatic while picking; the scale is restored on commit/cancel. */
    s_song_edit_pick_prev_scale = tiles_note_map_get_scale();
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);
    /* Release standby so the grid shows the live note surface (see
     * mode_owns_standby_grid()). */
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
}

static void song_edit_pick_cancel(void) {
    if (!s_song_edit_pick_active) {
        return;
    }
    s_song_edit_pick_active = false;
    tiles_note_map_set_scale(s_song_edit_pick_prev_scale);
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void song_edit_pick_commit(void) {
    op_song_pattern_t *pat = &s_song_pattern[s_song_edit_slot];
    for (uint8_t i = 0u; i < OP_SONG_MAX_NOTES_PER_STEP; i++) {
        pat->step_notes[s_song_edit_pick_step][i] = (i < s_song_edit_pick_count) ? s_song_edit_pick_notes[i] : 0xFFu;
    }
    /* Flash write deferred to song_edit_exit() (see s_song_edit_dirty). */
    s_song_edit_dirty = true;
    s_song_edit_pick_active = false;
    tiles_note_map_set_scale(s_song_edit_pick_prev_scale);
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void song_edit_exit(void) {
    if (s_song_edit_pick_active) {
        song_edit_pick_cancel();
    }
    if (s_song_edit_dirty) {
        song_store_write_all();
        s_song_edit_dirty = false;
    }
    s_song_edit_active = false;
    printf("[op_mode] song edit -> off\n");
}

static void handle_song_edit_taps(uint32_t now_ms) {
    (void)now_ms;
    uint8_t page_base = (uint8_t)(s_song_edit_page * OP_SONG_STEPS_PER_PAGE);

    for (uint8_t s = 0u; s < OP_SONG_STEPS_PER_PAGE; s++) {
        uint8_t pad = song_edit_step_pad(s);
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_song_edit_step_prev_touched[s];
        if (touched && !was) {
            tiles_haptics_trigger_touch_pulse(pad);
            song_edit_pick_enter((uint8_t)(page_base + s), pad);
        }
        s_song_edit_step_prev_touched[s] = touched;
    }
    for (uint8_t p = 0u; p < OP_SONG_NUM_PAGES; p++) {
        uint8_t pad = song_edit_page_pad(p);
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_song_edit_page_prev_touched[p];
        if (touched && !was) {
            tiles_haptics_trigger_touch_pulse(pad);
            s_song_edit_page = p;
        }
        s_song_edit_page_prev_touched[p] = touched;
    }
}

static void handle_song_edit_pick_taps(uint32_t now_ms) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was = s_song_edit_pick_prev_touched[pad - 1u];
        if (touched && !was) {
            tiles_haptics_trigger_touch_pulse(pad);
            if (pad == s_song_edit_pick_confirm_pad) {
                song_edit_pick_commit();
                return; /* pick just ended; later pads' prev_touched are re-seeded on the next pick */
            }
            uint8_t note = tiles_note_map_get_note(pad);
            if (!s_song_edit_pick_window_active ||
                (now_ms - s_song_edit_pick_window_start_ms) > OP_SONG_EDIT_PICK_WINDOW_MS) {
                /* Window expired (or first touch): start a fresh chord. */
                s_song_edit_pick_count = 0u;
                s_song_edit_pick_window_active = true;
                s_song_edit_pick_window_start_ms = now_ms;
            }
            bool already_picked = false;
            for (uint8_t i = 0u; i < s_song_edit_pick_count; i++) {
                if (s_song_edit_pick_notes[i] == note) {
                    already_picked = true;
                    break;
                }
            }
            if (!already_picked && s_song_edit_pick_count < OP_SONG_MAX_NOTES_PER_STEP) {
                s_song_edit_pick_notes[s_song_edit_pick_count] = note;
                s_song_edit_pick_count++;
            }
        }
        s_song_edit_pick_prev_touched[pad - 1u] = touched;
    }
}

static void render_song_edit(uint32_t now_ms) {
    uint8_t slot = s_song_edit_slot;
    float r, g, b;
    song_hue_to_rgb(s_song_pattern[slot].hue_byte, 1.0f, 1.0f, &r, &g, &b);
    float pulse = menu_selected_pulse_level(now_ms);
    uint8_t page_base = (uint8_t)(s_song_edit_page * OP_SONG_STEPS_PER_PAGE);

    for (uint8_t s = 0u; s < OP_SONG_STEPS_PER_PAGE; s++) {
        uint8_t pad = song_edit_step_pad(s);
        bool armed = s_song_pattern[slot].step_notes[page_base + s][0] != 0xFFu;
        if (armed) {
            tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    for (uint8_t p = 0u; p < OP_SONG_NUM_PAGES; p++) {
        uint8_t pad = song_edit_page_pad(p);
        if (p == s_song_edit_page) {
            tiles_lighting_set_standby_pad_rgb(pad, pulse, pulse, pulse);
        } else if (song_edit_page_has_content(slot, p)) {
            tiles_lighting_set_standby_pad_rgb(pad, r * OP_SCALE_AVAILABLE_LEVEL, g * OP_SCALE_AVAILABLE_LEVEL,
                                                b * OP_SCALE_AVAILABLE_LEVEL);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        /* Diamond shows the transport LED here (see transport_led_level()). */
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? transport_led_level(now_ms) : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    render_song_underglow();
}

/* ---- Song mode: playback --------------------------------------------------
 * A separate copy of the sequencer's engine rather than a shared one: Song
 * patterns (128 fixed steps, no probability/ratchet/length) differ
 * enough. Same OP_SEQ_CLOCKS_PER_STEP, so both follow one tempo. */
#define OP_SONG_VELOCITY 100u
static bool s_song_pending_start[OP_SONG_NUM_SLOTS];

static void song_enter_step(uint8_t slot, uint8_t step) {
    song_end_current_note(slot);
    s_song_current_step[slot] = step;
    uint8_t notes[OP_SONG_MAX_NOTES_PER_STEP];
    uint8_t count = 0u;
    for (uint8_t i = 0; i < OP_SONG_MAX_NOTES_PER_STEP; i++) {
        uint8_t note = s_song_pattern[slot].step_notes[step][i];
        if (note != 0xFFu) {
            notes[count] = note;
            count++;
        }
    }
    if (count == 0u) {
        return;
    }
    for (uint8_t i = 0; i < count; i++) {
        tiles_midi_note_on(s_song_slot_channel[slot], notes[i], OP_SONG_VELOCITY);
        tiles_cv_gate_note_on(notes[i], OP_SONG_VELOCITY);
        s_song_sounding_notes[slot][i] = notes[i];
    }
    s_song_sounding_note_count[slot] = count;
    s_song_note_sounding[slot] = true;
}

/* Runs for every slot every scan, except the one being captured. Like
 * seq_advance_clock(), without length or restart/resume handling. */
static void song_advance_clock(uint8_t slot, tiles_midi_clock_state_t clock) {
    if (!s_song_slot_running[slot]) {
        song_end_current_note(slot);
        return;
    }
    if (s_song_pending_start[slot]) {
        s_song_pending_start[slot] = false;
        s_song_step_started_at_pulse[slot] = clock.pulse_count;
        song_enter_step(slot, 0u);
        return;
    }
    if (!clock.running) {
        song_end_current_note(slot);
        return;
    }
    uint32_t elapsed = clock.pulse_count - s_song_step_started_at_pulse[slot];
    if (elapsed < OP_SEQ_CLOCKS_PER_STEP) {
        return;
    }
    uint32_t steps_to_advance = elapsed / OP_SEQ_CLOCKS_PER_STEP;
    s_song_step_started_at_pulse[slot] += steps_to_advance * OP_SEQ_CLOCKS_PER_STEP;
    uint8_t new_step = (uint8_t)((s_song_current_step[slot] + steps_to_advance) % OP_SONG_NUM_STEPS);
    song_enter_step(slot, new_step);
}

/* ---- Song mode: capture ------------------------------------------------------
 * Circle + diamond from melodic, chord, bass guitar or Song mode records
 * into the next empty Song slot. Works like sequencer capture:
 * nearest-step quantization, a cluster committed at the next step
 * boundary, live notes that sound independently, and chord pads captured
 * on their real strike (velocity and voicing from handle_chord_pad_taps()). */
static tiles_scale_mode_t s_song_capture_prev_scale;
static bool s_song_capture_prev_pad_touched[TILES_NUM_PADS];
static uint8_t s_song_capture_armed_notes[OP_SONG_MAX_NOTES_PER_STEP];
static uint8_t s_song_capture_armed_count;
static uint8_t s_song_capture_target_step;
static uint8_t s_song_capture_live_pads[OP_SONG_MAX_NOTES_PER_STEP];
static uint8_t s_song_capture_live_notes[OP_SONG_MAX_NOTES_PER_STEP];
static uint8_t s_song_capture_live_count;

static void song_capture_end_one_sounding_note(uint8_t pad) {
    uint8_t slot = s_song_capture_slot - 1u;
    for (uint8_t i = 0; i < s_song_capture_live_count; i++) {
        if (s_song_capture_live_pads[i] == pad) {
            tiles_midi_note_off(s_song_slot_channel[slot], s_song_capture_live_notes[i], 0u); /* ended by release/exit: release velocity 0 */
            tiles_cv_gate_note_off(s_song_capture_live_notes[i]);
            for (uint8_t j = i; (uint8_t)(j + 1u) < s_song_capture_live_count; j++) {
                s_song_capture_live_pads[j] = s_song_capture_live_pads[j + 1u];
                s_song_capture_live_notes[j] = s_song_capture_live_notes[j + 1u];
            }
            s_song_capture_live_count--;
            return;
        }
    }
}

static void song_capture_end_all_sounding_notes(void) {
    uint8_t slot = s_song_capture_slot - 1u;
    for (uint8_t i = 0; i < s_song_capture_live_count; i++) {
        tiles_midi_note_off(s_song_slot_channel[slot], s_song_capture_live_notes[i], 0u); /* ended by exit: release velocity 0 */
        tiles_cv_gate_note_off(s_song_capture_live_notes[i]);
    }
    s_song_capture_live_count = 0u;
}

bool tiles_op_mode_song_capture_is_active(void) {
    return s_song_capture_active;
}

/* Reads the same sounding-note state song_end_current_note() uses, so it
 * can't drift from what's actually playing. */
bool tiles_op_mode_song_capture_is_note_sounding(uint8_t note) {
    if (!s_song_capture_active) {
        return false;
    }
    uint8_t slot = s_song_capture_slot - 1u;
    if (!s_song_note_sounding[slot]) {
        return false;
    }
    for (uint8_t i = 0; i < s_song_sounding_note_count[slot]; i++) {
        if (s_song_sounding_notes[slot][i] == note) {
            return true;
        }
    }
    return false;
}

/* Chord pads: capture on the strike edge, as in sequencer capture. */
static bool s_song_capture_prev_chord_sounding[TILES_NUM_PADS];

static void song_capture_handle_taps(tiles_midi_clock_state_t clock) {
    uint8_t slot = s_song_capture_slot - 1u;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_song_capture_prev_pad_touched[pad - 1u];
        bool is_chord_pad = tiles_note_map_is_chord_mode_active() && tiles_note_map_is_chord_region_pad(pad);
        bool chord_strike_edge =
            is_chord_pad && s_chord_pad_sounding[pad - 1u] && !s_song_capture_prev_chord_sounding[pad - 1u];
        if ((touched && !was_touched && !is_chord_pad) || chord_strike_edge) {
            uint8_t notes[OP_SONG_MAX_NOTES_PER_STEP];
            uint8_t note_count;
            uint8_t velocity;
            if (is_chord_pad) {
                note_count = (OP_SONG_MAX_NOTES_PER_STEP < OP_CHORD_NUM_VOICES) ? OP_SONG_MAX_NOTES_PER_STEP
                                                                                 : OP_CHORD_NUM_VOICES;
                for (uint8_t i = 0; i < note_count; i++) {
                    notes[i] = s_chord_pad_notes[pad - 1u][i];
                }
                velocity = s_chord_pad_last_velocity[pad - 1u];
            } else {
                notes[0] = tiles_note_map_get_note(pad);
                note_count = 1u;
                velocity = OP_SONG_VELOCITY;
            }
            tiles_haptics_trigger_kick(pad, velocity);
            for (uint8_t i = 0; i < note_count; i++) {
                tiles_midi_note_on(s_song_slot_channel[slot], notes[i], velocity);
                tiles_cv_gate_note_on(notes[i], velocity);
                if (s_song_capture_live_count < OP_SONG_MAX_NOTES_PER_STEP) {
                    s_song_capture_live_pads[s_song_capture_live_count] = pad;
                    s_song_capture_live_notes[s_song_capture_live_count] = notes[i];
                    s_song_capture_live_count++;
                }
            }
            uint32_t elapsed_in_step = clock.pulse_count - s_song_step_started_at_pulse[slot];
            bool nearest_is_next_step = (elapsed_in_step * 2u) >= OP_SEQ_CLOCKS_PER_STEP;
            uint8_t target = nearest_is_next_step ? (uint8_t)((s_song_current_step[slot] + 1u) % OP_SONG_NUM_STEPS)
                                                   : s_song_current_step[slot];
            if (s_song_capture_armed_count == 0u || s_song_capture_target_step != target) {
                s_song_capture_target_step = target;
                s_song_capture_armed_count = 0u;
            }
            for (uint8_t i = 0; i < note_count; i++) {
                if (s_song_capture_armed_count < OP_SONG_MAX_NOTES_PER_STEP) {
                    s_song_capture_armed_notes[s_song_capture_armed_count] = notes[i];
                    s_song_capture_armed_count++;
                }
            }
        } else if (!touched && was_touched) {
            song_capture_end_one_sounding_note(pad);
        }
        s_song_capture_prev_pad_touched[pad - 1u] = touched;
        s_song_capture_prev_chord_sounding[pad - 1u] = is_chord_pad && s_chord_pad_sounding[pad - 1u];
    }
}

static void song_capture_advance_clock(tiles_midi_clock_state_t clock) {
    uint8_t slot = s_song_capture_slot - 1u;
    if (clock.start_edge) {
        s_song_current_step[slot] = 0u;
        s_song_step_started_at_pulse[slot] = clock.pulse_count;
        s_song_capture_armed_count = 0u;
        s_song_pending_start[slot] = false;
        return;
    }
    if (s_song_pending_start[slot]) {
        s_song_pending_start[slot] = false;
        s_song_current_step[slot] = 0u;
        s_song_step_started_at_pulse[slot] = clock.pulse_count;
        s_song_capture_armed_count = 0u;
        return;
    }
    uint32_t elapsed = clock.pulse_count - s_song_step_started_at_pulse[slot];
    if (elapsed < OP_SEQ_CLOCKS_PER_STEP) {
        return;
    }
    uint32_t steps_to_advance = elapsed / OP_SEQ_CLOCKS_PER_STEP;
    s_song_step_started_at_pulse[slot] += steps_to_advance * OP_SEQ_CLOCKS_PER_STEP;

    if (s_song_capture_armed_count > 0u && s_song_capture_target_step == s_song_current_step[slot]) {
        uint8_t step = s_song_current_step[slot];
        uint8_t count = s_song_capture_armed_count;
        if (count > OP_SONG_MAX_NOTES_PER_STEP) {
            count = OP_SONG_MAX_NOTES_PER_STEP;
        }
        for (uint8_t i = 0; i < OP_SONG_MAX_NOTES_PER_STEP; i++) {
            s_song_pattern[slot].step_notes[step][i] = (i < count) ? s_song_capture_armed_notes[i] : 0xFFu;
        }
        s_song_capture_armed_count = 0u;
    }
    s_song_current_step[slot] = (uint8_t)((s_song_current_step[slot] + steps_to_advance) % OP_SONG_NUM_STEPS);
}

static bool song_find_next_empty_slot(uint8_t *out_slot_index) {
    for (uint8_t i = 0; i < OP_SONG_NUM_SLOTS; i++) {
        if (!s_song_slot_occupied[i]) {
            *out_slot_index = i;
            return true;
        }
    }
    return false;
}

/* No empty slot, or no free channel: silently do nothing (there's no
 * single pad to flash the error on). */
/* Each new pattern's hue is the previous one + 97 (wrapping), not random:
 * random picks could land nearly the same color back to back. 97 is odd,
 * so it visits all 256 values before repeating, and it's close to the
 * golden-angle fraction (~97.8), which keeps successive colors clearly
 * apart. The counter is persisted. */
#define OP_SONG_HUE_STEP 97u

static void song_capture_enter(void) {
    uint8_t slot_index;
    if (!song_find_next_empty_slot(&slot_index)) {
        return;
    }
    uint8_t channel;
    if (!tiles_midi_channels_song_claim(&channel)) {
        return;
    }
    tiles_midi_send_note_setup(channel); /* same channel reset as the slot-start path */
    memset(s_song_pattern[slot_index].step_notes, 0xFF, sizeof(s_song_pattern[slot_index].step_notes));
    s_song_pattern[slot_index].hue_byte = s_song_next_hue_byte;
    s_song_next_hue_byte = (uint8_t)(s_song_next_hue_byte + OP_SONG_HUE_STEP);
    s_song_slot_occupied[slot_index] = true;
    s_song_slot_running[slot_index] = true;
    s_song_slot_channel[slot_index] = channel;
    s_song_note_sounding[slot_index] = false;
    s_song_sounding_note_count[slot_index] = 0u;
    s_song_pending_start[slot_index] = true;

    s_song_capture_active = true;
    s_song_capture_slot = (uint8_t)(slot_index + 1u);
    /* Chromatic while capturing; restored on exit. */
    s_song_capture_prev_scale = tiles_note_map_get_scale();
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);
    s_song_capture_armed_count = 0u;
    s_song_capture_live_count = 0u;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_song_capture_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        /* A chord pad already sounding isn't a new strike. */
        s_song_capture_prev_chord_sounding[i] = s_chord_pad_sounding[i];
    }
    /* Release standby while capturing, so the grid shows the live melodic
     * surface notes are played on. Re-claimed on exit if needed. */
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
    printf("[op_mode] song capture -> on (slot %u)\n", (unsigned)s_song_capture_slot);
}

static void song_capture_exit(void) {
    if (!s_song_capture_active) {
        return;
    }
    song_capture_end_all_sounding_notes();
    s_song_capture_active = false;
    tiles_note_map_set_scale(s_song_capture_prev_scale);
    /* Re-claim standby only if the current view draws through it
     * (mode_owns_standby_grid()); e.g. a Song pitch pick still open needs it
     * released. */
    if (mode_owns_standby_grid(s_active_mode)) {
        tiles_lighting_set_standby_active(true);
        tiles_buttons_set_standby_active(true);
    } else {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    song_store_write_all();
    printf("[op_mode] song capture -> off\n");
}

/* ---- Scene Launch (Ableton) mode --------------------------------------------
 * A Session View grid: rows 1-4 = Live's first 4 scenes; columns 1-5 = 5
 * tracks' clip slots ("-"/"+" pan the tracks; scenes don't page);
 * column 6 = launch that whole scene.
 *
 * Clip and scene state (color, has clip, playing, triggered) comes from
 * the TILES control surface script over SysEx (daw-integration/ableton/
 * TILES/scene_launch.py; shared/protocol/README.md "Scene Launch"), with
 * Live's real RGB colors:
 *   - empty slot: off
 *   - clip, not playing: its color at the dim "available" level
 *   - playing: its color, strong pulse
 *   - triggered (queued): fast blink
 * Column 6 is Sentia magenta while any track has a clip in that scene
 * (blinks while queued).
 * Underglow: teal at rest; flashes Sentia magenta on a scene launch, or the
 * clip's color on a clip action. */

/* Manufacturer ID from midi/product_identity.h (0x7D until SENTIA has its
 * own). The sub-ID scopes these messages under it. */
#define OP_SCENE_SYSEX_MFR_ID TILES_SYSEX_MANUFACTURER_ID
#define OP_SCENE_SYSEX_SUB_ID 0x01u

/* Live -> TILES only. TILES -> Live uses CCs (see OP_SCENE_CC_*). */
#define OP_SCENE_MSG_CLIP_STATE 0x10u
#define OP_SCENE_MSG_SCENE_STATE 0x11u
/* Sent by the script after a click on an EMPTY slot armed the track and
 * started recording, only if the track takes MIDI (only Live knows). See
 * s_scene_pending_melodic. */
#define OP_SCENE_MSG_OPEN_MELODIC 0x12u

/* CLIP_STATE/SCENE_STATE flag bits (one 7-bit byte). SCENE_STATE uses
 * only IS_TRIGGERED (a scene has no playing state of its own). */
#define OP_SCENE_FLAG_HAS_CLIP 0x01u
#define OP_SCENE_FLAG_IS_PLAYING 0x02u
#define OP_SCENE_FLAG_IS_TRIGGERED 0x04u

#define OP_SCENE_NUM_ROWS 4u
#define OP_SCENE_LAUNCH_COL 6u

typedef struct {
    bool has_clip;
    bool is_playing;
    bool is_triggered;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} op_scene_cell_state_t;

/* [track][scene] for every track heard about, not just the visible
 * window, so panning never loses state. The script sends every
 * track/scene on connect. */
static op_scene_cell_state_t s_scene_clip[OP_SCENE_MAX_TRACKS][OP_SCENE_NUM_ROWS];

typedef struct {
    bool is_triggered;
    uint8_t r;
    uint8_t g;
    uint8_t b;
} op_scene_row_state_t;

static op_scene_row_state_t s_scene_row[OP_SCENE_NUM_ROWS];

/* Two inputs per pad:
 *   - capacitive TOUCH: haptics only, never sends anything to Live
 *     (scene_update_haptics()).
 *   - PRESSURE CLICK (depth past OP_SCENE_CLICK_DEPTH_THRESHOLD, the menus'
 *     "more than half" press): the only thing that acts in Live
 *     (scene_handle_click()).
 * The re-arm depth sits well below the click so jitter can't double-fire. */
#define OP_SCENE_CLICK_DEPTH_THRESHOLD OP_MENU_SELECT_DEPTH_THRESHOLD
#define OP_SCENE_CLICK_REARM_DEPTH 250.0f
static bool s_scene_prev_pad_touched[TILES_NUM_PADS];
static bool s_scene_click_latched[TILES_NUM_PADS];

/* Circle held while touching a pad with a clip for
 * OP_SCENE_DELETE_HOLD_MS deletes the clip in Live (the same 3 s as the
 * pattern bank). Releasing either cancels. The pad blinks red and the
 * underglow goes red during the hold, and stays red briefly after. */
#define OP_SCENE_DELETE_HOLD_MS 3000u
#define OP_SCENE_DELETE_CONFIRM_MS 500u
static bool s_scene_delete_holding[TILES_NUM_PADS];
static bool s_scene_delete_fired[TILES_NUM_PADS];
static uint32_t s_scene_delete_hold_start_ms[TILES_NUM_PADS];
static bool s_scene_delete_confirm_active;
static uint32_t s_scene_delete_confirm_until_ms;

/* Per-pad haptic state: NONE; CLICK = the one-shot "ready" kick, cut at
 * s_scene_haptic_click_end_ms (a kick would roll into a sustain buzz);
 * SUSTAINING = the clip is playing and the buzz lasts while the finger
 * rests. */
typedef enum {
    OP_SCENE_HAPTIC_NONE = 0,
    OP_SCENE_HAPTIC_CLICK,
    OP_SCENE_HAPTIC_SUSTAINING,
} op_scene_haptic_state_t;
static op_scene_haptic_state_t s_scene_haptic_state[TILES_NUM_PADS];
static uint32_t s_scene_haptic_click_end_ms[TILES_NUM_PADS];

/* Touching a pad with a clip: a strong "ready" click. 127 = full-duty
 * kick. OP_SCENE_HAPTIC_CLICK_MS outlasts the kick + gap (45 + 8 ms) and
 * cuts the voice before sustain builds. */
#define OP_SCENE_HAPTIC_CLICK_VELOCITY 127u
#define OP_SCENE_HAPTIC_CLICK_MS 56u
/* Touching a playing clip: a steady buzz (a fixed stand-in for pressure;
 * touch carries none). First guess. */
#define OP_SCENE_HAPTIC_PLAYING_LEVEL 90u
/* Kick when a clip STARTS playing under a resting finger (after a
 * quantized launch), opening the voice for the buzz. */
#define OP_SCENE_HAPTIC_PLAYING_ONSET_VELOCITY 100u

/* One underglow flash per action: Sentia magenta for a scene launch, the
 * clip's own color for a clip action. */
#define OP_SCENE_TRIGGER_FLASH_MS 200u
static bool s_scene_trigger_flash_active;
static uint32_t s_scene_trigger_flash_start_ms;
static float s_scene_flash_r;
static float s_scene_flash_g;
static float s_scene_flash_b;

/* Queued clips blink fast and plain, distinct from the playing pulse
 * ("about to change" vs "active"), as Launchpad-style scripts do. First
 * guess at pacing. */
#define OP_SCENE_TRIGGERED_BLINK_PERIOD_MS 250u

static float scene_triggered_blink_level(uint32_t now_ms) {
    uint32_t phase = now_ms % OP_SCENE_TRIGGERED_BLINK_PERIOD_MS;
    return (phase < OP_SCENE_TRIGGERED_BLINK_PERIOD_MS / 2u) ? 1.0f : 0.2f;
}

/* Playing clips: a strong near-off-to-full pulse, faster than the menu
 * pulse, so "live" reads at a glance. The low end stays barely lit so the
 * color stays visible. First guess. */
#define OP_SCENE_PLAYING_PULSE_PERIOD_MS 600.0f
#define OP_SCENE_PLAYING_PULSE_MIN 0.15f
#define OP_SCENE_PLAYING_PULSE_MAX 1.0f

static float scene_playing_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / OP_SCENE_PLAYING_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
    return OP_SCENE_PLAYING_PULSE_MIN + (OP_SCENE_PLAYING_PULSE_MAX - OP_SCENE_PLAYING_PULSE_MIN) * raw;
}

/* TILES -> Live is CCs on channel 1, DAW port, handled by the control
 * surface script (TILES.py), like the transport CCs. Not SysEx (never
 * delivered reliably) and not notes (the same USB port also carries
 * playable notes, so a note would also reach the instrument track).
 *
 * Pad actions are ONE CC whose value is the pad (1-24), then 0:
 * grid (fire a clip / launch a scene), stop (column 1-5), delete. All
 * Scene Launch CCs are 105-110, in the MIDI spec's undefined range. Never
 * use performance CCs: per-pad CCs once used 11-94, so CC 64 (pad 24's
 * stop) was claimed by the script and the sustain pedal never reached
 * the instrument. */
#define OP_SCENE_CC_MASTER_STOP 105u
#define OP_SCENE_CC_TRACK_OFFSET 106u
#define OP_SCENE_CC_END_CAPTURE 107u
#define OP_SCENE_CC_GRID_TOUCH 108u
#define OP_SCENE_CC_STOP_TOUCH 109u
#define OP_SCENE_CC_DELETE_TOUCH 110u

/* One pad event: `cc` = pad, then 0 (so two taps of the same pad aren't
 * two identical values in a row). */
static void scene_send_pad_event(uint8_t cc, uint8_t pad) {
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, cc, pad);
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, cc, 0u);
}

static void scene_send_grid_touch(uint8_t pad) {
    scene_send_pad_event(OP_SCENE_CC_GRID_TOUCH, pad);
}

/* Circle + diamond in Ableton mode: stop all clips (separate from the
 * diamond's transport Stop). */
static void scene_send_stop_all(void) {
    printf("[op_mode] scene launch: shift+diamond -> stop all clips\n");
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_SCENE_CC_MASTER_STOP, 127u);
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_SCENE_CC_MASTER_STOP, 0u);
}

/* No printf before the send (blocking stdio; see services/pedal.c). */
static void scene_send_stop_clip_cc(uint8_t pad) {
    scene_send_pad_event(OP_SCENE_CC_STOP_TOUCH, pad);
}

/* Tells the script which 5-track window is shown, so Live's session ring
 * (and its pad-to-track mapping) follows. */
static void scene_send_track_offset(uint8_t offset) {
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_SCENE_CC_TRACK_OFFSET, offset);
}

/* Delete that pad's clip. Track columns only. No printf before the send. */
static void scene_send_delete_clip(uint8_t pad) {
    scene_send_pad_event(OP_SCENE_CC_DELETE_TOUCH, pad);
}

/* True if ANY track (not just the visible 5) has a clip in this scene. */
static bool scene_row_has_clips(uint8_t row) {
    for (uint8_t track = 0u; track < OP_SCENE_MAX_TRACKS; track++) {
        if (s_scene_clip[track][row].has_clip) {
            return true;
        }
    }
    return false;
}

/* Ends Live's capture and returns to Ableton mode: tells Live first (the
 * script remembers which slot it's recording), then switches here, so one
 * gesture does both. */
static void scene_end_capture(void) {
    printf("[op_mode] ableton capture: shift+diamond -> end capture, back to scene launch\n");
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_SCENE_CC_END_CAPTURE, 127u);
    tiles_midi_send_daw_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_SCENE_CC_END_CAPTURE, 0u);
    set_active_mode(OP_MODE_SCENE_LAUNCH);
}

/* midi_in's SysEx callback. Every SysEx arrives here, so the manufacturer
 * and sub-ID checks matter. Payload lengths must match exactly (malformed
 * or newer messages are ignored). DAW port only: that's where the script
 * is. */
static void scene_on_sysex(tiles_midi_port_t port, const uint8_t *data, size_t len) {
    if (port != TILES_MIDI_PORT_DAW) {
        return;
    }
    if (len < 3u || data[0] != OP_SCENE_SYSEX_MFR_ID || data[1] != OP_SCENE_SYSEX_SUB_ID) {
        return;
    }
    uint8_t msg_type = data[2];
    if (msg_type == OP_SCENE_MSG_CLIP_STATE) {
        if (len != 9u) {
            return;
        }
        uint8_t track = data[3];
        uint8_t scene = data[4];
        if (track >= OP_SCENE_MAX_TRACKS || scene >= OP_SCENE_NUM_ROWS) {
            return;
        }
        uint8_t flags = data[5];
        op_scene_cell_state_t *cell = &s_scene_clip[track][scene];
        cell->has_clip = (flags & OP_SCENE_FLAG_HAS_CLIP) != 0u;
        cell->is_playing = (flags & OP_SCENE_FLAG_IS_PLAYING) != 0u;
        cell->is_triggered = (flags & OP_SCENE_FLAG_IS_TRIGGERED) != 0u;
        /* 7-bit wire values doubled back toward 8-bit; the lost bit doesn't
         * matter for these LEDs. */
        cell->r = (uint8_t)(data[6] * 2u);
        cell->g = (uint8_t)(data[7] * 2u);
        cell->b = (uint8_t)(data[8] * 2u);
    } else if (msg_type == OP_SCENE_MSG_SCENE_STATE) {
        if (len != 8u) {
            return;
        }
        uint8_t scene = data[3];
        if (scene >= OP_SCENE_NUM_ROWS) {
            return;
        }
        uint8_t flags = data[4];
        op_scene_row_state_t *row = &s_scene_row[scene];
        row->is_triggered = (flags & OP_SCENE_FLAG_IS_TRIGGERED) != 0u;
        row->r = (uint8_t)(data[5] * 2u);
        row->g = (uint8_t)(data[6] * 2u);
        row->b = (uint8_t)(data[7] * 2u);
    } else if (msg_type == OP_SCENE_MSG_OPEN_MELODIC) {
        if (len != 3u) {
            return;
        }
        /* Only while Ableton mode is on screen, so a late message can't switch
         * modes after the player left. */
        if (s_active_mode == OP_MODE_SCENE_LAUNCH) {
            printf("[op_mode] scene launch: Ableton armed a track -> melodic mode once pads are released\n");
            s_scene_pending_melodic = true;
        }
    }
    /* Unknown message types are ignored (forward compatible). */
}

static void scene_launch_init(void) {
    for (uint8_t track = 0u; track < OP_SCENE_MAX_TRACKS; track++) {
        for (uint8_t row = 0u; row < OP_SCENE_NUM_ROWS; row++) {
            s_scene_clip[track][row] = (op_scene_cell_state_t){0};
        }
    }
    for (uint8_t row = 0u; row < OP_SCENE_NUM_ROWS; row++) {
        s_scene_row[row] = (op_scene_row_state_t){0};
    }
    for (uint8_t pad = 0u; pad < TILES_NUM_PADS; pad++) {
        s_scene_prev_pad_touched[pad] = false;
        s_scene_click_latched[pad] = false;
        s_scene_haptic_state[pad] = OP_SCENE_HAPTIC_NONE;
        s_scene_delete_holding[pad] = false;
        s_scene_delete_fired[pad] = false;
    }
    s_scene_delete_confirm_active = false;
    s_scene_track_offset = 0u;
    s_scene_pending_melodic = false;
    tiles_midi_in_register_sysex_callback(scene_on_sysex);
}

/* Called on entering Ableton mode. Seed each pad's click latch from what's
 * pressed now: the finger that picked the mode is still on the menu's
 * column-6 pad, which here is scene 1's launch pad. Also sends the current
 * track window so Live shows the outline right away. */
static void scene_launch_enter(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        s_scene_prev_pad_touched[pad - 1u] = touched;
        s_scene_click_latched[pad - 1u] = touched;
        s_scene_haptic_state[pad - 1u] = OP_SCENE_HAPTIC_NONE;
        s_scene_delete_holding[pad - 1u] = false;
        s_scene_delete_fired[pad - 1u] = false;
    }
    s_scene_delete_confirm_active = false;
    scene_send_track_offset(s_scene_track_offset);
}

/* Called on leaving Ableton mode: stop every haptic voice this mode
 * started (a playing clip's buzz is held open while a finger rests). */
static void scene_launch_leave(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (s_scene_haptic_state[pad - 1u] != OP_SCENE_HAPTIC_NONE) {
            tiles_haptics_stop(pad);
            s_scene_haptic_state[pad - 1u] = OP_SCENE_HAPTIC_NONE;
        }
    }
}

static void scene_start_flash(float r, float g, float b, uint32_t now_ms) {
    s_scene_flash_r = r;
    s_scene_flash_g = g;
    s_scene_flash_b = b;
    s_scene_trigger_flash_active = true;
    s_scene_trigger_flash_start_ms = now_ms;
}

/* Touch -> haptics only, never Live. `has_content`: something launchable
 * under the pad (a clip, or a scene with clips); empty slots get no
 * haptics. `playing`: a track column whose clip is playing. The click is a
 * kick cut on a timer; a playing clip keeps that voice as a steady buzz. */
static void scene_update_haptics(uint8_t pad, bool touched, bool was_touched, bool has_content, bool playing,
                                 uint32_t now_ms) {
    uint8_t idx = (uint8_t)(pad - 1u);
    op_scene_haptic_state_t state = s_scene_haptic_state[idx];

    if (!touched) {
        if (state != OP_SCENE_HAPTIC_NONE) {
            tiles_haptics_stop(pad);
            s_scene_haptic_state[idx] = OP_SCENE_HAPTIC_NONE;
        }
        return;
    }

    if (!was_touched && has_content) {
        tiles_haptics_trigger_kick(pad, OP_SCENE_HAPTIC_CLICK_VELOCITY);
        if (playing) {
            tiles_haptics_set_sustain_level(pad, OP_SCENE_HAPTIC_PLAYING_LEVEL);
            s_scene_haptic_state[idx] = OP_SCENE_HAPTIC_SUSTAINING;
        } else {
            s_scene_haptic_click_end_ms[idx] = now_ms + OP_SCENE_HAPTIC_CLICK_MS;
            s_scene_haptic_state[idx] = OP_SCENE_HAPTIC_CLICK;
        }
        return;
    }

    state = s_scene_haptic_state[idx];
    if (playing) {
        if (state == OP_SCENE_HAPTIC_NONE) {
            /* The clip just started under a resting finger: open a voice for the
             * buzz. */
            tiles_haptics_trigger_kick(pad, OP_SCENE_HAPTIC_PLAYING_ONSET_VELOCITY);
        }
        if (state != OP_SCENE_HAPTIC_SUSTAINING) {
            s_scene_haptic_state[idx] = OP_SCENE_HAPTIC_SUSTAINING;
        }
        tiles_haptics_set_sustain_level(pad, OP_SCENE_HAPTIC_PLAYING_LEVEL);
    } else if (state == OP_SCENE_HAPTIC_SUSTAINING) {
        /* Stopped (or replaced) under a resting finger: the buzz ends. */
        tiles_haptics_stop(pad);
        s_scene_haptic_state[idx] = OP_SCENE_HAPTIC_NONE;
    } else if (state == OP_SCENE_HAPTIC_CLICK && now_ms >= s_scene_haptic_click_end_ms[idx]) {
        tiles_haptics_stop(pad);
        s_scene_haptic_state[idx] = OP_SCENE_HAPTIC_NONE;
    }
}

/* The pressure click, the only thing that acts in Live:
 *   - column 6: launch the scene (magenta flash);
 *   - a playing clip: stop it (the clip's color);
 *   - a stopped clip: fire it (the clip's color);
 *   - an empty slot: the same fire CC; the script arms the track and
 *     records into the slot (and opens melodic mode for a MIDI track; see
 *     OP_SCENE_MSG_OPEN_MELODIC). Red flash (no clip color yet). */
static void scene_handle_click(uint8_t pad, uint8_t col, const op_scene_cell_state_t *cell, uint32_t now_ms) {
    if (col == OP_SCENE_LAUNCH_COL) {
        scene_send_grid_touch(pad);
        /* Sentia magenta */
        scene_start_flash(OP_MENU_MELODIC_R, OP_MENU_MELODIC_G, OP_MENU_MELODIC_B, now_ms);
        return;
    }
    if (cell == NULL) {
        return;
    }
    if (!cell->has_clip) {
        scene_send_grid_touch(pad);
        scene_start_flash(1.0f, 0.0f, 0.0f, now_ms);
        return;
    }
    if (cell->is_playing) {
        scene_send_stop_clip_cc(pad);
    } else {
        scene_send_grid_touch(pad);
    }
    scene_start_flash((float)cell->r / 255.0f, (float)cell->g / 255.0f, (float)cell->b / 255.0f, now_ms);
}

/* True if it switched modes (see s_scene_pending_melodic); the caller
 * skips this frame's render. */
static bool handle_scene_launch_taps(uint32_t now_ms) {
    bool any_touched = false;
    /* circle = shift (delete gesture) */
    bool shift = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t idx = (uint8_t)(pad - 1u);
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_scene_prev_pad_touched[idx];
        /* Inverse of board_pad_for_row_col(); row = scene (row 1 = scene 0). */
        uint8_t col = (uint8_t)(((pad - 1u) % 6u) + 1u);
        uint8_t scene = (uint8_t)((pad - 1u) / 6u);

        const op_scene_cell_state_t *cell = NULL;
        bool has_content;
        bool playing = false;
        if (col == OP_SCENE_LAUNCH_COL) {
            has_content = scene_row_has_clips(scene);
        } else {
            uint8_t track = (uint8_t)(s_scene_track_offset + (col - OP_SCENE_TRACK_COL_MIN));
            if (track < OP_SCENE_MAX_TRACKS) {
                cell = &s_scene_clip[track][scene];
            }
            has_content = (cell != NULL) && cell->has_clip;
            playing = has_content && cell->is_playing;
        }

        if (touched) {
            any_touched = true;
        }
        scene_update_haptics(pad, touched, was_touched, has_content, playing, now_ms);

        /* Circle + touch + a clip under the pad = delete hold in progress;
         * anything else cancels it. Scene pads never qualify. */
        bool delete_eligible = shift && touched && col != OP_SCENE_LAUNCH_COL && has_content;
        if (delete_eligible) {
            if (!s_scene_delete_holding[idx]) {
                s_scene_delete_holding[idx] = true;
                s_scene_delete_fired[idx] = false;
                s_scene_delete_hold_start_ms[idx] = now_ms;
            } else if (!s_scene_delete_fired[idx] &&
                       (now_ms - s_scene_delete_hold_start_ms[idx]) >= OP_SCENE_DELETE_HOLD_MS) {
                s_scene_delete_fired[idx] = true;
                scene_send_delete_clip(pad);
                s_scene_delete_confirm_active = true;
                s_scene_delete_confirm_until_ms = now_ms + OP_SCENE_DELETE_CONFIRM_MS;
            }
        } else {
            s_scene_delete_holding[idx] = false;
            s_scene_delete_fired[idx] = false;
        }

        float depth = touched ? (float)tiles_hall_get_depth(pad) : 0.0f;
        if (!touched || depth < OP_SCENE_CLICK_REARM_DEPTH) {
            s_scene_click_latched[idx] = false;
        } else if (!s_scene_click_latched[idx] && depth > OP_SCENE_CLICK_DEPTH_THRESHOLD) {
            s_scene_click_latched[idx] = true;
            /* Latched either way, but only acts without circle, so the delete
             * gesture never also fires or stops the clip. */
            if (!shift) {
                scene_handle_click(pad, col, cell, now_ms);
            }
        }

        s_scene_prev_pad_touched[idx] = touched;
    }

    if (s_scene_pending_melodic && !any_touched) {
        s_scene_pending_melodic = false;
        set_active_mode(OP_MODE_MELODIC);
        /* After the switch: set_active_mode() clears this flag for some modes,
         * not melodic. */
        s_ableton_capture_active = true;
        return true;
    }
    return false;
}

static void render_scene_launch_underglow(uint32_t now_ms) {
    bool flashing = s_scene_trigger_flash_active && (now_ms - s_scene_trigger_flash_start_ms) < OP_SCENE_TRIGGER_FLASH_MS;
    if (s_scene_trigger_flash_active && !flashing) {
        s_scene_trigger_flash_active = false;
    }
    bool delete_active = s_scene_delete_confirm_active && now_ms < s_scene_delete_confirm_until_ms;
    if (s_scene_delete_confirm_active && !delete_active) {
        s_scene_delete_confirm_active = false;
    }
    for (uint8_t pad = 0u; pad < TILES_NUM_PADS && !delete_active; pad++) {
        if (s_scene_delete_holding[pad]) {
            delete_active = true;
        }
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (delete_active) {
            /* Red during a delete hold and for OP_SCENE_DELETE_CONFIRM_MS after;
             * beats a click flash. */
            tiles_lighting_set_standby_underglow_rgb(i, 1.0f, 0.0f, 0.0f);
        } else if (flashing) {
            tiles_lighting_set_standby_underglow_rgb(i, s_scene_flash_r, s_scene_flash_g, s_scene_flash_b);
        } else {
            /* Idle: teal (this mode's menu color). */
            tiles_lighting_set_standby_underglow_rgb(i, OP_MENU_SCENE_LAUNCH_R, OP_MENU_SCENE_LAUNCH_G,
                                                      OP_MENU_SCENE_LAUNCH_B);
        }
    }
}

static void render_scene_launch(uint32_t now_ms) {
    float pulse = scene_playing_pulse_level(now_ms);
    float blink = scene_triggered_blink_level(now_ms);

    for (uint8_t row = 0u; row < OP_SCENE_NUM_ROWS; row++) {
        uint8_t grid_row = (uint8_t)(row + TILES_GRID_MIN_ROW + 1u);
        for (uint8_t col = OP_SCENE_TRACK_COL_MIN; col <= OP_SCENE_TRACK_COL_MAX; col++) {
            uint8_t track = (uint8_t)(s_scene_track_offset + (col - OP_SCENE_TRACK_COL_MIN));
            float r = 0.0f, g = 0.0f, b = 0.0f;
            uint8_t pad_for_cell = board_pad_for_row_col(grid_row, col);
            if (track < OP_SCENE_MAX_TRACKS) {
                op_scene_cell_state_t *cell = &s_scene_clip[track][row];
                if (cell->has_clip && s_scene_delete_holding[pad_for_cell - 1u]) {
                    /* Delete hold: blinks red, then steady red once fired until Live reports
                     * the clip gone. */
                    r = s_scene_delete_fired[pad_for_cell - 1u] ? 1.0f : blink;
                } else if (cell->has_clip) {
                    float level;
                    if (cell->is_triggered) {
                        level = blink;
                    } else if (cell->is_playing) {
                        level = pulse;
                    } else {
                        level = OP_SCALE_AVAILABLE_LEVEL;
                    }
                    r = (float)cell->r / 255.0f * level;
                    g = (float)cell->g / 255.0f * level;
                    b = (float)cell->b / 255.0f * level;
                }
            }
            tiles_lighting_set_standby_pad_rgb(pad_for_cell, r, g, b);
        }

        /* Scene column: Sentia magenta whenever any track has a clip in the scene
         * (regardless of the scene's own color), blinking while queued. */
        op_scene_row_state_t *scene_row = &s_scene_row[row];
        float row_level = 0.0f;
        if (scene_row_has_clips(row)) {
            row_level = scene_row->is_triggered ? blink : OP_SCALE_AVAILABLE_LEVEL;
        }
        tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(grid_row, OP_SCENE_LAUNCH_COL),
                                            OP_MENU_MELODIC_R * row_level, OP_MENU_MELODIC_G * row_level,
                                            OP_MENU_MELODIC_B * row_level);
    }

    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        /* Diamond shows the transport LED here (see transport_led_level()). */
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? transport_led_level(now_ms) : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    render_scene_launch_underglow(now_ms);
}
