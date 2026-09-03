#include "op_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "expression_control.h"
#include "game_mode.h"
#include "hall.h"
#include "haptics.h"
#include "lighting.h"
#include "midi_clock.h"
#include "midi_out.h"
#include "note_map.h"
#include "octave_control.h"
#include "standby.h"
#include "touch.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Real feedback: "also the mode selector has all these lights always on.
 * only availabkle modes shouyld be on meaning for now only sequencer, and
 * the note mode" -- CHORD was, at the time, a real, planned, but
 * unimplemented stub (see this file's own header); ARP was the identical
 * kind of stub, but real feedback replaced its picker row outright with a
 * real feature instead of leaving it as a placeholder: "lets imoplenment
 * for note mode a guitar fret mode... this is going to be mode 3 on the
 * mode function selector." ARP is removed entirely (it had no real logic
 * beyond a stub + circle's beat-flash override, both deleted with it)
 * rather than kept as unreachable dead code alongside a mode that no
 * longer has a picker slot -- a genuine future arp mode would be a fresh,
 * deliberate feature request, not scaffolding worth preserving
 * unreachable. CHORD later became a real 4th mode too -- real feedback:
 * "lets create a mode that does chords on one side colum 1 and 2... and
 * melody in columns 3456... so this would go as our 4th play mode" -- see
 * this file's own "Chord mode" section below. */
typedef enum {
    OP_MODE_MELODIC = 0,
    OP_MODE_CHORD,
    OP_MODE_SEQUENCER,
    OP_MODE_GUITAR,
} tiles_op_mode_t;

#define OP_NUM_MODES 4u

/* Menu layout: one pad per mode, packed together on a single row, each
 * its own color -- real feedback: "the row thing for the mode menu on
 * triangle is bad... like the minigame menu," pointing at
 * services/game_mode.h's own game-select screen (gm_enter_menu()'s
 * render_menu(): row 1, one pad per game, snake/brick/tetris/pong/simon
 * each a single differently-colored pad, nothing else lit) as the
 * reference shape to copy. Replaces an earlier design where each mode
 * claimed an entire row (all 6 columns, same color) -- that version's
 * own history (guitar moved from "3rd available mode" to "literal 3rd
 * row" after real feedback found the row-counting confusing) is now
 * moot along with the row scheme itself. Row 1 is the pad row physically
 * closest to the function buttons (TILES_GRID_MIN_ROW + 1), matching
 * both game_mode.h's own choice and services/expression_control.h's
 * sub-menu's top-to-bottom sense. Column order: melodic, sequencer,
 * guitar, chord -- chord now a real, available mode too (see
 * col_is_available() below and this file's own "Chord mode" section),
 * kept last in this same column order it already had as a reserved
 * slot, so the layout doesn't shift now that it ships. */
#define OP_MENU_ROW 1u
#define OP_MENU_COL_MELODIC 1u
#define OP_MENU_COL_SEQUENCER 2u
#define OP_MENU_COL_GUITAR 3u
#define OP_MENU_COL_CHORD 4u

/* Mode-selector row colors -- real feedback's own phrase, "mode selector
 * color": melodic = Sentia magenta (this codebase's brand color,
 * redefined locally here same as services/expression_control.c and
 * services/boot_sequence.c each already do rather than sharing one
 * definition -- established precedent, not an oversight), sequencer =
 * red. Guitar = amber/orange (an instrument-wood-toned accent, distinct
 * from every other mode's color and from the guitar fretboard's own
 * idle amber coloring in services/lighting.c, tying the two together
 * visually). Chord = blue -- real feedback, alongside the mode's own
 * design: "so this would go as our 4th play mode and its designated
 * blue," matching the chord strip's own solid-blue LED coloring in
 * services/lighting.c (was green, back when chord was still an
 * unimplemented, unavailable stub). */
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

/* Triangle LED glow while the top-level mode picker is actually open --
 * the button itself is monochrome PWM (not addressable RGB like the
 * pads), so it can't show the mode-selector colors above; this is just a
 * "you're picking a mode right now" brightness cue, same reasoning
 * services/expression_control.c's SQUARE_LED_* levels use. Real feedback:
 * "the led for modes should light up on menu on not alwayus" -- this
 * USED to also glow continuously (a since-removed OP_DIAMOND_LED_MODE_
 * ACTIVE_LEVEL) for the entire time any non-melodic mode was active, not
 * just while browsing the picker; real feedback reversed that. (Named
 * for diamond back then -- see this file's own note on the later
 * triangle/diamond functionality swap for why the mode-picker LED is
 * named for triangle now instead.) */
#define OP_TRIANGLE_LED_MENU_LEVEL 1.0f
/* Same idea, diamond's own LED, lit while its per-mode sub-menu (e.g.
 * melodic's scale picker) is showing. */
#define OP_DIAMOND_LED_SUBMENU_LEVEL 1.0f
/* Real feedback: "the led for start and top shoukld light up as toggles
 * respectively" -- SW1 "-"/SW2 "+" light up as a two-state transport
 * indicator (see render_sequencer()'s own use) rather than sitting dark
 * the whole time sequencer mode owns the grid. */
#define OP_TRANSPORT_LED_LEVEL 0.8f

/* Real feedback: "lets standardize the pulsing and brightness and
 * behaviour for menues, meaning select color or active color is always
 * white bright pulsing in menues, respective less bright but still
 * readable bright for non selected and available and of for
 * unaveilabe." The shared shape (not a shared function -- redefined per
 * file, same precedent as SENTIA_MAGENTA_R/G/B/etc.) with
 * services/expression_control.c's own sub-menu, the other menu this
 * standard applies to. */
#define OP_MENU_SELECTED_PULSE_PERIOD_MS 900.0f
#define OP_MENU_SELECTED_PULSE_MIN 0.5f
#define OP_MENU_SELECTED_PULSE_MAX 1.0f
#define OP_MODE_PI 3.14159265358979323846f
/* The scale picker's own "available but not selected" level -- Sentia
 * magenta (this file's OP_MENU_MELODIC_* above), at a readable-but-
 * secondary brightness, distinct from the pulsing white selection.
 * Raised from 0.35, real feedback: "we could have idle led in scale mode
 * more bright not as dim." */
#define OP_SCALE_AVAILABLE_LEVEL 0.5f

static float menu_selected_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / OP_MENU_SELECTED_PULSE_PERIOD_MS;
    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
    return OP_MENU_SELECTED_PULSE_MIN + (OP_MENU_SELECTED_PULSE_MAX - OP_MENU_SELECTED_PULSE_MIN) * raw;
}

/* Real feedback: "when you touch but not click in menu make a strong
 * haptic click be felt, push pad to at least mroe than 50% to sleect and
 * selected pad give a constant haptic pulsing pattern to indicate its
 * the active one." Applies to both selection-style menus this file has
 * (the mode-picker and the scale-picker) -- sequencer mode's own step
 * taps are a different interaction (toggle on/off, not "pick one") and
 * deliberately unchanged.
 * 900 (tiles_hall_get_depth()'s own rough full-scale range on this
 * hardware -- see expression.c's s_depth_to_aftertouch_full_scale,
 * 900, the same reference point) * 0.5 = "more than half pressed." */
#define OP_MENU_SELECT_DEPTH_THRESHOLD 450.0f
/* Both the fresh-touch click acknowledgment and the periodic "still
 * selected" pulse below go through tiles_haptics_trigger_touch_pulse()
 * (a fixed-strength, self-terminating pulse -- see haptics.h's own
 * header), not tiles_haptics_trigger_kick(). A real bug found this round:
 * kick() settles into an indefinite low-level SUSTAIN buzz that nothing
 * in this file ever explicitly stopped for a menu touch (menus suppress
 * services/expression.c's own release-driven stop logic entirely while
 * they own the grid) -- every pad ever touched while browsing a menu was
 * left buzzing until something UNRELATED happened to stop it, matching
 * real feedback: "some pads get haptics stuck idk why." touch_pulse()
 * needs no matching stop call at all, exactly the right primitive for a
 * momentary UI acknowledgment. services/expression.c's own note strikes
 * are the only place in this codebase that still needs kick()'s full
 * KICK->SUSTAIN lifecycle (a real, held musical note) -- this file's own
 * sequencer playback (seq_enter_step()) is the other, and it's correctly
 * paired with an explicit seq_end_current_note() stop. */

/* ---- Master tap tempo (SW6/circle) + beat flash -------------------------
 * Real feedback: "lets make the midi clock work in a way where we can do
 * master tap tempo on the instrument with shit round button when not
 * derecting midi clock from a daw. the tapp tempo is only active in
 * sequencer and arp mode and requiere 4 taps to calcualte minimum. and
 * then flash that light as the tempo even when midi sync flash the tempo
 * there." Circle is this board's established "shift" button
 * (services/standby.h's own file header); a qualifying press-edge (see
 * handle_circle_tap() below) feeds services/midi_clock.h's tap-tempo API
 * directly -- that file owns the actual averaging/generation, this one
 * only owns deciding WHEN a press counts and rendering the beat flash.
 * The flash itself is driven purely by tiles_midi_clock_get_state()'s
 * shared pulse_count, so it shows the beat identically whether that
 * count is currently coming from real external clock bytes or the
 * internal tap-tempo generator -- no special-casing needed, matching the
 * "flash the tempo there [too]" half of the ask. */
#define OP_BEAT_FLASH_DURATION_MS 100u
#define OP_BEAT_FLASH_LEVEL 1.0f
/* 24 clock pulses per quarter note, fixed by the MIDI spec -- one beat
 * flash per quarter note, not per individual clock pulse. */
#define OP_CLOCK_PULSES_PER_BEAT 24u

static tiles_op_mode_t s_active_mode;
static bool s_menu_visible;

/* Real feedback: "its powering on with the mode light on" -- triangle's
 * permanent override gets explicitly claimed and set to 0.0f (off) once,
 * at the end of tiles_op_mode_init() below, and that write was traced
 * through the entire boot sequence (boot_sequence.c's animation,
 * buttons.c's own PCA9685 init-time correction, this file's own init)
 * without finding a logic gap where it should fail -- yet real hardware
 * still sometimes shows it lit right after boot, cleared only once the
 * player manually opens and closes the mode picker (whose own menu_exit()
 * makes the identical write). Rather than keep chasing an exact
 * mechanism with no further code-level lead, this re-asserts the same
 * "off" write on every scan for a short window after boot instead of
 * only once -- self-healing against whatever transient (a PCA9685
 * chip-level glitch during power-on, most likely, given the timing) is
 * corrupting the single init-time write, without needing to identify it
 * first. Not a real root-cause fix -- see this file's own README entry
 * for the honest state of this investigation. */
#define OP_BOOT_RELIGHT_GUARD_MS 1000u
static uint32_t s_boot_relight_guard_until_ms;

static bool s_triangle_was_held;
/* True if SW4 was ALSO seen held at any point during the CURRENT triangle
 * press -- see this file's own header on why (game_mode.h's entry combo
 * is SW3+SW4+SW5+SW6; without this, imperfectly-simultaneous presses of
 * that combo could misread as a plain triangle click, the identical
 * collision real feedback already found once between this board's
 * circle+square gestures and that same combo). Checked on release, not
 * press, same "can't know it's a genuine click until it's over" reasoning
 * services/standby.c's own circle-short-tap wake fix uses. */
static bool s_triangle_press_had_conflict;

