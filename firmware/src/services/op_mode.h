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
 * ---- Sequencer: the one mode fully built this pass -----------------------
 * All 24 pads = 24 steps, real feedback: "lets build sequencermode with
 * the full 24 keys as a standard 24 step." Step order is row-major,
 * top-left to bottom-right -- pad 1 (row 1, col 1) is step 0, pad 24
 * (row 4, col 6) is step 23, exactly board_pad_for_row_col()'s own
 * numbering, no remapping needed. Tapping a pad while sequencer mode owns
 * the grid toggles whether that step is armed (will play its own
 * note_map.c pitch when the playhead reaches it) or not, the same
 * touch-a-pad-to-toggle interaction expression_control.c's sub-menu
 * slider taps use.
 *
 * Clock: driven entirely by services/midi_clock.h's real, external MIDI
 * clock (no internal free-running fallback tempo -- see that file's own
 * header for why). 1 step = 1 sixteenth note = OP_SEQ_CLOCKS_PER_STEP (6)
 * MIDI clock pulses, the standard convention (24 clocks/quarter note per
 * the MIDI spec, divided by 4 sixteenths/quarter) -- unmeasured against
 * what actually feels right for this board, a starting default like
 * every other timing constant in this codebase. A Start (0xFA) resets
 * the playhead to step 0 and plays it immediately; Continue (0xFB)
 * resumes from wherever the playhead already was, no reset; Stop (0xFC)
 * silences whatever's currently sounding and freezes the playhead in
 * place (armed/disarmed step editing still works with no clock running
 * at all -- only PLAYBACK needs one).
 *
 * Step colors, real feedback: "in sequencer steps that are not playing
 * are slightly dim red. steps thats on is white bright and steps that
 * are unavailable are off." Three states, not two: an ARMED step shows
 * dim red at rest, and bright white for exactly the window its own note
 * is sounding (the playhead indicator); a step that was never armed
 * ("unavailable") shows fully off ALWAYS, including at the instant the
 * playhead passes over it -- no playhead flash on an empty step, per the
 * literal "unavailable are off" with no stated exception. Notes/haptics:
 * "the happtics activate on each active step" -- an armed step reaching
 * the playhead sends a real MIDI note (fixed velocity -- no strike/touch
 * event exists to derive one from) on a single reserved MPE Member
 * Channel (the last of the 15, TILES_MIDI_MPE_NUM_MEMBER_CHANNELS - 1
 * past the first -- sequencer playback only ever sounds one note at a
 * time in this v1, so one dedicated channel is enough; see op_mode.c's
 * OP_SEQ_CHANNEL for the narrow theoretical overlap this simplification
 * accepts) and a tiles_haptics_trigger_kick() on that step's own pad, at
 * the moment the playhead lands on it; note-off + haptics stop fire the
 * moment the playhead LEAVES that step (full-length "gate," no separate
 * gate-length concept yet -- exactly the kind of per-row column variant
 * the file header above already anticipates adding later).
 *
 * Future variations noted, not yet built: a two-lane mode (16 steps per
 * lane instead of 24 in one), configurable pattern length/clocks-per-step,
 * a real gate-length parameter, chord/arp's actual trigger logic -- all
 * left as explicit follow-ups rather than guessed into this first pass.
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
