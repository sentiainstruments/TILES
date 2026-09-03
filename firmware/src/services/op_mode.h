#pragma once

/*
 * Operation modes: melodic (the board's normal, default behavior), chord,
 * sequencer, and guitar/bass fret mode -- real feedback: "its time to
 * implement the operation modes. we have standard melodic, chord trigger
 * mode (does full chords on one click but not implemented yet),
 * sequencer mode..., arpeggiator mode.... we trigger those with the
 * rombus diamond button." Arp was later replaced outright by guitar mode
 * (see the enum's own comment in op_mode.c) rather than kept as an
 * unreachable stub alongside it. (At the time of that quote, mode-select
 * really was diamond -- see the swap note below.)
 *
 * ---- SW3 (triangle) <-> SW4 (diamond): functionality swapped -----------
 * Real feedback: "switch triangle and diamond functionality swapp them
 * fully." Everything below describes CURRENT behavior post-swap: SW3/
 * triangle is now the top-level mode-select click (was SW4/diamond), and
 * SW4/diamond is now each mode's own per-mode sub-menu click (was SW3/
 * triangle) -- a full, symmetric reversal of the two buttons' roles, not
 * a partial remap. Every identifier in op_mode.c reflects this (e.g.
 * `handle_triangle_click()` is the mode-picker handler, `handle_diamond_
 * click()` is the sub-menu handler -- exactly backwards from what their
 * names alone would suggest before you know about this swap). Quotes
 * elsewhere in this file that predate the swap and still say "diamond"
 * for mode-select or "triangle" for sub-menu (like the one just above)
 * are left verbatim as accurate historical record of what was actually
 * said at the time, not stale documentation of current behavior.
 *
 * ---- Mode select: SW3 (triangle), single click -------------------------
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
 * play -- SW3 alone has no such normal-play collision).
 *
 * Guarded against game_mode.h's own SW3+SW4+SW5+SW6 entry combo: since
 * SW3 is one of that combo's four buttons, a triangle press that also
 * sees SW4 go down before release is never treated as this module's own
 * click -- see s_triangle_press_had_conflict in op_mode.c. The two
 * features are otherwise fully mutually exclusive (see
 * tiles_op_mode_owns_pad_grid() below and game_mode.c's gm_combo_held(),
 * which now also refuses to fire while this module owns the grid).
 *
 * ---- The menu -----------------------------------------------------------
 * One pad per mode, all four packed onto a single row -- real feedback:
 * "the row thing for the mode menu on triangle is bad... like the
 * minigame menu," pointing at services/game_mode.h's own game-select
 * screen (one pad per game on row 1) as the shape to copy. Replaces an
 * earlier design that gave each mode an entire row (all 6 columns, one
 * color): "we have those 4 modes for now each on its own row because we
 * might add alternative modes derived from each to each corresponding
 * column" was that version's own reasoning, now superseded -- no mode
 * currently has any such column-variant built, so the row-per-mode
 * headroom it was reserving was never actually used. Tapping a mode's
 * pad activates that mode and closes the menu; every other pad, in the
 * menu row or not, is unlit and does nothing while browsing.
 * Slot colors ("mode selector color" -- real feedback's own phrase):
 * melodic = Sentia magenta, chord = green, sequencer = red, guitar =
 * amber/orange.
 * **Only AVAILABLE slots actually light up or respond to a tap** -- real
 * feedback: "the mode selector has all these lights always on. only
 * availabkle modes shouyld be on meaning for now only sequencer, and the
 * note mode" (now joined by guitar). Chord's slot renders fully off and
 * is a no-op to tap, the same "unavailable" language this file's own
 * scale/pattern pickers already use for their own reserved slots. The
 * current mode's own slot pulses white rather than showing its plain
 * hue, this file's one "selected" language, matching the same rule
 * services/expression_control.h's sub-menu uses.
 *
 * ---- Chord: selectable, not yet implemented ------------------------------
 * Real feedback named chord mode explicitly as "not implemented yet" --
 * still a real planned mode (not removed, unlike arp), just correctly
 * marked unavailable in the picker for now (see above). Selecting it
 * anyway (were its slot ever made available again) wouldn't claim the pad
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
 * - **4 patterns**, one per pad row, picked via SW4 (diamond)'s own
 *   sub-menu while sequencer mode is active (SW3/triangle at the time of
 *   the quote below -- see this file's own swap note above) -- real
 *   feedback: "sub menu triangle is reserved for other stuff... maybe in
 *   triangle we can select midi channels for multiple patterns." Each
 *   pattern keeps its own armed steps, per-step pitch overrides, length,
 *   and MIDI output channel; switching patterns is immediate (no
 *   quantizing), always silencing whatever was sounding on the old
 *   pattern's channel first.
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
#include <stdint.h>

void tiles_op_mode_init(void);

/* Handles the triangle click (mode-select), the diamond click (each
 * mode's own sub-menu), menu pad taps, and (while sequencer mode is
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
 * other. False while melodic, chord, or guitar is selected -- guitar
 * mode deliberately reuses that same "doesn't own the grid" pass-through
 * pipeline on purpose (see this file's own "Guitar/bass fret mode"
 * section above), and chord mode follows the identical pattern for its
 * own melody columns. Chord mode's 8 chord-strip pads are the one
 * exception -- excluded from normal play via the finer-grained
 * tiles_op_mode_owns_pad() below instead of this blanket accessor. */
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

/* Finer-grained than tiles_op_mode_owns_pad_grid() above: per-pad instead
 * of all-or-nothing. Every mode except chord defers straight to
 * tiles_op_mode_owns_pad_grid()'s own existing answer for every pad
 * (identical behavior to before this accessor existed). Chord mode is
 * the one exception -- it does NOT claim tiles_op_mode_owns_pad_grid()
 * itself (its melody columns need normal expression.c play, same as
 * guitar mode), but its 8 chord-strip pads (columns 1-2) DO need to be
 * excluded from services/expression.c's normal touch pipeline, since
 * services/op_mode.c drives them directly with multi-note chord MIDI
 * instead (see services/note_map.h's own "Chord mode" section for why a
 * single logical note can't represent a full chord). services/
 * expression.c's PAD_STATE_IDLE gate calls this once per pad instead of
 * the blanket accessor above for exactly that reason. */
bool tiles_op_mode_owns_pad(uint8_t logical_pad);

/* True whenever sequencer mode is the currently active mode, regardless
 * of which sequencer sub-view (pattern picker, pitch assign, normal step
 * view) is showing. Used by services/standby.h to give sequencer mode a
 * longer idle timeout before screensaver/deep sleep than plain melodic
 * idle gets -- real feedback: "sleep screensaver should be set to 20
 * minute in sequencer mode since its a more stratic thing." */
bool tiles_op_mode_is_sequencer_active(void);

/* True while any of this module's own sub-views is open: the top-level
 * mode picker, melodic's scale picker, sequencer's pattern picker, or a
 * sequencer per-step pitch/probability/ratchet editor. Used by
 * services/standby.h to hold off its own automatic idle timeout while
 * one of these is showing -- real feedback: "something triggering
 * animations when clicking the diamond menu" turned out to be
 * services/standby.h's plain 60-second idle timer elapsing while the
 * mode picker sat open with no touch on it (reading a menu takes no
 * touch input at all), silently replacing the menu with the screensaver
 * animation mid-browse. The same class of interruption applies to any of
 * this file's other sub-views for the same reason, not just the one that
 * happened to get reported first. */
bool tiles_op_mode_has_menu_open(void);