static bool s_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Per-mode sub-menu (SW4/diamond, SW3/triangle at the time of the
 * quote below -- see this file's own swap note in op_mode.h) -----------
 * Real feedback: "the triangle is sub menues per each mode so that
 * button toggles its onw menue in each mode. for melodic it toggles
 * different scale modes." Only melodic's sub-menu (the scale picker) is
 * built -- other modes don't have one yet, same "selectable but not
 * implemented" spirit as chord/arp themselves; handle_diamond_click()
 * below is the extension point once they do. */
static bool s_scale_menu_visible;
static bool s_diamond_was_held;
static bool s_diamond_press_had_conflict; /* see s_triangle_press_had_conflict's own comment -- same reasoning, watches diamond instead of triangle */
static bool s_scale_menu_prev_pad_touched[TILES_NUM_PADS];

/* Tap-tempo press tracking -- see this file's own "Master tap tempo"
 * section above. No conflict flag like diamond/triangle's own: a tap
 * must register on the PRESS edge (not release) for accurate timing, so
 * handle_circle_tap() checks the other three combo buttons' CURRENT
 * state at the moment of the press instead of latching a flag to check
 * on release. */
static bool s_circle_was_held;
/* Beat-flash rendering state -- consumed by render_sequencer(),
 * computed once per scan in tiles_op_mode_scan() regardless. (Arp mode
 * used to ALSO show this via circle's button override before it was
 * removed -- see the mode enum's own comment -- leaving sequencer as the
 * only consumer for now.) */
static uint32_t s_last_beat_index = 0xFFFFFFFFu;
static uint32_t s_beat_flash_start_ms;

/* ---- Sequencer state ------------------------------------------------- */

#define OP_SEQ_NUM_STEPS TILES_NUM_PADS /* 24 -- one step per pad, see this file's header */
/* 24 MIDI clocks per quarter note (fixed by the MIDI spec, not a guess)
 * / 4 sixteenths per quarter = 6 -- the standard "1 step = 1 sixteenth
 * note" sequencer convention. Unmeasured against what feels right on
 * this board specifically, same as every other timing constant in this
 * codebase -- a reasonable, well-established starting point rather than
 * an arbitrary guess. */
#define OP_SEQ_CLOCKS_PER_STEP 6u
/* No strike/touch event exists for a clock-triggered note to derive a
 * velocity from -- fixed at a solid, unaccented level rather than
 * guessing a curve with no input to drive it. */
#define OP_SEQ_VELOCITY 100u

#define OP_SEQ_DIM_RED_LEVEL 0.3f
/* Real feedback: "we need cuentet stept to be lit up always" -- a dim
 * white marker on the current step regardless of armed state, distinct
 * from OP_SEQ_DIM_RED_LEVEL so it never reads as "armed." Only used for
 * an UNARMED current step now -- see OP_SEQ_CURSOR_ARMED_* below for why
 * an armed one needs its own, different color. */
#define OP_SEQ_CURSOR_LEVEL 0.15f
/* Real feedback: "make play head on active pad in sewquencer blue if pad
 * active so it wont look as an inactive pad." Needed once probability
 * could make an ARMED step silently not sound on a given pass
 * (`seq_enter_step()`'s own probability roll) -- without this, the
 * cursor sitting on that step would fall through to the plain white
 * OP_SEQ_CURSOR_LEVEL above, visually indistinguishable from sitting on
 * a step that was never armed at all. Blue means "the cursor is here AND
 * this step is armed" -- unconditionally, sounding or not (a first pass
 * let bright white win whenever the step was actually sounding, but real
 * feedback afterward -- "play head is still white on play when going
 * over selected step" -- confirmed that just reintroduced the exact
 * confusion this color exists to prevent, since sounding IS the armed
 * case that matters most; see render_sequencer()'s own comment). */
#define OP_SEQ_CURSOR_ARMED_R 0.0f
#define OP_SEQ_CURSOR_ARMED_G 0.3f
#define OP_SEQ_CURSOR_ARMED_B 1.0f

/* ---- Multi-pattern bank ------------------------------------------------
 * Real feedback: "sub menu triangle is reserved for other stuff... maybe
 * in triangle we can select midi channels for multiple patterns." 4
 * patterns -- deliberately matches the grid's own 4 pad rows exactly, so
 * the picker (below) reuses the mode-picker's one-row-per-item shape
 * rather than inventing a new layout for a small, fixed item count.
 * Each pattern keeps its own armed steps, per-step pitch override, length,
 * and output channel -- what used to be this file's only flat
 * s_seq_step_armed[]/fixed OP_SEQ_CHANNEL is now per-pattern state behind
 * active_pattern() below. Default channel per pattern claims from the TOP
 * of the 15 MPE Member Channels downward (pattern 0 = nibble 15, exactly
 * today's original single-channel behavior, unchanged for anyone not
 * using extra patterns) -- a default, not a reservation:
 * services/expression.c's own live per-touch channel allocator is
 * untouched, so this can only ever collide with live touch playing in the
 * rare case of using many fingers at once while ALSO running multiple
 * active patterns, an accepted edge case rather than something worth
 * shrinking live MPE polyphony to avoid. */
#define OP_SEQ_NUM_PATTERNS 4u
#define OP_SEQ_MIN_LENGTH 1u
#define OP_SEQ_MAX_LENGTH OP_SEQ_NUM_STEPS
/* Real feedback: "2 retrigger yess but we need to be able to control that
 * feature." Capped at OP_SEQ_CLOCKS_PER_STEP (6) since a ratchet can't
 * usefully subdivide a step finer than the step's own pulse resolution --
 * 4 leaves each sub-hit at least 1 full clock pulse apart even at the max. */
#define OP_SEQ_MAX_RATCHET 4u

typedef struct {
    bool step_armed[OP_SEQ_NUM_STEPS];
    bool step_pitch_override[OP_SEQ_NUM_STEPS];
    uint8_t step_note[OP_SEQ_NUM_STEPS];             /* only meaningful if step_pitch_override[i] */
    uint8_t step_probability_percent[OP_SEQ_NUM_STEPS]; /* 0-100, default 100 -- only applied if probability_enabled */
    uint8_t step_ratchet_count[OP_SEQ_NUM_STEPS];    /* 1..OP_SEQ_MAX_RATCHET, default 1 (no ratchet), always applied */
    /* Real feedback: "yes per step probablility but we should be able to
     * turn that on and off." Per-pattern master switch -- while false,
     * every step behaves as if its own probability were 100% regardless
     * of what's stored, so a performer can flip a whole pattern back to
     * fully deterministic without having to remember/reset every
     * individually-dialed step. Toggled via a plain circle click while
     * the pattern picker (below) is open -- see handle_circle_tap(). */
    bool probability_enabled;
    uint8_t length;  /* 1..24 active steps -- see OP_SEQ_MIN/MAX_LENGTH */
    uint8_t channel; /* raw 0-15 status-nibble MPE channel */
} op_seq_pattern_t;

static op_seq_pattern_t s_seq_pattern[OP_SEQ_NUM_PATTERNS];
static uint8_t s_seq_active_pattern;

static uint8_t s_seq_current_step; /* 0..23 */
static bool s_seq_note_sounding;
static uint8_t s_seq_sounding_pad; /* 1..24, valid iff s_seq_note_sounding */
static uint8_t s_seq_sounding_channel;
static uint8_t s_seq_sounding_note;
static uint32_t s_seq_step_started_at_pulse;
static bool s_seq_prev_pad_touched[TILES_NUM_PADS];
/* Real feedback: "we need to quantice to midi clock when that is
 * conected" -- confirmed meaning: a manual (re-)start doesn't just jump
 * in at whatever phase the clock happens to be at, it waits for the next
 * beat boundary. Set by seq_start() (entering sequencer mode) and by "+"
 * (handle_transport_and_length()); consumed by seq_advance_clock().
 * s_seq_pending_restart distinguishes what happens once that boundary
 * arrives -- real feedback pinned down the exact required behavior:
 * "play when playing brings head to start point again" (restart, step 0)
 * vs "when stopped makes play" with the head otherwise left wherever a
 * plain stop left it (resume IN PLACE, no reset) -- these are genuinely
 * different actions, not the same one applied at different times, so one
 * pending flag isn't enough on its own. True for a genuine restart
 * (seq_start()'s own fresh entry, and "+" pressed WHILE already playing);
 * false for a plain resume ("+" pressed while stopped, not preceded by a
 * double-stop rewind). */
static bool s_seq_pending_start;
static bool s_seq_pending_restart;

/* Ratchet playback state for the CURRENT step only -- reset every time a
 * new step is entered (seq_enter_step()), consumed by seq_advance_clock()
 * firing additional sub-hits within that same step's own pulse window.
 * s_seq_ratchet_remaining counts hits still owed AFTER the one seq_enter_
 * step() already fired directly. */
static uint8_t s_seq_ratchet_remaining;
static uint32_t s_seq_ratchet_interval_pulses;
static uint32_t s_seq_next_ratchet_pulse;

/* ---- Per-step editing: pitch, probability, ratchet ---------------------
 * Real feedback: "we need a way to assign pitches to the notes"; later,
 * asked for as controllable features rather than fixed values: "yes per
 * step probablility but we should be able to turn that on and off, 2
 * retrigger yess but we need to be able to control that feature." A
 * first pass escalated through THREE hold-duration tiers (pitch ->
 * probability -> ratchet) mirroring services/standby.h's own circle-hold
 * escalation -- real feedback after trying it: "time escalation is good
 * but not for so many features." Reworked to two DIFFERENT entry
 * mechanisms instead of one long timeline:
 * - Pitch and probability still escalate on a single hold
 *   (seq_handle_step_taps() detects OP_SEQ_PITCH_ASSIGN_HOLD_MS,
 *   handle_edit_mode() itself escalates to OP_SEQ_PROBABILITY_HOLD_MS) --
 *   only two tiers now, easier to land on reliably.
 * - Ratchet is now a SEPARATE, immediate combo instead of a third timing
 *   tier: holding circle (this board's established "shift") FIRST, then
 *   touching a step, enters ratchet-edit directly, no wait at all --
 *   matches how circle+"-"/"+" already means "the shifted version of
 *   this action" elsewhere in this file, so it's an extension of an
 *   already-learned convention rather than a new one.
 * Pitch stays a genuine TOGGLE once opened (real feedback from an
 * earlier round: "it should be a toggle... not a momentary thing") --
 * releasing the held pad before escalating leaves it open, waiting for a
 * fresh tap on any pad to commit a note. Probability and ratchet are a
 * live DIAL instead (Hall depth of the SAME held pad maps continuously
 * to the value while still held, committing simply by however the value
 * stood at release) -- a fundamentally different shape from pitch's
 * discrete pick-from-24, and one that doesn't have pitch's "had to keep
 * two fingers down" problem since only the one held pad is ever needed.
 * Diamond cancels out of any of the three with no change
 * (handle_diamond_click()'s own branch).
 * A plain tap's own armed-toggle resolves on RELEASE, not press -- real
 * feedback: "when setting the pitch of pad we are still affecting note
 * on." Toggling on press couldn't yet tell a tap from the start of a
 * hold, so holding an armed step to edit it flipped its armed state
 * first, before the hold even had a chance to escalate -- see
 * seq_handle_step_taps()'s own comment for the full fix. */
typedef enum {
    OP_SEQ_EDIT_NONE = 0,
    OP_SEQ_EDIT_PITCH,
    OP_SEQ_EDIT_PROBABILITY,
    OP_SEQ_EDIT_RATCHET,
} op_seq_edit_mode_t;

#define OP_SEQ_PITCH_ASSIGN_HOLD_MS 350u
#define OP_SEQ_PROBABILITY_HOLD_MS 1200u

static uint32_t s_seq_step_touch_started_ms[TILES_NUM_PADS]; /* 0 = not currently timing a hold */
static op_seq_edit_mode_t s_seq_edit_mode;
static uint8_t s_seq_edit_step;         /* 0..23, valid iff s_seq_edit_mode != OP_SEQ_EDIT_NONE */
static uint32_t s_seq_edit_started_ms;  /* the ORIGINAL touch-down time -- escalation is timed from here, not from OP_SEQ_EDIT_PITCH's own entry */
static bool s_pitch_edit_prev_pad_touched[TILES_NUM_PADS]; /* only meaningful during OP_SEQ_EDIT_PITCH */
/* Real feedback: "im woried the value decreses before the mode is
 * exited... lets make sure the lift dosnt loose the feature." Lifting a
 * finger off a pad is a continuous, physical release -- Hall depth
 * necessarily passes back down through every lower value on its way to
 * 0 before the touch sensor itself finally reports "released," so a
 * live value tied directly to CURRENT depth always got dragged toward
 * zero by the release motion itself, corrupting whatever the player
 * actually intended.
 * A first attempt tracked the PEAK depth reached and only ever wrote on
 * a NEW peak -- immune to the release drag, but real feedback after
 * trying it: "we solved the push and increse and hold but we didnt
 * solve the reduce value... we need some way to hold and not loose
 * value but still have reduce power funciton" -- peak-tracking also made
 * it impossible to deliberately DIAL a value back down while still
 * holding, since any decrease (intentional or not) was simply ignored.
 * Depth alone can't tell "easing off on purpose, still holding" from
 * "beginning to lift off entirely" -- both look identical, a smooth
 * decrease -- until the very end of a release, which always finishes by
 * passing through the sensor's true near-zero rest depth on its way to
 * full contact loss. So instead of ignoring every decrease, only the
 * FINAL approach to that near-zero floor is ignored: below
 * OP_SEQ_EDIT_RELEASE_GUARD_DEPTH, depth stops being written to the
 * pattern data at all, freezing whatever the last real value above the
 * guard was. Above the guard, depth is written on every sample,
 * increase or decrease alike -- a genuine two-way live dial. This does
 * mean the dial's lowest reachable value while still holding is capped
 * just above true zero (using the guard's own probability-percent
 * mapping, roughly OP_SEQ_EDIT_RELEASE_GUARD_DEPTH / 900 * 100) rather
 * than exactly 0 -- an accepted trade, since a step that should never
 * fire is better served by disarming it or the probability master
 * toggle than by fighting this gesture down to an exact zero.
 * OP_SEQ_EDIT_RELEASE_GUARD_DEPTH itself is a first-attempt guess, not
 * yet validated against real capture data the way
 * expression.c's MIN_STRIKE_DEPTH_DELTA was -- revisit if real hardware
 * testing shows it's cutting off legitimately-intended low values, or
 * conversely still letting the release motion sneak in a bad write. */
#define OP_SEQ_EDIT_RELEASE_GUARD_DEPTH 60u

/* ---- Pattern/channel picker (SW4/diamond, sequencer mode) --------------
 * Same role diamond already plays in melodic mode (the scale picker) --
 * see handle_diamond_click()'s own branch on s_active_mode. Row colors
 * are shades within sequencer's own red identity (the mode-picker's
 * "sequencer = red" real feedback) rather than melodic/chord/arp's own
 * colors, so picking a pattern never reads as switching modes. */
#define OP_PATTERN_1_R 1.0f
#define OP_PATTERN_1_G 0.0f
#define OP_PATTERN_1_B 0.0f
#define OP_PATTERN_2_R 1.0f
#define OP_PATTERN_2_G 0.5f
#define OP_PATTERN_2_B 0.0f
#define OP_PATTERN_3_R 1.0f
#define OP_PATTERN_3_G 1.0f
#define OP_PATTERN_3_B 0.0f
#define OP_PATTERN_4_R 1.0f
#define OP_PATTERN_4_G 0.0f
#define OP_PATTERN_4_B 0.5f
static bool s_pattern_menu_visible;
static bool s_pattern_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Transport + length (SW1 "-"/SW2 "+", sequencer mode only) ---------
 * Real feedback: "we need a button that starts and stops sequencer... we
 * need to be able to adjsut length of sequence with shift + -." Plain
 * press = transport (resolved on release, exactly like
 * services/octave_control.c's own solo-step-vs-combo disambiguation --
 * see that file's "solo-step race" fix for why release, not press, is
 * required). Circle held FIRST, then a fresh "-"/"+" press, steps length
 * instead -- circle is this board's established "shift" identity, matching
 * services/expression_control.c's own "hold the modifier first" square-
 * then-"-"/"+" convention. Only the circle-first order is supported (not
 * the reverse) -- see handle_transport_and_length()'s own comment.
 * services/octave_control.c already yields "-"/"+" entirely whenever
 * tiles_op_mode_owns_pad_grid() is true (sequencer mode included), so
 * there's no double-handling to guard against here. */
static bool s_minus_was_held;
static bool s_plus_was_held;
static bool s_minus_used_as_combo;
static bool s_plus_used_as_combo;

/* ---- Chord mode (columns 1-2) -------------------------------------------
 * Real feedback: "lets create a mode that does chords on one side colum 1
 * and 2 (pad19 cchord, pad20 d chord, pad13 chord e and loke that.) and
 * melody in columns 3456 in a 4x4 grid starting with c in pad 21... so
 * this would go as our 4th play mode." The melody columns need nothing
 * here at all -- they pass straight through services/expression.c's
 * unmodified pipeline, remapped via services/note_map.h exactly like
 * guitar mode's fretboard. Only the 8 chord-strip pads need real logic:
 * services/note_map.h's tiles_note_map_is_chord_region_pad() already
 * says which pads those are (columns 1-2, only while this mode is
 * active), so this claims exactly those pads (via tiles_op_mode_owns_
 * pad() below) and drives 3-note chords on them directly, the same
 * "claim the pads, drive MIDI directly" shape this file's sequencer
 * already uses for the whole grid, just narrowed to 8 pads. Per-pad
 * state (not a single global "sounding note" the way the sequencer has)
 * because, unlike the sequencer's one-step-at-a-time playhead, multiple
 * chord pads can plausibly be held down together. */
#define OP_CHORD_CHANNEL 10u /* raw 0-15 nibble -- one below game_mode.c's
                                 own GM_MELODY_CHANNEL (11) and this
                                 file's own sequencer patterns (12-15), so
                                 none of this codebase's other direct-MIDI
                                 claims collide. A default claim, not a
                                 hard reservation, same as the sequencer's
                                 own per-pattern channels above: services/
                                 expression.c's live per-touch allocator
                                 (still running for chord mode's own
                                 melody columns) is untouched, so this can
                                 only ever collide with live touch play in
                                 the rare case of using many fingers at
                                 once while chords are also held -- an
                                 accepted edge case, not worth shrinking
                                 live MPE polyphony to avoid. */
#define OP_CHORD_VELOCITY 100u /* fixed -- chord pads are triggered
                                   directly here, bypassing services/
                                   expression.c entirely, so there's no
                                   real strike-velocity signal to read;
                                   matches this file's own OP_SEQ_VELOCITY
                                   precedent for the identical reason. */
static bool s_chord_pad_touched[TILES_NUM_PADS];
static bool s_chord_pad_sounding[TILES_NUM_PADS];
static uint8_t s_chord_pad_notes[TILES_NUM_PADS][TILES_NOTE_MAP_CHORD_NUM_NOTES];

static void chord_pad_note_off(uint8_t pad) {
    if (!s_chord_pad_sounding[pad - 1u]) {
        return;
    }
    for (uint8_t i = 0; i < TILES_NOTE_MAP_CHORD_NUM_NOTES; i++) {
        tiles_midi_note_off(OP_CHORD_CHANNEL, s_chord_pad_notes[pad - 1u][i]);
    }
    tiles_haptics_stop(pad);
    s_chord_pad_sounding[pad - 1u] = false;
}

static void chord_pad_note_on(uint8_t pad) {
    tiles_note_map_get_chord_notes(pad, s_chord_pad_notes[pad - 1u]);
    for (uint8_t i = 0; i < TILES_NOTE_MAP_CHORD_NUM_NOTES; i++) {
        tiles_midi_note_on(OP_CHORD_CHANNEL, s_chord_pad_notes[pad - 1u][i], OP_CHORD_VELOCITY);
    }
    tiles_haptics_trigger_kick(pad, OP_CHORD_VELOCITY);
    s_chord_pad_sounding[pad - 1u] = true;
}

/* Releases every currently-sounding chord pad -- called whenever chord
 * mode stops being the active mode (set_active_mode() below), the same
 * "clean up whatever's sounding the instant a mode hands off control"
 * rule this file's own seq_end_current_note()/set_active_mode() pairing
 * already established for the sequencer. */
static void chord_end_all_notes(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        chord_pad_note_off(pad);
    }
}

