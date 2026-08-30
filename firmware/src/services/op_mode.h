#pragma once

/*
 * Operation modes: melodic (the board's normal, default behavior), chord,
 * sequencer, and guitar/bass fret mode -- real feedback: "its time to
 * implement the operation modes. we have standard melodic, chord trigger
 * mode (does full chords on one click but not implemented yet),
 * sequencer mode..., arpeggiator mode.... we trigger those with the
 * rombus diamond button." Arp was later replaced outright by guitar mode
 * (see the enum's own comment in op_mode.c) rather than kept as an
 * unreachable stub alongside it.
 *
 * ---- Mode select: SW4 (diamond), single click -------------------------
 * A single click (press then release, not a hold) toggles between three
 * things: MELODIC + no menu -> opens the menu; the MENU -> cancels back
 * to melodic; any other active mode (chord/sequencer/guitar) -> exits
 * back to melodic. To actually CHANGE modes, click to open the menu, then
 * tap a pad in the mode's row. Deliberately the simplest possible gesture
 * -- real feedback compared this to game_mode.h's own menu trigger, but
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
 * melodic = Sentia magenta, chord = green, sequencer = red, guitar =
 * amber/orange.
 * **Only AVAILABLE rows actually light up or respond to a tap** -- real
 * feedback: "the mode selector has all these lights always on. only
 * availabkle modes shouyld be on meaning for now only sequencer, and the
 * note mode" (now joined by guitar). Chord's row renders fully off and
 * is a no-op to tap, the same "unavailable" language this file's own
 * scale/pattern pickers already use for their own reserved slots.
 *
 * ---- Chord: selectable, not yet implemented ------------------------------
 * Real feedback named chord mode explicitly as "not implemented yet" --
 * still a real planned mode (not removed, unlike arp), just correctly
 * marked unavailable in the picker for now (see above). Selecting it
 * anyway (were its row ever made available again) wouldn't claim the pad
 * grid -- see tiles_op_mode_owns_pad_grid() below -- so touching pads
 * would keep playing completely normal melodic notes underneath until
 * its real logic is built. A deliberate, honest stub, not a guess
 * dressed up as a feature.
 *
 * ---- Guitar/bass fret mode ------------------------------------------------
 * Real feedback: "lets imoplenment for note mode a guitar fret mode for 4
 * stings with the structure of bass shapes, -+ change frets up and down.
 * each row is a string each colum is a fret." Row = string (4 rows, 4
 * strings -- a real bass's own string count), column = fret: a sliding
 * 6-fret window into a modeled 24-fret neck, shifted by "-"/"+" one fret
 * per press ("-+ change frets up and down"), standard 4-string bass
 * tuning (E1/A1/D2/G2, each string a perfect 4th above the last -- the
 * real, standard interval, not invented here). String-to-row order
 * follows standard TAB notation (highest string on top, lowest on
 * bottom) -- the closest existing convention to this exact row=string,
 * column=fret/time shape. See services/note_map.h's own "Guitar/bass fret
 * mode" section for the full note-mapping/fret-marker design.
 * Deliberately reuses services/expression.c's and services/lighting.c's
 * EXISTING touch/velocity/pitch-bend/haptics/idle-coloring pipelines
 * wholesale rather than building a custom rendering path the way
 * sequencer mode did -- this mode is architecturally much closer to
 * "melodic mode with a different note-mapping function" than to a
 * custom instrument, so it doesn't claim tiles_op_mode_owns_pad_grid()
 * at all (unlike sequencer); the ONLY thing this file owns for guitar
 * mode is "-"/"+" ownership (tiles_op_mode_owns_octave_buttons() below)
 * and pushing the active/fret-offset state into services/note_map.h,
 * which both services/expression.c (note computation) and
 * services/lighting.c (idle fret-marker coloring) already read from
 * directly.
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
 *   need a way to assign pitches to the notes." Continuing to hold the
 *   same pad escalates into one further per-step edit, probability (a
 *   per-pattern-togglable chance a step's otherwise-armed occurrence
 *   actually fires), set by Hall depth as a live dial rather than
 *   pitch's discrete pick -- real feedback: "yes per step probablility
 *   but we should be able to turn that on and off." Ratchet (additional
 *   retriggers within that same step, real feedback: "2 retrigger yess
 *   but we need to be able to control that feature") is a SEPARATE
 *   gesture instead of a third hold tier -- real feedback after trying
 *   three tiers on one timeline: "time escalation is good but not for so
 *   many features" -- reached by holding circle first, then touching the
 *   step, no wait at all.
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
 * lane instead of 24 in one), a real gate-length parameter, chord's
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
 * other. False while melodic, chord, or guitar is selected -- chord is
 * still an unimplemented stub that passes touch straight through to
 * normal melodic play, and guitar mode deliberately reuses that same
 * "doesn't own the grid" pass-through pipeline on purpose (see this
 * file's own "Guitar/bass fret mode" section above). */
bool tiles_op_mode_owns_pad_grid(void);

/* Broader than the accessor above: also true for guitar mode, which needs
 * "-"/"+" ownership (so services/octave_control.h's own default
 * octave-shift function yields -- see that file's own scan-gate) WITHOUT
 * the "suppress new note strikes" side effect real pad-grid ownership
 * carries elsewhere (services/expression.h checks owns_pad_grid() for
 * exactly that, and guitar mode's whole design depends on real notes
 * still playing normally through that same pipeline). Sequencer mode is
 * covered either way, since it already legitimately owns the whole grid. */
bool tiles_op_mode_owns_octave_buttons(void);

/* True whenever sequencer mode is the currently active mode, regardless
 * of which sequencer sub-view (pattern picker, pitch assign, normal step
 * view) is showing. Used by services/standby.h to give sequencer mode a
 * longer idle timeout before screensaver/deep sleep than plain melodic
 * idle gets -- real feedback: "sleep screensaver should be set to 20
 * minute in sequencer mode since its a more stratic thing." */
bool tiles_op_mode_is_sequencer_active(void);
