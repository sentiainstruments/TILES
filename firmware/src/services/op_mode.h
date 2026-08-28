#pragma once

/*
 * Operation modes: melodic (the board's normal, default behavior),
 * chord, sequencer, and arpeggiator -- real feedback: "its time to
 * implement the operation modes. we have standard melodic, chord
 * trigger mode (does full chords on one click but not implemented yet),
 * sequencer mode..., arpeggiator mode.... we trigger those with the
 * rombus diamond button."
 *
 * ---- Mode select: SW4 (diamond), single click -------------------------
 * A single click (press then release, not a hold) toggles between three
 * things: MELODIC + no menu -> opens the menu; the MENU -> cancels back
 * to melodic; any other active mode (chord/sequencer/arp) -> exits back
 * to melodic. To actually CHANGE modes, click to open the menu, then tap
 * a pad in the mode's row. Deliberately the simplest possible gesture --
 * real feedback compared this to game_mode.h's own menu trigger, but
 * that one is a 700ms hold of 4 buttons together; this is a plain click
 * of one, since there's no second "away from melodic" state that a click
 * could ambiguously mean here the way there is for game mode (which
 * needed a hold to not eat every accidental brush of all 4 during normal
 * play -- SW4 alone has no such normal-play collision).
 *
 * Guarded against game_mode.h's own SW3+SW4+SW5+SW6 entry combo: since
 * SW4 is one of that combo's four buttons, a diamond press that also
 * sees SW3 go down before release is never treated as this module's own
 * click -- see s_diamond_press_had_conflict in op_mode.c. The two
 * features are otherwise fully mutually exclusive (see
 * tiles_op_mode_owns_pad_grid() below and game_mode.c's gm_combo_held(),
 * which now also refuses to fire while this module owns the grid).
 *
 * ---- The menu -----------------------------------------------------------
 * 4 rows (one per mode), the same "row = feature, column = value" shape
 * services/expression_control.h's sub-menu uses rather than
 * services/game_mode.h's single row of 4 touch-targets -- real feedback:
 * "we have those 4 modes for now each on its own row because we might
 * add alternative modes derived from each to each corresponding
 * column." Only column 1 (the whole row, for now) is wired to anything;
 * later variants (e.g. the two-lane 16-step sequencer variant mentioned
 * in real feedback) would occupy other columns of the SEQUENCER row
 * without disturbing this file's overall shape. Tapping any pad in a row
 * activates that row's mode (default variant) and closes the menu.
 * Row colors ("mode selector color" -- real feedback's own phrase):
 * melodic = Sentia magenta, chord = green, sequencer = red, arp = blue.
 *
 * ---- Chord and arp: selectable, not yet implemented ----------------------
 * Real feedback named chord mode explicitly as "not implemented yet";
 * arp's real trigger logic (which pattern, which rate, which note order
 * from the currently-held notes) wasn't specified in enough detail to
 * build without guessing blind -- same "measure/spec before building"
 * discipline this codebase has followed all along. Both ARE selectable
 * from the menu (their row lights, tapping it "activates" the mode, the
 * diamond LED glows to confirm something other than melodic is
 * selected) but neither claims the pad grid -- see
 * tiles_op_mode_owns_pad_grid() below -- so touching pads keeps playing
 * completely normal melodic notes underneath until each mode's real
 * logic is built. A deliberate, honest stub, not a guess dressed up as
 * a feature.
 *
 * ---- Sequencer: reworked into a full multi-pattern workflow --------------
 * All 24 pads = 24 steps, real feedback: "lets build sequencermode with
 * the full 24 keys as a standard 24 step." Step order is row-major,
 * top-left to bottom-right -- pad 1 (row 1, col 1) is step 0, pad 24
 * (row 4, col 6) is step 23, exactly board_pad_for_row_col()'s own
 * numbering, no remapping needed. Quick tap toggles whether a step is
 * armed, same as always; holding a step past ~350ms ALSO opens per-step
 * pitch assignment (see "Pitch assignment" below).
 *
 * Real feedback asked for "a better architecture" studying real hardware
 * step sequencers, resulting in four new pieces beyond the original
 * single-pattern build:
 *
 * - **4 patterns**, one per pad row, picked via SW3 (triangle)'s own
 *   sub-menu while sequencer mode is active -- real feedback: "sub menu
 *   triangle is reserved for other stuff... maybe in triangle we can
 *   select midi channels for multiple patterns." Each pattern keeps its
 *   own armed steps, per-step pitch overrides, length, and MIDI output
 *   channel; switching patterns is immediate (no quantizing), always
 *   silencing whatever was sounding on the old pattern's channel first.
 * - **Pitch assignment**: holding a step opens a note-picker view of the
 *   whole grid (the same root/natural/sharp coloring melodic idle uses)
 *   -- tapping any pad sets that step's pitch to that pad's current
 *   note, independent of the step's own position. The same "hold the
 *   trig, play the note" workflow real hardware step sequencers use,
 *   adapted to touch-only hardware with no encoders. Real feedback: "we
 *   need a way to assign pitches to the notes."
 * - **Manual transport + length**: SW1 "-"/SW2 "+" (otherwise unused in
 *   sequencer mode, since services/octave_control.h's own default
 *   octave-shift function yields whenever this module owns the grid)
 *   are now stop/start -- real feedback: "we need a button that starts
 *   and stops sequencer." Held circle FIRST, then a fresh "-"/"+" press
 *   instead steps pattern length by +/-1 (1-24) -- real feedback: "we
 *   need to be able to adjsut length of sequence with shift + -." Both
 *   are no-ops whenever a real external clock is present (the DAW's own
 *   transport is the only valid control then, mirroring tap tempo's own
 *   "external always wins").
 * - **Quantized (re-)start**: real feedback, confirmed meaning: "we need
 *   to quantice to midi clock when that is conected" -- entering
 *   sequencer mode, or pressing "+", while a clock (real or tap-tempo)
 *   is already running waits for the next quarter-note boundary before
 *   actually resetting to step 0, instead of jumping in at whatever
 *   phase the clock happened to already be at.
 *
 * Clock: driven by services/midi_clock.h's shared pulse counter, fed
 * either by real external MIDI clock bytes OR that file's own internal
 * tap-tempo generator (SW6/circle) once established -- op_mode.c doesn't
 * need to know which. 1 step = 1 sixteenth note = OP_SEQ_CLOCKS_PER_STEP
 * (6) MIDI clock pulses, the standard convention (24 clocks/quarter note
 * per the MIDI spec, divided by 4 sixteenths/quarter) -- unmeasured
 * against what actually feels right for this board, a starting default
 * like every other timing constant in this codebase. A Start (0xFA, or
 * tap tempo's first establishment) resets the playhead to step 0 and
 * plays it immediately; Continue (0xFB) resumes from wherever the
 * playhead already was, no reset; Stop (0xFC) silences whatever's
 * currently sounding and freezes the playhead in place (armed/disarmed
 * step editing still works with no clock running at all -- only
 * PLAYBACK needs one).
 *
 * Step colors, real feedback: "in sequencer steps that are not playing
 * are slightly dim red. steps thats on is white bright and steps that
 * are unavailable are off," later extended: "we need cuentet stept to be
 * lit up always." Four states now: the CURRENT step (playhead) always
 * shows something -- bright white while its own note is actually
 * sounding, a dim cursor color otherwise (unarmed, or paused/waiting on
 * a quantized start); any other armed step shows dim red at rest; a
 * step that was never armed shows fully off; a step at or beyond the
 * pattern's current length shows off regardless of armed state. Notes/
 * haptics: "the happtics activate on each active step" -- an armed step
 * reaching the playhead sends a real MIDI note (fixed velocity -- no
 * strike/touch event exists to derive one from, on whichever channel
 * the active pattern claims) and a tiles_haptics_trigger_kick() on that
 * step's own pad; note-off + haptics stop fire the moment the playhead
 * LEAVES that step (full-length "gate," no separate gate-length concept
 * yet).
 *
 * Future variations noted, not yet built: a two-lane mode (16 steps per
 * lane instead of 24 in one), a real gate-length parameter, chord/arp's
 * actual trigger logic, quantized pattern switching -- all left as
 * explicit follow-ups rather than guessed into this pass.
 */

#include <stdbool.h>

void tiles_op_mode_init(void);

/* Handles the diamond click, menu pad taps, and (while sequencer mode is
 * active) step-arm taps + clock-driven playback. Call every main-loop
 * iteration, after tiles_buttons_scan()/tiles_touch_scan() (fresh input)
 * and services/midi_clock.h's tiles_midi_clock_scan() (fresh clock
 * state), before anything that reads tiles_op_mode_owns_pad_grid()
 * below this same iteration. */
void tiles_op_mode_scan(void);

/* True while the mode-select menu is showing OR sequencer mode is
 * actively running -- the two cases that need the WHOLE pad grid/button
 * row for this module's own rendering, mutually exclusive with
 * services/game_mode.h, services/expression_control.h's sub-menu,
 * services/octave_control.h's transpose mode, and services/standby.h,
 * exactly like those features are already mutually exclusive with each
 * other. False while melodic, chord, or arp is selected (chord/arp are
 * still unimplemented stubs that pass touch straight through to normal
 * melodic play -- see this file's own header). */
bool tiles_op_mode_owns_pad_grid(void);