static void handle_chord_pad_taps(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (!tiles_note_map_is_chord_region_pad(pad)) {
            continue;
        }
        bool touched = tiles_touch_is_touched(pad);
        if (touched && !s_chord_pad_touched[pad - 1u]) {
            chord_pad_note_on(pad);
        } else if (!touched && s_chord_pad_touched[pad - 1u]) {
            chord_pad_note_off(pad);
        }
        s_chord_pad_touched[pad - 1u] = touched;
    }
}

static bool other_feature_owns_input(void) {
    return tiles_game_mode_is_active() || tiles_expression_control_owns_pad_grid() ||
           tiles_octave_control_is_transpose_active() || tiles_standby_is_active() || tiles_standby_is_deep_sleep();
}

static op_seq_pattern_t *active_pattern(void) {
    return &s_seq_pattern[s_seq_active_pattern];
}

static void edit_enter(uint8_t step, uint32_t started_ms); /* defined below, used by seq_handle_step_taps()'s own hold detection */
static void edit_enter_ratchet(uint8_t step); /* defined below, used by seq_handle_step_taps()'s own circle+touch detection */

/* Uses the channel/note captured at note-on time (below), not whatever
 * active_pattern() currently resolves to -- a pattern switch mid-note
 * (handle_pattern_menu_taps()) always calls this FIRST, but capturing the
 * actual sounding values independently means correctness never depends on
 * that ordering being preserved everywhere this gets called from. */
static void seq_end_current_note(void) {
    if (!s_seq_note_sounding) {
        return;
    }
    tiles_midi_note_off(s_seq_sounding_channel, s_seq_sounding_note);
    tiles_haptics_stop(s_seq_sounding_pad);
    s_seq_note_sounding = false;
}

/* Fires ONE note for `step` -- shared by seq_enter_step() (the step's
 * first hit) and seq_advance_clock() (any additional ratchet sub-hits
 * within that same step) so both go through identical logic. Does NOT
 * touch s_seq_current_step or roll probability -- those are seq_enter_
 * step()'s own concerns, once per step, not per ratchet hit. */
