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
 * fully." SW3/triangle became the top-level mode-select click (was SW4/
 * diamond), and SW4/diamond became each mode's own per-mode sub-menu
 * click (was SW3/triangle) -- a full, symmetric reversal of the two
 * buttons' roles, not a partial remap. Quotes elsewhere in this file
 * that predate the swap and still say "diamond" for mode-select or
 * "triangle" for sub-menu (like the one just above) are left verbatim as
 * accurate historical record of what was actually said at the time, not
 * stale documentation of current behavior.
 *
 * ---- Diamond freed entirely: sub-menu folded onto triangle+shift -------
 * Real feedback: "lets put the scale menu into the mode menu when
 * triangle plus shift pressed. freeing up diamond from everything for
 * now." SW4/diamond's per-mode sub-menu role from the swap above moved
 * again, onto SW3/triangle held together with SW6/circle ("shift" --
 * see services/midi_clock.h's own naming precedent) -- a plain solo
 * triangle click keeps meaning mode-select, unchanged. SW4/diamond
 * itself became a dedicated Ableton transport remote instead (see
 * op_mode.c's own handle_diamond_transport()) -- fully unrelated to
 * modes or menus now, not a third stop on the same swap. Every
 * identifier in op_mode.c reflects this current state (e.g.
 * `handle_triangle_click()` handles BOTH mode-select and, in its shift
 * branch, the sub-menu that predecessor `handle_diamond_click()` used to
 * own; `handle_diamond_transport()` replaced it). Quotes that predate
 * THIS move and still describe diamond as the sub-menu button (like the
 * "SW4 (diamond)'s own sub-menu" one further below) are, again, left
 * verbatim as historical record.
 *
 * ---- Sub-menu made universal; sequencer's own picker removed -----------
 * Real feedback: "make sure the shift scasle works on chord melodic mode
 * and on sequewndcer as well measning remove whatever aux menu we had in
 * sequencer mode." Triangle+shift's sub-menu, introduced by the move just
 * above with per-mode branching (melodic got the scale picker, sequencer
 * got a DIFFERENT sub-menu, a pattern/channel picker), is now just the
 * scale picker, unconditionally, in every mode that has one -- no branch
 * on s_active_mode at all anymore. The pattern/channel picker is REMOVED
 * outright, not replaced: its underlying multi-pattern data (4 patterns,
 * per-pattern MIDI channel -- see this file's own "Sequencer" section
 * below) is left in the code, but s_seq_active_pattern now stays
 * permanently 0 with no UI left to change it, until/unless a future round
 * gives pattern-switching a new access point. Chord mode's own melody
 * columns and sequencer's own note mapping both already read note_map.c's
 * global scale setting the identical way melodic mode's own idle grid
 * does, so opening the scale picker from either is exactly as meaningful
 * as it always was from melodic -- there was never a real reason for the
 * per-mode split once diamond had already taken the pattern picker's
 * original button away.
 *
 * ---- Mode select: SW3 (triangle), single click -------------------------
 * A single click (press then release, not a hold) is a plain toggle: menu
 * closed -> opens it; menu open -> closes it (no mode change) -- the SAME
 * two outcomes regardless of which mode happens to be active right now.
 * Real feedback: "why does a click of triangle send to melodic mode? in
 * other modes? it should just bring menu up" -- an earlier version instead
 * jumped straight back to melodic from any other active mode without
 * opening the picker at all; removed once real feedback called it out,
 * since render_menu()'s own col_is_current_mode() check already correctly
 * pulses whichever mode is ACTUALLY active regardless of what it is, so
 * opening the menu works identically from every mode and the special case
 * was never actually needed. To CHANGE modes, click to open the menu, then
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
 * scale picker already uses for its own reserved (undefined-scale)
 * slots. The
 * current mode's own slot pulses white rather than showing its plain
 * hue, this file's one "selected" language, matching the same rule
 * services/expression_control.h's sub-menu uses.
 *
 * ---- Chord: built, but still not selectable from the picker -------------
 * Originally named explicitly as "not implemented yet" when the picker
 * itself was built -- that's now stale: chord mode's own chord-strip
 * pads (columns 1-2) have a real implementation (op_mode.c's own
 * handle_chord_pad_taps()/chord_pad_strike(), see that section's own
 * header for the current triad+bass, velocity-sensitive design and its
 * real-feedback history). Still correctly marked unavailable in the
 * picker for now (see above) -- a separate, deliberate "not yet
 * offered as a mode to switch into" decision, not "doesn't work yet."
 * Melody-region pads (columns 3-6) already play normally through the
 * usual services/expression.c pipeline, same as melodic mode's own
 * grid, whether or not chord mode is ever reachable from the picker.
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
 * - **4 patterns**, originally picked via a dedicated sub-menu (SW4/
 *   diamond, then SW3/triangle+shift -- see this file's own swap notes
 *   above), REMOVED since (see the "Sub-menu made universal" section
 *   above) -- real feedback that first asked for the picker: "sub menu
 *   triangle is reserved for other stuff... maybe in triangle we can
 *   select midi channels for multiple patterns." The underlying data
 *   model is unchanged: each pattern still keeps its own armed steps,
 *   per-step pitch overrides, length, and MIDI output channel, and
 *   switching (when something could still trigger it) is immediate (no
 *   quantizing), always silencing whatever was sounding on the old
 *   pattern's channel first -- there is just no UI left that can
 *   actually trigger a switch right now, so s_seq_active_pattern stays
 *   permanently 0 in practice.
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

/* Real feedback: "we need to make sure it reboots to last state
 * completely includeing sequence, layout, scale, play state." Pass
 * exactly what watchdog_enable_caused_reboot() read at the top of
 * main() -- same value every other boot-time skip there already
 * reuses. When true, the active mode ("layout") and every lane's
 * running/active-alt state ("play state") are left exactly as they
 * were the instant before the crash instead of snapping back to
 * melodic/all-stopped -- see op_mode.c's own s_active_mode/s_seq_lane_
 * running[]/s_seq_active_alt[] declarations for the __uninitialized_ram
 * mechanism this relies on. Pattern CONTENT ("sequence") was already
 * unconditionally safe across a crash-recovery reboot before this --
 * see pattern_store_load_all()'s own comment -- only the four items
 * above needed this parameter added. Scale/octave/key restoration is
 * services/note_map.h's own tiles_note_map_init(), a separate call. */
void tiles_op_mode_init(bool crash_recovered);

/* Handles the triangle click (mode-select, or the scale picker -- now
 * universal, not per-mode -- when circle/"shift" is also held), the
 * diamond click (Ableton
 * transport remote -- see op_mode.c's own handle_diamond_transport()),
 * menu pad taps, and (while sequencer mode is active) step-arm taps +
 * clock-driven playback. Call every main-loop
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

/* True whenever sequencer mode OR Song mode is the currently active
 * mode, regardless of which sub-view (pitch assign, normal step view,
 * the restored pattern bank for sequencer; track-overview or the
 * step-edit screen for Song) is showing -- OR whenever a pattern is
 * genuinely still running in the background while some OTHER mode is
 * displayed (see op_mode.c's own "sequencer should not stop if mode is
 * changed" fix, and its Song-mode equivalent). Used by services/
 * standby.h to give both modes a longer idle timeout before
 * screensaver/deep sleep than plain melodic idle gets -- real
 * feedback: "sleep screensaver should be set to 20 minute in sequencer
 * mode since its a more stratic thing" (Song mode's own inclusion is
 * this codebase's own extension of that same reasoning, not separately
 * requested -- a Song pattern looping unattended is exactly as
 * "static" a thing as a sequencer pattern doing the same). */
bool tiles_op_mode_is_sequencer_active(void);

/* True while OP_MODE_MELODIC is active, OR while OP_MODE_CHORD is
 * active -- deliberately still narrower than tiles_op_mode_owns_pad_
 * grid()/_owns_pad(), which also let GUITAR mode's own melody pads
 * through the same real-strike pipeline. Board-2-only melodic
 * harmonics (services/expression.c, TILES_MELODIC_HARMONICS_ENABLED)
 * is this function's one caller and needs exactly this distinction.
 * Originally melodic-only ("real feedback scoped the feature to
 * melodic mode specifically, not 'everywhere a real note can play'");
 * widened to also cover chord mode once real feedback had chord mode's
 * own melody sub-grid follow the globally selected scale "like a mini
 * melodic mode" (see note_map.c's tiles_note_map_get_note(), the chord-
 * mode branch): "harmonics also apply to that mode." Chord mode's own
 * chord-STRIP pads (columns 1-2) can never actually trigger harmonics
 * regardless of this function's answer -- they never enter services/
 * expression.c's real-strike state machine at all (op_mode.c's own
 * chord_pad_strike() drives them directly), so scan_melodic_harmonics()'s
 * find_sole_held_pad() can only ever find a melody-grid pad while chord
 * mode is active, never a chord-strip one. Guitar mode is deliberately
 * still excluded -- its note mapping is a wholly different fretboard
 * system (guitar_note_for_pad()), not scale-degree play, and nothing
 * asked for harmonics there. */
bool tiles_op_mode_melodic_harmonics_may_play(void);

/* True while any of this module's own sub-views is open: the top-level
 * mode picker, the (now-universal, not melodic-only) scale picker,
 * sequencer mode's own pattern bank, a sequencer per-step pitch/
 * probability/ratchet editor, or sequencer capture mode. Used by
 * services/standby.h to hold off its own automatic idle timeout while
 * one of these is showing -- real feedback: "something triggering
 * animations when clicking the diamond menu" turned out to be
 * services/standby.h's plain 60-second idle timer elapsing while the
 * mode picker sat open with no touch on it (reading a menu takes no
 * touch input at all), silently replacing the menu with the screensaver
 * animation mid-browse. The same class of interruption applies to any of
 * this file's other sub-views for the same reason, not just the one that
 * happened to get reported first -- capture mode joined this list latest
 * (found auditing, not reported): it can sit genuinely armed with no
 * touch at all while waiting for a tempo/the next beat. */
bool tiles_op_mode_has_menu_open(void);

/* True if `channel` (an MPE Member Channel, 1-15) is one of the sequencer's
 * OP_SEQ_NUM_LANES own output channels AND the sequencer is genuinely
 * running right now -- always false otherwise. services/expression.c's
 * own claim_mpe_channel() checks every candidate channel against this
 * before claiming it from its live-touch pool, so the two independent
 * channel-allocation systems can't collide -- see that function's own
 * comment, and this accessor's own comment in op_mode.c, for the real
 * stuck-note failure mode this prevents. */
bool tiles_op_mode_sequencer_channel_is_reserved(uint8_t channel);

/* True while Song mode's own capture is actively recording (shift+
 * diamond from melodic/chord/guitar mode, or from within Song mode
 * itself -- see op_mode.c's own "Song mode: capture" section).
 * services/lighting.c checks this for its own underglow-only
 * recording indicator, the same "bypass standby ownership entirely,
 * write straight to hardware" pattern services/crash_indicator.h/
 * services/debug_mode.h's own underglow overrides already use -- the
 * pad grid otherwise stays exactly as the current mode already
 * renders it (see tiles_op_mode_song_capture_is_note_sounding() below
 * for the one pad-level exception real feedback asked for on top of
 * that).
 * Formerly "cross-mode capture into lane 3," rewired onto Song mode's
 * own pattern library entirely -- real feedback: "song mode as the
 * default capture mode instead of regular sequencer." */
bool tiles_op_mode_song_capture_is_active(void);

/* True if `note` is one of the notes currently sounding on whichever
 * slot Song mode's capture is currently recording into -- always
 * false otherwise, including once capture ends (that slot's own
 * s_song_sounding_notes[] is really just "whatever's audibly playing
 * right now," same array normal background playback uses). Real
 * feedback: "im asking for the sequencer to be visible on the pads on
 * the leds" -- the underglow-only indicator above wasn't enough on its
 * own; services/lighting.c's pad_desired_rgb() checks this, per pad,
 * against that pad's own currently-mapped note (tiles_note_map_get_
 * note()) so whichever pad the loop is currently playing visibly
 * flashes right on the melodic/guitar grid, without touching any OTHER
 * pad's own note-role coloring or blocking real touches -- the same
 * "layer on top, don't replace" precedent this whole feature already
 * follows for MIDI output. Chord mode's own chord-strip pads (which
 * don't resolve through tiles_note_map_get_note() at all) are outside
 * what this can highlight; only the melody-region/guitar-neck pads
 * that a captured note could ever actually reverse-map to.
 * No equivalent of the old cross-capture's own "current step pad"
 * marker yet -- that one relied on the regular sequencer's 24-step
 * pattern mapping naturally onto the 24-pad grid 1:1; Song mode's own
 * 128 steps (16/page x 8 pages) has no equally natural single-pad
 * mapping while melodic/chord/guitar's grid (not Song's own step-edit
 * screen) is what's actually showing. Deferred, not forgotten. */
bool tiles_op_mode_song_capture_is_note_sounding(uint8_t note);

/* True if `note` is currently held by an incoming MIDI Note-On (any
 * channel) AND melodic mode is the active mode -- real feedback: "in
 * midi melodic mode is there any way we could read the playing melody
 * of the armed track and display it back on tiles?" services/lighting.c
 * checks this, per pad, against that pad's own currently-mapped note
 * (tiles_note_map_get_note()), the exact same "layer on top of idle
 * coloring, don't replace or block a real touch" shape tiles_op_mode_
 * song_capture_is_note_sounding() above already established, just fed
 * by USB MIDI IN instead of this board's own captured pattern data.
 * Getting a note from the DAW's armed track onto this board's MIDI IN
 * at all needs the player's own DAW-side routing (a plain MIDI-thru/
 * monitor connection) -- nothing here configures or assumes that.
 * Chord mode's own melody sub-grid is deliberately NOT covered (see
 * op_mode.c's own "Melodic mode: live echo of an incoming melody"
 * section for why), and a note outside whatever's currently mapped to a
 * real pad simply has nothing to light -- both real, accepted
 * tradeoffs, not oversights. */
bool tiles_op_mode_incoming_note_is_sounding(uint8_t note);

/* True while the pattern bank's own save/delete confirmation flash is
 * currently showing, with *out_r/*out_g/*out_b set to the color it
 * wants underglow to show RIGHT NOW (already resolved through its own
 * two-blink timing -- see op_mode.c's own OP_PATTERN_FLASH_* section)
 * -- false (out params untouched) otherwise. services/lighting.c
 * checks this ABOVE debug mode's own underglow override: real
 * feedback found debug mode (armed for nearly this entire session)
 * was unconditionally swallowing this confirmation's green/red the
 * whole time it was on, since debug mode's own magenta pulse otherwise
 * takes priority over the plain write_underglow() path this
 * confirmation would normally show through. */
bool tiles_op_mode_pattern_flash_underglow_color(float *out_r, float *out_g, float *out_b);