static void seq_fire_note(uint8_t step) {
    seq_end_current_note();
    op_seq_pattern_t *pat = active_pattern();
    uint8_t pad = (uint8_t)(step + 1u);
    uint8_t note = pat->step_pitch_override[step] ? pat->step_note[step] : tiles_note_map_get_note(pad);
    tiles_midi_note_on(pat->channel, note, OP_SEQ_VELOCITY);
    tiles_haptics_trigger_kick(pad, OP_SEQ_VELOCITY);
    s_seq_note_sounding = true;
    s_seq_sounding_pad = pad;
    s_seq_sounding_channel = pat->channel;
    s_seq_sounding_note = note;
}

/* Real feedback: "yes per step probablility but we should be able to
 * turn that on and off, 2 retrigger yess but we need to be able to
 * control that feature." Rolls this step's probability (once per step
 * occurrence, not per ratchet hit -- a whole step either fires or it
 * doesn't, matching how real hardware "trig probability" works) and, if
 * it fires, arms however many additional ratchet hits it's set for --
 * seq_advance_clock() below fires those on schedule via seq_fire_note(). */
static void seq_enter_step(uint8_t step) {
    seq_end_current_note();
    s_seq_current_step = step;
    s_seq_ratchet_remaining = 0u;
    op_seq_pattern_t *pat = active_pattern();
    if (!pat->step_armed[step]) {
        return;
    }
    if (pat->probability_enabled && (uint32_t)(rand() % 100) >= pat->step_probability_percent[step]) {
        /* This occurrence of an otherwise-armed step is silently
         * skipped -- the cursor still moves, nothing sounds. */
        return;
    }
    uint8_t ratchet_total = pat->step_ratchet_count[step];
    if (ratchet_total < 1u) {
        ratchet_total = 1u;
    }
    seq_fire_note(step);
    if (ratchet_total > 1u) {
        s_seq_ratchet_remaining = (uint8_t)(ratchet_total - 1u);
        s_seq_ratchet_interval_pulses = OP_SEQ_CLOCKS_PER_STEP / ratchet_total;
        if (s_seq_ratchet_interval_pulses < 1u) {
            s_seq_ratchet_interval_pulses = 1u;
        }
        s_seq_next_ratchet_pulse = s_seq_step_started_at_pulse + s_seq_ratchet_interval_pulses;
    }
}

static void seq_reset(uint32_t now_pulse) {
    s_seq_step_started_at_pulse = now_pulse;
    s_seq_pending_start = false;
    seq_enter_step(0u);
}

/* The other half of s_seq_pending_restart's distinction -- resumes
 * whatever step the playhead was already parked on (re-firing it fresh,
 * including a new probability roll/ratchet cycle, since from the
 * player's perspective this IS a new occurrence of that step) instead of
 * jumping back to step 0. */
static void seq_resume_current_step(uint32_t now_pulse) {
    s_seq_step_started_at_pulse = now_pulse;
    seq_enter_step(s_seq_current_step);
}

/* Called every time sequencer mode is (re-)entered from the menu --
 * deliberately does NOT clear any pattern's step_armed[]: leaving to
 * melodic mode to check something and coming back would otherwise wipe
 * whatever pattern was already programmed, which is more frustrating than
 * useful. Armed steps are only ever cleared at boot (tiles_op_mode_init()).
 * Arms a pending (quantized) start rather than jumping straight to step 0
 * -- see s_seq_pending_start's own comment and seq_advance_clock() below;
 * this is what fixes entering sequencer mode while an external clock is
 * already mid-phrase landing on an essentially random step. */
static void seq_start(void) {
    s_seq_current_step = 0u;
    s_seq_note_sounding = false;
    s_seq_step_started_at_pulse = 0u;
    s_seq_pending_start = true;
    s_seq_pending_restart = true; /* fresh entry always starts from step 0 */
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    s_pattern_menu_visible = false;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_seq_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        s_seq_step_touch_started_ms[i] = 0u;
    }
}

/* Extended with hold detection: a touch held past
 * OP_SEQ_PITCH_ASSIGN_HOLD_MS opens per-step editing (see this file's own
 * "Per-step editing" section) -- the arm-toggle on touch-down already
 * fired for this same touch, so holding a step both toggles it AND, if
 * you keep holding, also lets you edit it; these don't conflict, they're
 * just both true for the same gesture. A fresh touch while circle is
 * ALREADY held instead goes straight to ratchet-edit, no hold needed --
 * see this file's own "Per-step editing" section for why ratchet moved
 * off the hold-escalation timeline entirely. */
static void seq_handle_step_taps(uint32_t now_ms) {
    op_seq_pattern_t *pat = active_pattern();
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        uint8_t step = (uint8_t)(pad - 1u);
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_seq_prev_pad_touched[step];
        if (touched && !was_touched) {
            if (tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID)) {
                edit_enter_ratchet(step);
                return; /* grid ownership just changed under this loop -- stop iterating it */
            }
            s_seq_step_touch_started_ms[step] = now_ms;
        } else if (touched) {
            if (s_seq_step_touch_started_ms[step] != 0u &&
                (now_ms - s_seq_step_touch_started_ms[step]) >= OP_SEQ_PITCH_ASSIGN_HOLD_MS) {
                edit_enter(step, s_seq_step_touch_started_ms[step]);
                return; /* grid ownership just changed under this loop -- stop iterating it */
            }
        } else {
            /* Released -- a genuine tap-and-release commits the arm
             * toggle HERE, not on the original press. Real feedback:
             * "when setting the pitch of pad we are still affecting note
             * on" -- toggling immediately on press (the old behavior)
             * meant holding an already-armed step to open pitch/ratchet
             * edit silently disarmed it first, before the press even had
             * a chance to reveal itself as a hold rather than a tap.
             * started_ms == 0 here means this touch's lifecycle was never
             * tracked start-to-finish by this loop -- e.g. it's the pad a
             * pitch-edit pick-a-pad commit just consumed instead (see
             * edit_exit()'s own resync), which already did its job and
             * shouldn't ALSO arm/disarm the step it landed on. */
            if (s_seq_step_touch_started_ms[step] != 0u) {
                pat->step_armed[step] = !pat->step_armed[step];
            }
            s_seq_step_touch_started_ms[step] = 0u;
        }
        s_seq_prev_pad_touched[step] = touched;
    }
}

/* Takes the clock snapshot as a parameter rather than calling
 * tiles_midi_clock_get_state() itself -- that function consumes
 * (clears) start_edge as a side effect, and tiles_op_mode_scan() also
 * needs the same snapshot for the beat flash (see this file's own
 * "Master tap tempo" section) -- fetching it twice in one scan would
 * silently drop a real start_edge on whichever call ran second. */
static void seq_advance_clock(tiles_midi_clock_state_t clock) {
    if (clock.start_edge) {
        seq_reset(clock.pulse_count);
        return;
    }

    if (!clock.running) {
        seq_end_current_note();
        return;
    }

    if (s_seq_pending_start) {
        /* Real feedback: "we need to quantice to midi clock when that is
         * conected" -- confirmed meaning: entering sequencer mode (or
         * pressing "+") while a clock is already running waits for the
         * next quarter-note boundary rather than jumping in at whatever
         * pulse_count % OP_SEQ_CLOCKS_PER_STEP happens to be right now. A
         * fresh Start/tap-tempo establishment already took the branch
         * above instead (start_edge implies phase-zero already), so this
         * only ever waits for a resume mid-stream.
         * s_seq_pending_restart decides WHAT happens once that boundary
         * arrives -- real feedback pinned this down precisely: "play when
         * playing brings head to start point again" (restart, step 0) is
         * a genuinely different action from "when stopped makes play"
         * (resume exactly where a plain stop left it, no reset). */
        if ((clock.pulse_count % OP_CLOCK_PULSES_PER_BEAT) != 0u) {
            return;
        }
        s_seq_pending_start = false;
        if (s_seq_pending_restart) {
            seq_reset(clock.pulse_count);
        } else {
            seq_resume_current_step(clock.pulse_count);
        }
        return;
    }

    /* Real feedback: "2 retrigger yess but we need to be able to control
     * that feature." Fires any ratchet sub-hits due WITHIN the current
     * step BEFORE checking for a step boundary below, so a hit scheduled
     * right at the edge of the step never gets skipped. Guarded, not an
     * unbounded while(), matching this function's own "handle more than
     * one due" pattern elsewhere. */
    uint32_t ratchet_guard = 0u;
    while (s_seq_ratchet_remaining > 0u && clock.pulse_count >= s_seq_next_ratchet_pulse && ratchet_guard < OP_SEQ_MAX_RATCHET) {
        seq_fire_note(s_seq_current_step);
        s_seq_ratchet_remaining--;
        s_seq_next_ratchet_pulse += s_seq_ratchet_interval_pulses;
        ratchet_guard++;
    }

    uint32_t elapsed = clock.pulse_count - s_seq_step_started_at_pulse;
    if (elapsed < OP_SEQ_CLOCKS_PER_STEP) {
        return;
    }
    /* Handles more than one step's worth of pulses having accumulated
     * between scans (robust regardless of main-loop iteration rate vs
     * incoming clock rate), not just the common one-step case. */
    uint32_t steps_to_advance = elapsed / OP_SEQ_CLOCKS_PER_STEP;
    s_seq_step_started_at_pulse += steps_to_advance * OP_SEQ_CLOCKS_PER_STEP;
    uint8_t length = active_pattern()->length;
    uint8_t new_step = (uint8_t)((s_seq_current_step + steps_to_advance) % length);
    seq_enter_step(new_step);
}

static void render_sequencer(float beat_flash_level, bool transport_running) {
    op_seq_pattern_t *pat = active_pattern();
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            uint8_t step = (uint8_t)(pad - 1u);
            bool is_current = (step == s_seq_current_step);
            if (step >= pat->length) {
                /* Out of the current loop -- "unavailable are off," this
                 * codebase's own established rule, unchanged. */
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            } else if (is_current && pat->step_armed[step]) {
                /* Real feedback: "make play head on active pad in
                 * sewquencer blue if pad active so it wont look as an
                 * inactive pad" -- then, after trying it: "play head is
                 * still white on play when going over selected step."
                 * s_seq_note_sounding is only ever true for an ARMED step
                 * (seq_enter_step() returns early for an unarmed one
                 * before ever setting it), so the two used to be separate
                 * tiers with sounding checked FIRST and winning as plain
                 * bright white -- meaning the one moment this color most
                 * needed to read as "armed" (actually playing) was
                 * exactly when it didn't. Armed now always wins here,
                 * sounding or not -- one consistent blue for "the cursor
                 * is on an armed step," full stop. */
                tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_CURSOR_ARMED_R, OP_SEQ_CURSOR_ARMED_G, OP_SEQ_CURSOR_ARMED_B);
            } else if (is_current) {
                /* Real feedback: "we need cuentet stept to be lit up
                 * always" -- a dim cursor marks the playhead even on an
                 * unarmed step, or while paused/waiting for a quantized
                 * start (see seq_advance_clock()'s pending-start
                 * handling). Supersedes this file's own earlier explicit
                 * "no playhead flash on an empty step" rule. */
                tiles_lighting_set_standby_pad_rgb(pad, OP_SEQ_CURSOR_LEVEL, OP_SEQ_CURSOR_LEVEL, OP_SEQ_CURSOR_LEVEL);
            } else if (!pat->step_armed[step]) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
            } else {
                /* Tints the dim-red armed color toward amber if this step
                 * has a real chance of getting skipped this loop (only
                 * shown while probability_enabled -- an untinted dialed
                 * value that's currently inert would be misleading), and
                 * toward blue if it's set to ratchet -- an at-a-glance
                 * "this step has something going on" cue without needing
                 * to re-open its own edit view to check. */
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
    /* Diamond deliberately left at its default 0.0f here -- real
     * feedback: "the led for modes should light up on menu on not
     * alwayus." It used to glow continuously (OP_DIAMOND_LED_MODE_
     * ACTIVE_LEVEL) for the whole time any non-melodic mode was active;
     * now it only ever lights while the top-level mode picker itself is
     * actually open (render_menu()'s own OP_TRIANGLE_LED_MENU_LEVEL). */
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_CIRCLE_BUTTON_COL) {
            /* Real feedback: "flash that light as the tempo." */
            level = beat_flash_level;
        } else if (col == TILES_MINUS_BUTTON_COL) {
            /* Real feedback: "the led for start and top shoukld light up
             * as toggles respectively" -- exactly one of stop/start is
             * ever lit, reflecting current transport state. */
            level = transport_running ? 0.0f : OP_TRANSPORT_LED_LEVEL;
        } else if (col == TILES_PLUS_BUTTON_COL) {
            level = transport_running ? OP_TRANSPORT_LED_LEVEL : 0.0f;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* ---- Per-step editing: pitch, probability, ratchet ----------------------
 * See this file's own "Per-step editing" state-section comment for the
 * full design (escalating hold thresholds, pitch's toggle shape vs
 * probability/ratchet's live-dial shape). Silences whatever's currently
 * sounding the instant this opens rather than leaving a note stuck on for
 * the whole gesture -- the sequencer's own timeline keeps advancing
 * underneath regardless (this doesn't pause playback), so returning to
 * normal view simply catches up and resumes wherever the clock already
 * is, the same way this file's other sub-views have always worked. */
static void edit_enter(uint8_t step, uint32_t started_ms) {
    seq_end_current_note();
    s_seq_edit_mode = OP_SEQ_EDIT_PITCH;
    s_seq_edit_step = step;
    s_seq_edit_started_ms = started_ms;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pitch_edit_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
}

/* Ratchet's own, separate entry point -- real feedback: "time escalation
 * is good but not for so many features." Reached directly (circle held,
 * then the step touched -- see seq_handle_step_taps()'s own check), no
 * hold-duration wait at all, rather than a third escalation tier off the
 * same timeline pitch/probability already share. */
static void edit_enter_ratchet(uint8_t step) {
    seq_end_current_note();
    s_seq_edit_mode = OP_SEQ_EDIT_RATCHET;
    s_seq_edit_step = step;
}

static void edit_exit(void) {
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    /* Re-syncs seq_handle_step_taps()'s own touch tracking so a pad
     * that's still touched the instant control hands back doesn't misread
     * as a fresh arm-toggle -- same pattern this file already uses
     * whenever a sub-view that owned the grid closes. */
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_seq_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        s_seq_step_touch_started_ms[i] = 0u;
    }
}

static uint8_t probability_percent_from_depth(uint16_t depth) {
    /* Same ~900 full-scale reference this file already uses
     * (OP_MENU_SELECT_DEPTH_THRESHOLD's own comment). */
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

/* Pitch: a genuine TOGGLE, not a hold-and-don't-let-go gesture -- real
 * feedback: "it should be a toggle to set pitch of sequencer note. not a
 * momentary thing." Once opened, it stays open regardless of whether the
 * originally-held step pad is still touched -- release it freely, no
 * finger has to stay down. A fresh touch on ANY pad (the held step
 * included -- tapping your OWN step again re-commits it to its own
 * current note, a harmless no-op unless it had a different override
 * before, in which case it resets to default) commits that pad's current
 * note and closes, same "closes on selection" rule this file's other
 * pickers use. Diamond is the escape hatch for "back out with no change
 * at all" (handle_diamond_click()'s own branch).
 * Probability/ratchet: the OPPOSITE shape, a live DIAL -- Hall depth of
 * the SAME held pad maps continuously to the value while still held
 * (see probability_percent_from_depth()/ratchet_count_from_depth()
 * above), and releasing simply leaves whatever the last-read value was.
 * This needs the one pad to stay down the whole time, unlike pitch --
 * but unlike pitch, there's no second pad to also reach for, so it never
 * has pitch's original "two fingers, one of them pinned down" problem. */
static void handle_edit_mode(uint32_t now_ms) {
    uint8_t edit_pad = (uint8_t)(s_seq_edit_step + 1u);
    bool edit_pad_touched = tiles_touch_is_touched(edit_pad);

    if (s_seq_edit_mode == OP_SEQ_EDIT_PITCH) {
        if (edit_pad_touched && (now_ms - s_seq_edit_started_ms) >= OP_SEQ_PROBABILITY_HOLD_MS) {
            /* Real feedback: "yes per step probablility... 2 retrigger
             * yess but we need to be able to control that feature" --
             * escalates exactly like services/standby.h's own circle-hold
             * (4s screensaver -> 8s deep sleep) rather than a new gesture. */
            s_seq_edit_mode = OP_SEQ_EDIT_PROBABILITY;
            return;
        }
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            bool touched = tiles_touch_is_touched(pad);
            bool was_touched = s_pitch_edit_prev_pad_touched[pad - 1u];
            if (touched && !was_touched) {
                op_seq_pattern_t *pat = active_pattern();
                pat->step_pitch_override[s_seq_edit_step] = true;
                pat->step_note[s_seq_edit_step] = tiles_note_map_get_note(pad);
                tiles_haptics_trigger_touch_pulse(pad);
                edit_exit();
                return; /* grid ownership just changed under this loop -- stop iterating it */
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
            /* Live both ways (up or down) above the guard -- see
             * OP_SEQ_EDIT_RELEASE_GUARD_DEPTH's own comment for why only
             * the final near-zero approach (below the guard) is ignored,
             * not every decrease. */
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

static void render_transport_toggle_leds(bool transport_running) {
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_MINUS_BUTTON_COL) {
            level = transport_running ? 0.0f : OP_TRANSPORT_LED_LEVEL;
        } else if (col == TILES_PLUS_BUTTON_COL) {
            level = transport_running ? OP_TRANSPORT_LED_LEVEL : 0.0f;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
}

/* Reuses services/lighting.c's own melodic-idle note-role coloring
 * (root=magenta, natural=white, sharp=off, via the same
 * tiles_note_map_is_root_pad()/_is_natural_pad() accessors) so the "pick a
 * note" surface looks like a natural extension of melodic play rather
 * than a new visual language -- the step's currently-assigned note pulses
 * white instead, this file's own established "selected" language. */
static void render_pitch_edit(uint32_t now_ms, bool transport_running) {
    float pulse = menu_selected_pulse_level(now_ms);
    op_seq_pattern_t *pat = active_pattern();
    uint8_t current_note = pat->step_pitch_override[s_seq_edit_step] ? pat->step_note[s_seq_edit_step]
                                                                      : tiles_note_map_get_note((uint8_t)(s_seq_edit_step + 1u));

    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (tiles_note_map_get_note(pad) == current_note) {
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
    render_transport_toggle_leds(transport_running);
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

/* A simple linear "meter" across all 24 pads -- how many are lit is
 * directly proportional to the live value, so pressing deeper/shallower
 * gives immediate, legible visual feedback of exactly what Hall depth is
 * currently dialing in. */
static void render_value_meter(uint8_t lit_count, float r, float g, float b, bool transport_running) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        if (pad <= lit_count) {
            tiles_lighting_set_standby_pad_rgb(pad, r, g, b);
        } else {
            tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
        }
    }
    render_transport_toggle_leds(transport_running);
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
        render_value_meter(lit, 1.0f, 0.8f, 0.0f, transport_running); /* amber */
        break;
    }
    case OP_SEQ_EDIT_RATCHET: {
        uint8_t count = active_pattern()->step_ratchet_count[s_seq_edit_step];
        uint8_t lit = (uint8_t)((uint32_t)count * TILES_NUM_PADS / OP_SEQ_MAX_RATCHET);
        render_value_meter(lit, 0.0f, 0.4f, 1.0f, transport_running); /* blue */
        break;
    }
    default:
        break;
    }
}

/* ---- Pattern/channel picker (SW4/diamond, sequencer mode) --------------- */

static void pattern_row_color(uint8_t pattern_index, float *r, float *g, float *b) {
    switch (pattern_index) {
    case 0u:
        *r = OP_PATTERN_1_R;
        *g = OP_PATTERN_1_G;
        *b = OP_PATTERN_1_B;
        break;
    case 1u:
        *r = OP_PATTERN_2_R;
        *g = OP_PATTERN_2_G;
        *b = OP_PATTERN_2_B;
        break;
    case 2u:
        *r = OP_PATTERN_3_R;
        *g = OP_PATTERN_3_G;
        *b = OP_PATTERN_3_B;
        break;
    default: /* pattern 3 */
        *r = OP_PATTERN_4_R;
        *g = OP_PATTERN_4_G;
        *b = OP_PATTERN_4_B;
        break;
    }
}

static void render_pattern_menu(uint32_t now_ms, bool transport_running) {
    float pulse = menu_selected_pulse_level(now_ms);
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t pattern_index = (uint8_t)(row - (TILES_GRID_MIN_ROW + 1u));
        bool selected = (pattern_index == s_seq_active_pattern);
        float r, g, b;
        pattern_row_color(pattern_index, &r, &g, &b);
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            if (selected) {
                tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), pulse, pulse, pulse);
            } else {
                tiles_lighting_set_standby_pad_rgb(board_pad_for_row_col(row, col), r * OP_SCALE_AVAILABLE_LEVEL,
                                                    g * OP_SCALE_AVAILABLE_LEVEL, b * OP_SCALE_AVAILABLE_LEVEL);
            }
        }
    }
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_DIAMOND_BUTTON_COL) {
            level = OP_DIAMOND_LED_SUBMENU_LEVEL;
        } else if (col == TILES_MINUS_BUTTON_COL) {
            level = transport_running ? 0.0f : OP_TRANSPORT_LED_LEVEL;
        } else if (col == TILES_PLUS_BUTTON_COL) {
            level = transport_running ? OP_TRANSPORT_LED_LEVEL : 0.0f;
        } else if (col == TILES_CIRCLE_BUTTON_COL) {
            /* A plain circle click here toggles this -- see
             * handle_circle_tap()'s own release branch. */
            level = active_pattern()->probability_enabled ? 1.0f : 0.0f;
        }
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void pattern_menu_exit(void);

/* Same touch-click + push-past-50%-selects gesture as handle_menu_taps()
 * (the top-level mode picker), reused verbatim since 4 patterns mapping
 * onto 4 rows is the same shape as 4 modes mapping onto 4 rows. */
static void handle_pattern_menu_taps(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t pattern_index = (uint8_t)(row - (TILES_GRID_MIN_ROW + 1u));
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            if (touched && !s_pattern_menu_prev_pad_touched[pad - 1u]) {
                tiles_haptics_trigger_touch_pulse(pad);
            }
            if (touched && (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD) {
                if (pattern_index != s_seq_active_pattern) {
                    seq_end_current_note();
                    /* A ratchet mid-sequence when switching would
                     * otherwise fire its remaining hits against the NEW
                     * pattern's data at the same step index once
                     * seq_advance_clock() next checks -- a cross-pattern
                     * mix-up seq_enter_step()'s own reset doesn't reach
                     * here since this path doesn't go through it. */
                    s_seq_ratchet_remaining = 0u;
                    s_seq_active_pattern = pattern_index;
                    printf("[op_mode] sequencer pattern -> %u\n", (unsigned)pattern_index);
                }
                pattern_menu_exit();
                return; /* grid ownership just changed under this loop -- stop iterating it */
            }
            s_pattern_menu_prev_pad_touched[pad - 1u] = touched;
        }
    }
}

static void pattern_menu_enter(void) {
    seq_end_current_note();
    s_pattern_menu_visible = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pattern_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
}

static void pattern_menu_exit(void) {
    s_pattern_menu_visible = false;
}

/* ---- Menu -------------------------------------------------------------- */

static void menu_enter(void) {
    s_menu_visible = true;
    /* The mode-picker takes priority over a still-open per-mode
     * sub-menu -- can't sensibly show both at once. */
    s_scale_menu_visible = false;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void menu_exit(void) {
    s_menu_visible = false;
    if (s_active_mode != OP_MODE_SEQUENCER) {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    /* Real bug found from real feedback: "load a fix for exiting menues,
     * led stays toggled." (Diamond back then, per that bug's original
     * discovery -- triangle now, since the mode-picker button is
     * triangle's job post-swap; see this file's own header note.)
     * Triangle has a PERMANENT override claimed (see tiles_op_mode_init()),
     * so buttons.c's own refresh_all_button_leds() (run by tiles_buttons_
     * set_standby_active(false) above) deliberately SKIPS it -- "that
     * controller's own next scan repaints it correctly" is buttons.h's
     * own documented contract, but nothing was actually doing that
     * repaint here. Canceling the menu with a triangle click (as opposed
     * to SELECTING a mode, which routes through set_active_mode() --
     * that function's own trailing override write already covers this)
     * left triangle stuck at render_menu()'s own OP_TRIANGLE_LED_MENU_
     * LEVEL (bright) forever, since nothing wrote to it again afterward.
     * The menu is only ever reachable from melodic mode to begin with, so
     * 0.0f (melodic's own "triangle off" state) is always the correct
     * value to restore here, unconditionally. */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
}

/* Real feedback: "the mode selector has all these lights always on. only
 * availabkle modes shouyld be on meaning for now only sequencer, and the
 * note mode" (plus, with this round's guitar mode added, guitar itself,
 * and now chord mode too -- "so this would go as our 4th play mode").
 * Matches the "unavailable = off and not selectable" language this file
 * already established for the scale/pattern pickers' own reserved slots
 * -- see handle_menu_taps() below for the "not selectable" half. */
static bool col_is_available(uint8_t col) {
    switch (col) {
    case OP_MENU_COL_MELODIC:
    case OP_MENU_COL_SEQUENCER:
    case OP_MENU_COL_GUITAR:
    case OP_MENU_COL_CHORD:
        return true;
    default: /* outside the menu entirely */
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
    default: /* OP_MENU_COL_GUITAR -- only ever called for an available
              * column (see render_menu() below), so this catch-all is
              * safe: melodic/sequencer/chord are handled above, leaving
              * only guitar. */
        *r = OP_MENU_GUITAR_R;
        *g = OP_MENU_GUITAR_G;
        *b = OP_MENU_GUITAR_B;
        break;
    }
}

/* Column <-> mode, the inverse of handle_menu_taps()'s own col->mode
 * switch -- used only to find which single slot (if any) is the mode
 * already active, for render_menu()'s pulsing "current mode" state.
 * Every case listed explicitly (no catch-all default beyond false) --
 * unlike render_menu_col_color() above, this is called for EVERY column
 * including the ones outside the menu's 4 slots, so a careless default
 * here would misreport some other column as "chord," pulsing a pad that
 * isn't a mode slot at all. */
static bool col_is_current_mode(uint8_t col) {
    switch (col) {
    case OP_MENU_COL_MELODIC:
        return s_active_mode == OP_MODE_MELODIC;
    case OP_MENU_COL_SEQUENCER:
        return s_active_mode == OP_MODE_SEQUENCER;
    case OP_MENU_COL_GUITAR:
        return s_active_mode == OP_MODE_GUITAR;
    case OP_MENU_COL_CHORD:
        return s_active_mode == OP_MODE_CHORD;
    default:
        return false;
    }
}

/* Real feedback: "the row thing for the mode menu on triangle is bad...
 * like the minigame menu" -- see OP_MENU_ROW/OP_MENU_COL_*'s own comment
 * for the reference this copies (services/game_mode.h's game-select
 * screen: one pad per game, packed onto row 1, nothing else lit). Same
 * color rules an earlier round already established for the row-based
 * version, just applied to a single pad per mode instead of an entire
 * row: the current mode's pad pulses white (this file's one "selected"
 * language, reused rather than invented fresh), any OTHER available
 * mode's pad shows its own hue dimmed to the readable-secondary level
 * the scale picker's own "available but not selected" pads already use
 * (OP_SCALE_AVAILABLE_LEVEL), chord's reserved slot and every pad
 * outside the menu row stay fully off. */
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

/* ---- Melodic mode's sub-menu: the scale picker -------------------------
 * "Ableton Push style": one pad per scale, the whole 24-pad grid, no
 * row/column structure -- see note_map.h's TILES_NOTE_MAP_NUM_SCALE_GRID_
 * SLOTS and the 18 named scales + 6 reserved slots that exactly fill it.
 * Follows the standardized menu language above: selected = white bright
 * pulsing, available-but-unselected = Sentia magenta at a readable-but-
 * secondary level, a reserved/undefined slot = fully off and not
 * selectable at all (tapping one is a no-op, see handle_scale_menu_taps()
 * below). */
/* Real feedback: "selected pad give a constant haptic pulsing pattern to
 * indicate its the active one." Fires tiles_haptics_trigger_kick() on
 * the currently-selected pad every OP_MENU_SELECTED_PULSE_PERIOD_MS,
 * synced to the same period the visual pulse uses -- a repeating pulse,
 * not a continuous buzz (tiles_haptics_set_sustain_level() needs an
 * already-active voice from trigger_kick() to mean anything, so a real
 * periodic re-kick is what actually produces a felt "pattern" here). */
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
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? OP_DIAMOND_LED_SUBMENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
    }
}

static void scale_menu_exit(void);

/* Real feedback: "when you touch but not click in menu make a strong
 * haptic click be felt, push pad to at least mroe than 50% to sleect."
 * A fresh capacitive touch alone only fires the click (an
 * acknowledgment, "you're touching this") -- selection itself only
 * commits once Hall depth crosses OP_MENU_SELECT_DEPTH_THRESHOLD while
 * still touched.
 * Selecting now closes the menu immediately -- real feedback: "when we
 * select a menu item the menu should close not it doesnt stick around
 * until disabeled." An earlier version deliberately left it open (so
 * different scales could be tried while watching/hearing the
 * difference); real feedback reversed that call -- matches the
 * mode-picker's own close-on-select behavior now, one consistent rule
 * for both menus in this file. Re-opening (a fresh diamond click) shows
 * whatever's now selected pulsing, same as before. */
static void handle_scale_menu_taps(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        if (touched && !s_scale_menu_prev_pad_touched[pad - 1u]) {
            tiles_haptics_trigger_touch_pulse(pad);
        }
        if (touched && (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD) {
            tiles_scale_mode_t slot_scale = tiles_note_map_scale_for_grid_slot(pad);
            if (tiles_note_map_scale_is_defined(slot_scale)) {
                if (slot_scale != tiles_note_map_get_scale()) {
                    printf("[op_mode] scale -> %d\n", (int)slot_scale);
                }
                tiles_note_map_set_scale(slot_scale);
                scale_menu_exit();
                return; /* grid ownership just changed under this loop -- stop iterating it */
            }
            /* An undefined (reserved custom) slot is simply not
             * selectable -- "unavailable" per the standardized menu
             * language, not a smaller version of a real choice. */
        }
        s_scale_menu_prev_pad_touched[pad - 1u] = touched;
    }
}

static void scale_menu_enter(void) {
    s_scale_menu_visible = true;
    s_scale_menu_haptic_pulse_ms = to_ms_since_boot(get_absolute_time());
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_scale_menu_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
    }
    tiles_lighting_set_standby_active(true);
    tiles_buttons_set_standby_active(true);
}

static void scale_menu_exit(void) {
    s_scale_menu_visible = false;
    tiles_lighting_set_standby_active(false);
    tiles_buttons_set_standby_active(false);
}

static void set_active_mode(tiles_op_mode_t mode) {
    if (s_active_mode == OP_MODE_SEQUENCER && mode != OP_MODE_SEQUENCER) {
        seq_end_current_note();
    }
    if (s_active_mode == OP_MODE_CHORD && mode != OP_MODE_CHORD) {
        chord_end_all_notes();
    }
    if (mode != OP_MODE_MELODIC) {
        /* Melodic's own sub-menu can't stay open once melodic isn't the
         * active mode anymore. */
        s_scale_menu_visible = false;
    }
    if (mode != OP_MODE_SEQUENCER) {
        /* Same reasoning, sequencer's own two sub-views. */
        s_pattern_menu_visible = false;
        s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    }
    s_active_mode = mode;
    if (mode == OP_MODE_SEQUENCER) {
        seq_start();
        tiles_lighting_set_standby_active(true);
        tiles_buttons_set_standby_active(true);
    } else {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    /* Guitar mode reuses melodic's own touch/expression/lighting pipeline
     * entirely (see this file's own "Guitar/bass fret mode" section) --
     * it does NOT claim standby_active above, unlike sequencer. Pushing
     * this one flag into note_map.c is the only thing needed for
     * services/expression.c's existing note-triggering to start playing
     * guitar-mapped notes instead of scale-mapped ones, and for
     * services/lighting.c's idle coloring to switch to fret markers --
     * both already read note_map.c's own state, no changes needed there
     * beyond note_map.c/lighting.c's own guitar-awareness. */
    tiles_note_map_set_guitar_mode(mode == OP_MODE_GUITAR);
    /* Chord mode's own equivalent of the guitar-mode flag above -- pushes
     * this file's chord-strip/melody split into note_map.c so services/
     * lighting.c's idle coloring and this file's own chord_pad_note_on()/
     * tiles_op_mode_owns_pad() below all agree on which 8 pads are chord
     * pads. Seeds s_chord_pad_touched[] from whatever's ACTUALLY touched
     * right now (not false) the instant chord mode becomes active, the
     * same "don't let a finger already resting on a pad read as a fresh
     * touch" precedent scale_menu_enter() above already established --
     * otherwise a finger already on a chord pad while the mode-picker
     * menu is confirmed would fire an unintended chord the moment this
     * mode takes over. */
    tiles_note_map_set_chord_mode(mode == OP_MODE_CHORD);
    if (mode == OP_MODE_CHORD) {
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            s_chord_pad_touched[pad - 1u] = tiles_touch_is_touched(pad);
        }
    }
    if (mode == OP_MODE_GUITAR) {
        /* Real feedback: "load a fix for exiting menues, led stays
         * toggled" -- applying that same lesson here proactively rather
         * than waiting to find the identical bug again: "-"/"+" have a
         * PERMANENT override claimed by services/octave_control.c, and
         * this mode takes over their input (see tiles_op_mode_owns_
         * octave_buttons() below) without claiming standby_active, so
         * nothing else will write their LEDs while guitar mode owns
         * them -- explicitly turning them off here avoids leaving
         * whatever pattern octave_control.c's own default behavior last
         * showed stuck on screen. Leaving guitar mode needs no symmetric
         * fix: octave_control.c's own scan resumes immediately once
         * tiles_op_mode_owns_octave_buttons() goes false again, and its
         * normal logic repaints them correctly on its very next scan. */
        tiles_buttons_set_override_led(TILES_MINUS_BUTTON_ID, 0.0f);
        tiles_buttons_set_override_led(TILES_PLUS_BUTTON_ID, 0.0f);
    }
    /* Always off here -- triangle only ever lights while the mode picker
     * itself is open (render_menu()'s own write), not just because some
     * non-melodic mode happens to be active. See OP_TRIANGLE_LED_MENU_
     * LEVEL's own comment. This override write only matters for
     * CHORD/GUITAR anyway (sequencer claims standby_active, making any
     * override here a no-op per buttons.h's own contract). */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
    printf("[op_mode] active mode -> %d\n", (int)mode);
}

/* Same touch-clicks/press-past-50%-selects shape as
 * handle_scale_menu_taps() above -- see that function's own comment and
 * OP_MENU_SELECT_DEPTH_THRESHOLD's. Picking a mode still closes the menu
 * immediately (no persistent "selected, still browsing" state here the
 * way the scale picker has -- a mode activates and takes over the grid
 * the instant it's confirmed), so there's no pulsing-haptic step for
 * this menu specifically. Touch-click haptic acknowledgment still fires
 * for ANY pad on the grid (matching the old row-based version's own
 * behavior), even though selection itself now only ever fires on
 * OP_MENU_ROW's 4 slots -- see render_menu()'s own comment for why the
 * rest of the grid is otherwise unlit and unused while this menu is up. */
static void handle_menu_taps(void) {
    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t pad = board_pad_for_row_col(row, col);
            bool touched = tiles_touch_is_touched(pad);
            if (touched && !s_menu_prev_pad_touched[pad - 1u]) {
                tiles_haptics_trigger_touch_pulse(pad);
            }
            if (row == OP_MENU_ROW && touched && (float)tiles_hall_get_depth(pad) > OP_MENU_SELECT_DEPTH_THRESHOLD &&
                col_is_available(col)) {
                tiles_op_mode_t mode = OP_MODE_MELODIC;
                if (col == OP_MENU_COL_CHORD) {
                    mode = OP_MODE_CHORD;
                } else if (col == OP_MENU_COL_SEQUENCER) {
                    mode = OP_MODE_SEQUENCER;
                } else if (col == OP_MENU_COL_GUITAR) {
                    mode = OP_MODE_GUITAR;
                }
                menu_exit();
                set_active_mode(mode);
                return; /* grid ownership/state just changed under this loop -- stop iterating it */
            }
            s_menu_prev_pad_touched[pad - 1u] = touched;
        }
    }
}

/* ---- Diamond click + top-level scan -------------------------------------- */

static void handle_triangle_click(void) {
    bool held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);

    if (held && !s_triangle_was_held) {
        s_triangle_press_had_conflict = false;
    }
    if (held && tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID)) {
        /* See this file's header + s_triangle_press_had_conflict's own
         * comment -- part of game_mode.h's reserved 4-button combo, not
         * a genuine solo triangle press. */
        s_triangle_press_had_conflict = true;
    }

    if (!held && s_triangle_was_held) {
        if (!s_triangle_press_had_conflict) {
            if (s_menu_visible) {
                menu_exit();
            } else if (s_active_mode != OP_MODE_MELODIC) {
                set_active_mode(OP_MODE_MELODIC);
            } else {
                menu_enter();
            }
        }
    }

    s_triangle_was_held = held;
}

/* Same shape as handle_triangle_click() above, one level down: toggles
 * whichever per-mode sub-menu the CURRENT mode has -- melodic's is the
 * scale picker, sequencer's is the pattern/channel picker (real feedback:
 * "sub menu triangle is reserved for other stuff... maybe in triangle we
 * can select midi channels for multiple patterns"). Guarded against the
 * mode-picker also being open (a sub-menu click while picking a top-level
 * mode would be ambiguous/unwanted) the same way the mode-picker itself is
 * guarded against other_feature_owns_input().
 * While any per-step edit (pitch/probability/ratchet) owns the grid,
 * diamond instead cancels it with no change -- the escape hatch a
 * toggle-style gesture needs (real feedback: "it should be a toggle to
 * set pitch of sequencer note, not a momentary thing" -- see this file's
 * own "Per-step editing" section for the rest of that change). */
static void handle_diamond_click(void) {
    bool held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);

    if (held && !s_diamond_was_held) {
        s_diamond_press_had_conflict = false;
    }
    if (held && tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID)) {
        s_diamond_press_had_conflict = true;
    }

    if (!held && s_diamond_was_held) {
        if (!s_diamond_press_had_conflict && !s_menu_visible) {
            if (s_active_mode == OP_MODE_MELODIC) {
                if (s_scale_menu_visible) {
                    scale_menu_exit();
                } else {
                    scale_menu_enter();
                }
            } else if (s_active_mode == OP_MODE_SEQUENCER) {
                if (s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
                    edit_exit();
                } else if (s_pattern_menu_visible) {
                    pattern_menu_exit();
                } else {
                    pattern_menu_enter();
                }
            }
        }
    }

    s_diamond_was_held = held;
}

/* Real feedback: "master tap tempo on the instrument with shit round
 * button when not derecting midi clock from a daw. the tapp tempo is
 * only active in sequencer and arp mode." Qualifies a press as a genuine
 * tap candidate only if: sequencer or arp mode is the active mode, no
 * real external clock is currently detected (services/midi_clock.h's own
 * tiles_midi_clock_register_tap() also defensively re-checks this), and
 * none of game_mode.h's other three reserved combo buttons (SW3
 * triangle/SW4 diamond/SW5 square) are ALSO currently held -- the same
 * "not part of the 4-button combo" heuristic diamond/triangle's own click
 * handlers use.
 * Committed on RELEASE, not press -- a revision from this feature's first
 * pass, needed once handle_transport_and_length() below started using
 * "hold circle, then press -/+" for pattern length: circle's OWN press
 * happens chronologically before minus/plus is ever touched, so the
 * combo-conflict check above (which only looks at sibling state AT
 * press-time) can't catch that sequential gesture -- a length-adjust
 * combo would otherwise always register a spurious tap first. Deferring
 * to release, gated on whether minus/plus joined mid-hold (below), fixes
 * this while still using the ORIGINAL press timestamp for the actual
 * registered tap time, so tap-tempo accuracy is unaffected by the small
 * press-to-release latency of a real tap.
 * Also excludes the pattern picker and any per-step edit view from
 * `mode_ok` -- tapping a tempo while mid-edit doesn't make sense anyway,
 * and it frees up a plain circle CLICK while the pattern picker is open
 * to mean something else instead: toggling that pattern's probability_
 * enabled (see the release branch below).
 * Mid-hold cancellation ALSO checks for any pad touch now, not just
 * minus/plus -- real feedback moved ratchet-edit onto a "hold circle,
 * then touch a step" combo (see seq_handle_step_taps()'s own check),
 * which is exactly the same "circle-first" ordering length-adjust
 * already needed this exact fix for. */
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
        /* ARP mode (the original other half of "only active in sequencer
         * and arp mode") has been removed entirely -- see the mode enum's
         * own comment -- so tap tempo is sequencer-only now. */
        bool mode_ok = (s_active_mode == OP_MODE_SEQUENCER && !s_pattern_menu_visible && s_seq_edit_mode == OP_SEQ_EDIT_NONE);
        bool combo_conflict = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
        s_circle_press_pending_tap = mode_ok && !combo_conflict && !tiles_midi_clock_external_active(now_ms);
    }

    if (held && s_circle_press_pending_tap &&
        (tiles_button_is_pressed(TILES_MINUS_BUTTON_ID) || tiles_button_is_pressed(TILES_PLUS_BUTTON_ID) || any_pad_touched())) {
        /* This hold became a length-adjust or ratchet-edit combo instead
         * -- cancel candidacy so it doesn't ALSO register as a spurious
         * tap. */
        s_circle_press_pending_tap = false;
    }

    if (!held && s_circle_was_held) {
        if (s_active_mode == OP_MODE_SEQUENCER && s_pattern_menu_visible) {
            /* Real feedback: "yes per step probablility but we should be
             * able to turn that on and off." A per-pattern master switch,
             * toggled here rather than inside the picker's own tap
             * handling since circle is otherwise unclaimed while the
             * picker is open (length-adjust is disabled there too -- see
             * handle_transport_and_length()'s own `active` gate). */
            op_seq_pattern_t *pat = active_pattern();
            pat->probability_enabled = !pat->probability_enabled;
            printf("[op_mode] pattern %u probability_enabled -> %d\n", (unsigned)s_seq_active_pattern,
                   (int)pat->probability_enabled);
        } else if (s_circle_press_pending_tap) {
            tiles_midi_clock_register_tap(s_circle_press_ms);
        }
    }

    s_circle_was_held = held;
}

/* SW1/SW2 ("-"/"+") -- sequencer mode's own transport/length (unchanged),
 * plus guitar mode's fret-window shift (real feedback: "-+ change frets
 * up and down"). One function, not two, so both share a single press-
 * tracking read/update of these two buttons rather than each keeping its
 * own redundant copy -- the two modes are mutually exclusive, so there's
 * never a real conflict over what a release should mean. See this file's
 * own "Transport + length" state-section comment for sequencer's own
 * reasoning (circle-first-then-"-"/"+" ordering, the octave_control.c
 * solo-vs-combo precedent this mirrors). Guitar's own fret-shift needs
 * none of that -- no combo, just a plain step on release, matching
 * services/octave_control.c's own "resolves on release" convention for
 * its default octave-shift function (the one this mode is effectively
 * replacing for as long as it's active). */
static void handle_transport_and_length(uint32_t now_ms) {
    bool minus_held = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID);
    bool plus_held = tiles_button_is_pressed(TILES_PLUS_BUTTON_ID);
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
    bool active = (s_active_mode == OP_MODE_SEQUENCER) && !s_pattern_menu_visible && s_seq_edit_mode == OP_SEQ_EDIT_NONE;
    bool guitar_active = (s_active_mode == OP_MODE_GUITAR);

    if (active && minus_held && !s_minus_was_held && circle_held) {
        op_seq_pattern_t *pat = active_pattern();
        if (pat->length > OP_SEQ_MIN_LENGTH) {
            pat->length--;
        }
        s_minus_used_as_combo = true;
    }
    if (active && plus_held && !s_plus_was_held && circle_held) {
        op_seq_pattern_t *pat = active_pattern();
        if (pat->length < OP_SEQ_MAX_LENGTH) {
            pat->length++;
        }
        s_plus_used_as_combo = true;
    }

    if (!minus_held && s_minus_was_held) {
        if (active && !s_minus_used_as_combo) {
            /* Real feedback: "we need a button that starts and stops
             * sequencer... play position of head should reset when stop
             * click twice." tiles_midi_clock_is_running() reflects
             * whatever it was for this ENTIRE press (transport state
             * can't change from a button this file itself doesn't touch
             * anywhere else), so this cleanly distinguishes "stop while
             * playing" (pause in place) from "stop while ALREADY
             * stopped" (a second stop -- Ableton-style rewind to the top,
             * without resuming playback). */
            if (tiles_midi_clock_is_running()) {
                tiles_midi_clock_set_running(false);
                s_seq_pending_start = false;
            } else {
                s_seq_current_step = 0u;
                s_seq_note_sounding = false;
                s_seq_pending_start = false;
            }
        } else if (guitar_active) {
            /* Real feedback: "-+ change frets up and down." One fret per
             * press, no auto-repeat -- matches this codebase's own
             * established "-"/"+" convention everywhere else (octave
             * shift, key transpose, sequencer length all step by 1 per
             * press). */
            uint8_t offset = tiles_note_map_get_guitar_fret_offset();
            if (offset > 0u) {
                tiles_note_map_set_guitar_fret_offset((uint8_t)(offset - 1u));
            }
        }
        s_minus_used_as_combo = false;
    }
    if (!plus_held && s_plus_was_held) {
        if (active && !s_plus_used_as_combo) {
            if (tiles_midi_clock_is_running()) {
                /* Real feedback: "if playing and play again it starts
                 * from the top again" -- a retrigger, not a no-op.
                 * Re-arming pending-start quantizes the restart to the
                 * next beat boundary exactly like a fresh start would
                 * (see seq_advance_clock()), rather than snapping to step
                 * 0 mid-beat. pending_restart=true is what makes this
                 * land back on step 0 rather than replaying wherever the
                 * playhead already was. */
                s_seq_pending_start = true;
                s_seq_pending_restart = true;
            } else if (tiles_midi_clock_tap_tempo_established() || tiles_midi_clock_external_active(now_ms)) {
                /* Only meaningful once a tempo actually exists --
                 * otherwise this would set `running` true with nothing
                 * to ever advance pulse_count, an inert state rather
                 * than "playing." s_seq_pending_start quantizes the
                 * resume to the next beat boundary instead of fast-
                 * forwarding through however many pulses accumulated
                 * while stopped (pulse_count keeps advancing even while
                 * !running -- see midi_clock.h's own header).
                 * pending_restart=false: real feedback: "when stopped
                 * makes play" -- resumes exactly where a plain stop left
                 * the playhead (or step 0, if a double-stop rewound it
                 * first), never resetting position on its own. */
                tiles_midi_clock_set_running(true);
                s_seq_pending_start = true;
                s_seq_pending_restart = false;
            }
        } else if (guitar_active) {
            /* tiles_note_map_set_guitar_fret_offset() clamps internally
             * (GUITAR_MAX_FRET_OFFSET), no bound check needed here. */
            tiles_note_map_set_guitar_fret_offset((uint8_t)(tiles_note_map_get_guitar_fret_offset() + 1u));
        }
        s_plus_used_as_combo = false;
    }

    s_minus_was_held = minus_held;
    s_plus_was_held = plus_held;
}

/* Real feedback: "flash that light as the tempo even when midi sync
 * flash the tempo there." Fires once per quarter note (24 clock pulses,
 * the MIDI spec's own resolution), regardless of whether pulse_count is
 * currently advancing from real external clock bytes or the internal
 * tap-tempo generator -- see services/midi_clock.h's own file header for
 * why that distinction doesn't need to leak into this function at all. */
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

void tiles_op_mode_init(void) {
    s_active_mode = OP_MODE_MELODIC;
    s_menu_visible = false;
    s_triangle_was_held = false;
    s_triangle_press_had_conflict = false;
    s_scale_menu_visible = false;
    s_diamond_was_held = false;
    s_diamond_press_had_conflict = false;
    for (uint8_t p = 0; p < OP_SEQ_NUM_PATTERNS; p++) {
        for (uint8_t i = 0; i < OP_SEQ_NUM_STEPS; i++) {
            s_seq_pattern[p].step_armed[i] = false;
            s_seq_pattern[p].step_pitch_override[i] = false;
            s_seq_pattern[p].step_note[i] = 0u;
            s_seq_pattern[p].step_probability_percent[i] = 100u;
            s_seq_pattern[p].step_ratchet_count[i] = 1u;
        }
        s_seq_pattern[p].probability_enabled = false;
        s_seq_pattern[p].length = OP_SEQ_NUM_STEPS;
        /* Claims from the TOP of the 15 MPE Member Channels downward --
         * see this file's own "Multi-pattern bank" section. */
        s_seq_pattern[p].channel =
            (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + TILES_MIDI_MPE_NUM_MEMBER_CHANNELS - 1u - p);
    }
    s_seq_active_pattern = 0u;
    s_seq_current_step = 0u;
    s_seq_note_sounding = false;
    s_seq_step_started_at_pulse = 0u;
    s_seq_pending_start = false;
    s_seq_pending_restart = false;
    s_seq_ratchet_remaining = 0u;
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    s_pattern_menu_visible = false;
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
    s_boot_relight_guard_until_ms = to_ms_since_boot(get_absolute_time()) + OP_BOOT_RELIGHT_GUARD_MS;
}

void tiles_op_mode_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* See s_boot_relight_guard_until_ms's own comment -- re-asserts
     * triangle's override LED off on every scan for a short window after
     * boot, self-healing whatever occasionally corrupts the one-shot
     * write tiles_op_mode_init() already made. A no-op once the window
     * closes; also a no-op whenever render_menu() actually owns the LED
     * (tiles_buttons_set_override_led()'s own standby-active guard),
     * so this can't fight a menu that's genuinely showing at boot. */
    if (now_ms < s_boot_relight_guard_until_ms) {
        tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
    }

    if (other_feature_owns_input()) {
        /* Keep edge-tracking current so a button/touch already active the
         * instant control hands back doesn't misread as a fresh
         * click/tap -- same pattern services/expression_control.c and
         * services/game_mode.c already use for their own equivalent
         * guards. */
        s_triangle_was_held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
        s_diamond_was_held = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID);
        s_circle_was_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);
        s_minus_was_held = tiles_button_is_pressed(TILES_MINUS_BUTTON_ID);
        s_plus_was_held = tiles_button_is_pressed(TILES_PLUS_BUTTON_ID);
        /* Real bug found from real feedback: "somepads get haptics stuck
         * idk why." Whenever some other feature (standby/deep sleep most
         * commonly, since sequencer mode can legitimately run unattended
         * for a while) takes over input, this function stops running
         * entirely -- seq_advance_clock() (the only thing that would
         * otherwise eventually call seq_end_current_note() for whatever
         * step is currently sounding) never gets another chance to run
         * until control comes back AND a new step is entered. If a
         * sequencer note happened to be sounding at that exact moment,
         * its haptic motor's SUSTAIN phase (which never decays to true
         * zero on its own -- see haptics.c's own sustain_target_duty())
         * would keep buzzing at a low floor level for the entire time
         * something else owns the board. Ending it here, the instant
         * control is lost, closes that gap; idempotent (seq_end_current_
         * note() is a no-op once nothing's sounding), so calling it every
         * scan while frozen here is harmless. */
        if (s_seq_note_sounding) {
            seq_end_current_note();
        }
        /* Same reasoning as the sequencer case just above, applied to
         * chord mode's own directly-driven notes: whatever's sounding on
         * the chord strip when some other feature takes the board (e.g.
         * standby) must not keep sounding/buzzing until control returns
         * AND a pad is released. chord_end_all_notes() is a no-op once
         * nothing's sounding, so this is harmless every scan while
         * frozen here, same as seq_end_current_note() above. */
        if (s_active_mode == OP_MODE_CHORD) {
            chord_end_all_notes();
        }
        return;
    }

    handle_triangle_click();
    handle_diamond_click();
    handle_circle_tap(now_ms);
    handle_transport_and_length(now_ms);

    /* Fetched exactly once per scan -- tiles_midi_clock_get_state()
     * consumes start_edge as a side effect (see midi_clock.h's own
     * comment), and both the sequencer's own clock-advance below and the
     * beat-flash computation need to see the SAME snapshot. */
    tiles_midi_clock_state_t clock = tiles_midi_clock_get_state();
    float beat_flash_level = compute_beat_flash_level(now_ms, clock);

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

    if (s_active_mode == OP_MODE_SEQUENCER && s_pattern_menu_visible) {
        handle_pattern_menu_taps();
        render_pattern_menu(now_ms, clock.running);
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER && s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
        handle_edit_mode(now_ms);
        render_edit_mode(now_ms, clock.running);
        return;
    }

    if (s_active_mode == OP_MODE_SEQUENCER) {
        seq_handle_step_taps(now_ms);
        seq_advance_clock(clock);
        render_sequencer(beat_flash_level, clock.running);
    }
    if (s_active_mode == OP_MODE_CHORD) {
        handle_chord_pad_taps();
    }
    /* Guitar mode needs nothing further here -- handle_transport_and_
     * length() above already handles its "-"/"+" fret-shift, and its
     * note-playing/idle-fret-marker rendering both go entirely through
     * services/expression.c's and services/lighting.c's own EXISTING
     * pipelines (now guitar-aware via services/note_map.h), completely
     * unowned by this function -- see set_active_mode()'s own comment on
     * why guitar mode never claims standby_active. Chord mode's melody
     * columns follow that identical pass-through pattern (nothing needed
     * here for them either); only its chord-strip pads need the explicit
     * handle_chord_pad_taps() call above, and even that needs no render
     * call of its own -- services/lighting.c's pad_desired_rgb() already
     * paints the whole strip solid blue by itself once note_map.c reports
     * chord mode active, the same "lighting.c already reads note_map.c's
     * own state" precedent guitar mode's fret markers established. */
}

bool tiles_op_mode_owns_pad_grid(void) {
    return s_menu_visible || s_scale_menu_visible || s_active_mode == OP_MODE_SEQUENCER;
}

/* See this accessor's own declaration in op_mode.h for the full
 * reasoning -- every mode except chord just defers to the blanket
 * accessor above; chord narrows that answer down to its own 8
 * chord-strip pads instead of claiming (or releasing) the whole grid. */
bool tiles_op_mode_owns_pad(uint8_t logical_pad) {
    if (s_active_mode == OP_MODE_CHORD) {
        return tiles_note_map_is_chord_region_pad(logical_pad);
    }
    return tiles_op_mode_owns_pad_grid();
}

/* Broader than tiles_op_mode_owns_pad_grid() above: also true for guitar
 * mode, which needs "-"/"+" ownership (so services/octave_control.c
 * yields its own default octave-shift function -- see that file's own
 * scan-gate) WITHOUT the "suppress new note strikes" side effect real
 * grid ownership carries (services/expression.c checks owns_pad_grid()
 * for exactly that, and guitar mode's whole design depends on real notes
 * still playing normally -- see this file's "Guitar/bass fret mode"
 * section). Sequencer mode is covered either way, since it already
 * legitimately owns the whole grid. */
bool tiles_op_mode_owns_octave_buttons(void) {
    return tiles_op_mode_owns_pad_grid() || s_active_mode == OP_MODE_GUITAR;
}

bool tiles_op_mode_is_sequencer_active(void) {
    return s_active_mode == OP_MODE_SEQUENCER;
}

bool tiles_op_mode_has_menu_open(void) {
    return s_menu_visible || s_scale_menu_visible || s_pattern_menu_visible || s_seq_edit_mode != OP_SEQ_EDIT_NONE;
}
