#include "op_mode.h"

#include "board_layout.h"
#include "board_pins.h"
#include "buttons.h"
#include "debug_mode.h"
#include "expression.h"
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

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "pico/platform/sections.h"
#include "pico/time.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Diamond, freed from every menu-related duty (its per-mode sub-menu
 * role moved to triangle+shift -- see handle_triangle_click()'s own
 * comment) to be a persistent Ableton transport remote instead -- see
 * handle_diamond_transport()'s own comment for the full feature. Real
 * feedback gave this an explicit, four-state LED language (these
 * buttons are monochrome PWM, no color to distinguish states with
 * otherwise): "armed record ... is blink twice and pause then again,
 * ... stopped is off. play is on. record is pulsing in the same
 * fashion as the deep sleep for shift button." */
#define OP_TRANSPORT_LED_STOPPED_LEVEL 0.0f
#define OP_TRANSPORT_LED_PLAYING_LEVEL 1.0f
/* Armed: two quick flashes, then a pause, repeating -- a real-hardware
 * "recording is about to start" convention. Cycle = on, gap, on, pause
 * (120+120+120+500 = 860ms total); unmeasured against real use, worth
 * tuning like every other LED timing in this file. */
#define OP_TRANSPORT_ARMED_BLINK_ON_MS 120u
#define OP_TRANSPORT_ARMED_BLINK_GAP_MS 120u
#define OP_TRANSPORT_ARMED_PAUSE_MS 500u
/* Recording: the exact same sine-breathing shape services/standby.c's
 * own render_deep_sleep_frame() uses for circle's deep-sleep pulse
 * (DEEP_SLEEP_PULSE_PERIOD_MS/_MIN/_MAX there) -- real feedback pointed
 * at that specific animation as the reference, not a new one invented
 * here. Duplicated rather than shared because standby.c's constants are
 * static to that file and this is a different button/context; kept
 * numerically identical on purpose. */
#define OP_TRANSPORT_RECORDING_PULSE_PERIOD_MS 3000.0f
#define OP_TRANSPORT_RECORDING_PULSE_MIN 0.03f
#define OP_TRANSPORT_RECORDING_PULSE_MAX 0.35f
/* Uses this file's own OP_MODE_PI (defined further down, alongside
 * menu_selected_pulse_level() -- the pulse this constant's own
 * breathing shape is deliberately unlike, see that constant's history)
 * -- not services/standby.c's TILES_STANDBY_PI, which is file-local to
 * standby.c and not exported via its header, same numeric value either
 * way. A short-lived separate OP_TRANSPORT_PI duplicate briefly existed
 * here instead of reusing OP_MODE_PI; consolidated back to one. */
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

/* Real feedback: "it should be a pulsing like the deep sleep pulse" --
 * same slow pacing as services/standby.c's own DEEP_SLEEP_PULSE_
 * PERIOD_MS (3000ms), not shared code (same "not shared, just the same
 * convention" precedent as menu_selected_pulse_level() above already
 * follows relative to services/standby.c's animations), min/max raised
 * to a genuinely visible button-LED range instead of that pulse's own
 * barely-there idle-pad brightness. Used for "something's happening in
 * the background," deliberately calmer/slower than menu_selected_
 * pulse_level()'s own faster, brighter "this is active right now."
 * Real feedback: "the pulse for sequencer is running should be a bit
 * faster, how about we make it match the bpm of clock" -- period is
 * now one beat at whatever tempo tiles_midi_clock_get_ms_per_beat()
 * currently reports (real external clock or tap tempo, see that
 * function's own comment) instead of a fixed 3000ms regardless of it;
 * also affects handle_diamond_transport()'s own reuse of this exact
 * function for its "clock running, this lane isn't" state. */
#define OP_BACKGROUND_PULSE_MIN 0.0f
#define OP_BACKGROUND_PULSE_MAX 1.0f

static float background_pattern_pulse_level(uint32_t now_ms) {
    float phase = (float)now_ms / tiles_midi_clock_get_ms_per_beat();
    float raw = 0.5f + 0.5f * sinf(2.0f * OP_MODE_PI * phase);
    return OP_BACKGROUND_PULSE_MIN + (OP_BACKGROUND_PULSE_MAX - OP_BACKGROUND_PULSE_MIN) * raw;
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

/* Real feedback: "we need to make sure it reboots to last state
 * completely includeing sequence, layout, scale, play state." Placed in
 * __uninitialized_ram (survives a watchdog reset, unlike ordinary
 * statics -- see note_map.c's own comment on the same mechanism) along
 * with s_seq_lane_running[]/s_seq_active_alt[] below -- together,
 * "which mode is showing" + "which lanes are playing" + "which pattern
 * slot each lane has selected" is what this feedback means by "layout"
 * and "play state." Deliberately NOT extended to s_transport_playing/
 * s_transport_recording just below (this device's own BELIEF about
 * Ableton's transport state, not this device's own playback) -- MIDI
 * has no way to query Ableton's actual state, and Ableton may well have
 * kept running or been stopped by hand during whatever downtime this
 * crash caused, so restoring a guess here risks the diamond button's
 * next click sending the OPPOSITE of what the host actually needs
 * (Stop when it's already stopped, or vice versa); resetting to "assume
 * stopped" and letting the player's next diamond click resync is the
 * safer default. tiles_op_mode_init() below only skips re-defaulting
 * these three on a confirmed crash-recovery boot -- untouched here,
 * this declaration carries no initializer on purpose, same reason
 * note_map.c's own crash-survived statics don't either. */
static tiles_op_mode_t __uninitialized_ram(s_active_mode);
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
/* True if SW6/circle was ALSO seen held during the current triangle
 * press -- see handle_triangle_click()'s own comment for the "shift"
 * gesture this distinguishes from a plain solo click. Edge-latched
 * (sticky once observed) for the identical reason s_triangle_press_had_
 * conflict is: circle and triangle won't always release in the same
 * tick, so a fresh re-check of circle's state AT release could miss a
 * genuine shift-hold that happened to release circle first. */
static bool s_triangle_press_was_shift;

static bool s_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Per-mode sub-menu (SW3/triangle + SW6/circle "shift") -------------
 * Real feedback: "the triangle is sub menues per each mode so that
 * button toggles its onw menue in each mode. for melodic it toggles
 * different scale modes," later: "lets put the scale menu into the mode
 * menu when triangle plus shift pressed. freeing up diamond from
 * everything for now" -- moved off diamond (which used to own this,
 * see op_mode.h's own swap note for the SW3/SW4 history before THIS
 * move) once diamond was needed as a dedicated Ableton transport remote
 * instead (see handle_diamond_transport()). Only melodic's sub-menu (the
 * scale picker) is built -- other modes don't have one yet, same
 * "selectable but not implemented" spirit as chord/arp themselves;
 * handle_triangle_click()'s shift branch is the extension point once
 * they do. */
static bool s_scale_menu_visible;
static bool s_diamond_was_held;
static bool s_diamond_press_had_conflict; /* see s_triangle_press_had_conflict's own comment -- same reasoning, watches diamond instead of triangle, guards against game_mode.h's 4-button combo */
static bool s_scale_menu_prev_pad_touched[TILES_NUM_PADS];

/* ---- Diamond: Ableton transport remote ---------------------------------
 * See handle_diamond_transport()'s own comment for the full feature.
 * s_diamond_press_start_ms is when the CURRENT diamond press began (for
 * the 2-second arm-hold check); s_diamond_record_armed is edge-latched
 * true once that threshold is crossed, so it only fires once per hold;
 * s_transport_playing/s_transport_recording are this device's own
 * belief about Ableton's transport state (MIDI has no way to query it
 * back, so this is tracked locally and assumed to stay in sync with
 * whatever this device itself last sent) -- recording implies playing,
 * so s_transport_recording true always has s_transport_playing true
 * too, but not the reverse. */
static uint32_t s_diamond_press_start_ms;
static bool s_diamond_record_armed;
static bool s_transport_playing;
static bool s_transport_recording;
/* True if SW6/circle was ALSO seen held during the current diamond
 * press -- real feedback: "capture mode is triggered by diamond in
 * sequencer mode... shift diamond does pattern opicker." In sequencer
 * mode, this is what tells the pattern bank (shift held) apart from
 * capture mode's own plain-click toggle (shift NOT held) -- see
 * handle_diamond_transport()'s own release branch. Same edge-latched-
 * sticky reasoning as s_triangle_press_was_shift. */
static bool s_diamond_press_was_shift;

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

/* ---- Multi-lane pattern bank ---------------------------------------------
 * Real feedback: "sequence selector should have all 24 pads as possible
 * sequences... lets do 4 independent sequences that can be assigned to 4
 * channels selectable by each row of 6 alternatives... by defoult they
 * just do different midi channels." Reworked from the previous round's
 * flat 4-pattern bank (one pattern per row, only one ever playing) into a
 * genuine 4 LANE x 6 ALTERNATIVE grid -- all 24 pads meaningful, matching
 * the grid's own real shape exactly instead of using only 4 of its rows.
 * A LANE is a fully independent, always-running playhead with its own
 * fixed MIDI channel (s_seq_lane_channel[] below) -- all 4 play
 * SIMULTANEOUSLY once the clock is running, never just one "active"
 * pattern at a time the way the previous round worked. Each lane's own
 * row of 6 alternatives is a set of interchangeable patterns for THAT
 * lane -- picking a different column for a row swaps what that ONE lane
 * plays without touching the other 3 lanes' own playback at all. "write
 * down that the control software can send the sequences to differetn out
 * ports loke cv gate, or midi" -- noted for companion-app/README.md's own
 * planned feature list, not built here: this firmware round only ever
 * sends all 4 lanes out the same USB-MIDI endpoint on their own channels,
 * same as everything else in this file. */
#define OP_SEQ_NUM_LANES 4u
#define OP_SEQ_ALTS_PER_LANE 6u
#define OP_SEQ_MIN_LENGTH 1u
#define OP_SEQ_MAX_LENGTH OP_SEQ_NUM_STEPS
/* Real feedback: "when we do shift plus modifiers -+ for changiong
 * length of sequencer mode we should have a flash indicating which
 * length we made the sequence to make that a color sentia magenta."
 * render_sequencer() checks this against s_seq_length_flash_ms every
 * frame it draws (see that function's own flash branch) -- brief enough
 * to read as a confirmation flash, not a lingering mode change. */
#define OP_SEQ_LENGTH_FLASH_DURATION_MS 400u
/* Real feedback: "2 retrigger yess but we need to be able to control that
 * feature." Capped at OP_SEQ_CLOCKS_PER_STEP (6) since a ratchet can't
 * usefully subdivide a step finer than the step's own pulse resolution --
 * 4 leaves each sub-hit at least 1 full clock pulse apart even at the max. */
#define OP_SEQ_MAX_RATCHET 4u

/* Real feedback: "sequencer real time and note select should allow for
 * multiple notes per step so if i play a cluster of notes we should be
 * able to save those in that single step." 2, not something more
 * generous like a full hand-cluster or chord mode's own 4 voices, is a
 * hard flash-capacity ceiling, not a musical judgment call: the whole
 * pattern store (tiles_pattern_store_t -- all 24 patterns across 4
 * lanes x 6 alternatives) has to fit in exactly one 4096-byte flash
 * sector (see TILES_PATTERN_FLASH_OFFSET's own comment for why it's
 * one sector specifically -- the erase+program sequence runs with
 * interrupts disabled and nothing able to pet the watchdog partway
 * through, and a bigger region risks that whole window exceeding the
 * watchdog timeout on a slow flash chip). Even after packing this
 * store's own slot_saved bool[4][6] down to a bitmask specifically to
 * claw back header room (see tiles_pattern_store_t's own comment), 2
 * is the actual largest value that fits -- 3 overflows by 572 bytes,
 * confirmed empirically, not estimated. The _Static_assert right after
 * that struct's definition below exists specifically so this can never
 * silently regress if either constant ever changes again.
 * step_note_count[i] == 0 means "nothing recorded" even if step_armed[i]
 * is true (an armed-but-empty step, reachable via the plain tap-to-arm
 * path below, which arms first and only fills in a note on first pitch
 * assignment) -- seq_fire_note() below treats that case as silent,
 * matching how an unarmed step already behaves, rather than a special
 * case of its own. */
#define OP_SEQ_MAX_NOTES_PER_STEP 2u

typedef struct {
    bool step_armed[OP_SEQ_NUM_STEPS];
    bool step_pitch_override[OP_SEQ_NUM_STEPS];
    /* step_notes[i][0..step_note_count[i]-1] are the notes this step
     * plays, only meaningful if step_pitch_override[i]; see
     * OP_SEQ_MAX_NOTES_PER_STEP's own comment. */
    uint8_t step_notes[OP_SEQ_NUM_STEPS][OP_SEQ_MAX_NOTES_PER_STEP];
    uint8_t step_note_count[OP_SEQ_NUM_STEPS];
    uint8_t step_probability_percent[OP_SEQ_NUM_STEPS]; /* 0-100, default 100 -- only applied if probability_enabled */
    uint8_t step_ratchet_count[OP_SEQ_NUM_STEPS];    /* 1..OP_SEQ_MAX_RATCHET, default 1 (no ratchet), always applied */
    /* Real feedback: "yes per step probablility but we should be able to
     * turn that on and off." Per-pattern master switch -- while false,
     * every step behaves as if its own probability were 100% regardless
     * of what's stored, so a performer can flip a whole pattern back to
     * fully deterministic without having to remember/reset every
     * individually-dialed step. Used to be toggled via a plain circle
     * click while the (now-removed) pattern picker was open -- see this
     * file's own "Pattern/channel picker: REMOVED" section -- currently
     * has no access point at all, so this stays whatever it was last
     * left at (false by default, never set anywhere reachable now). */
    bool probability_enabled;
    uint8_t length;  /* 1..24 active steps -- see OP_SEQ_MIN/MAX_LENGTH */
    /* No `channel` field here anymore -- channel is now a LANE property
     * (s_seq_lane_channel[] below), shared by all 6 of a lane's own
     * alternatives, not something that could ever disagree between them.
     * No `scale` field either, not anymore -- an earlier round gave each
     * pattern its own independently stored scale; real feedback reversed
     * that call: "i need it to be a universal scale for now." Patterns
     * are scale-agnostic now, same as melodic/chord mode -- see note_
     * map.c's own tiles_note_map_quantize_to_scale() and seq_fire_note()'s
     * call into it for how a step's frozen, stored note gets approximated
     * to whatever the ONE global scale currently is, live, at the moment
     * it actually plays, without ever touching this stored data. */
} op_seq_pattern_t;

/* [lane][alternative] -- see this section's own header comment. */
static op_seq_pattern_t s_seq_pattern[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
/* True iff this slot has been explicitly saved to flash at least once
 * -- see this file's own "Pattern persistence" section. Independent of
 * pattern_has_content() (below, in the Pattern bank section): a slot
 * can have real, unsaved content (played since boot, never saved) or
 * be saved-but-since-cleared; this is specifically "does flash
 * currently hold this slot's data," not "does RAM currently hold
 * anything interesting." */
static bool s_pattern_slot_saved[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
/* Which alternative (0..5) each lane is CURRENTLY PLAYING -- read by every
 * lane's own independent seq_advance_clock() call, completely separate
 * from which lane the player happens to be LOOKING at right now
 * (s_seq_edit_lane below). */
/* __uninitialized_ram -- see s_active_mode's own comment above; part of
 * "which pattern slot each lane has selected" surviving a crash-recovery
 * reboot instead of every lane silently snapping back to alt 0. */
static uint8_t __uninitialized_ram(s_seq_active_alt)[OP_SEQ_NUM_LANES];
/* Which lane's pattern the main step view/editor currently shows -- the
 * OTHER 3 lanes keep playing in the background regardless, same
 * established "runs in the background" precedent this file already uses
 * for the sequencer as a whole vs. other top-level modes. active_pattern()
 * below always resolves through this + s_seq_active_alt[this lane]. */
static uint8_t s_seq_edit_lane;
/* Default channel per lane claims from the TOP of the 15 MPE Member
 * Channels downward (lane 0 = nibble 15, exactly today's original
 * single-pattern behavior, unchanged for anyone never touching the bank)
 * -- a default, not a hard reservation against LIVE touch (services/
 * expression.c's own per-touch allocator is untouched and can still use
 * any of these 15 channels when the sequencer isn't genuinely running --
 * see tiles_op_mode_sequencer_channel_is_reserved()'s own comment for
 * when it IS). */
static uint8_t s_seq_lane_channel[OP_SEQ_NUM_LANES];
/* See OP_SEQ_LENGTH_FLASH_DURATION_MS's own comment. 0 at boot/pattern
 * init is indistinguishable from "just flashed at boot time" for a
 * single frame at most -- not worth a separate bool for. */
static uint32_t s_seq_length_flash_ms;

/* ---- Per-lane playback state ---------------------------------------------
 * Every one of these used to be a single scalar, back when only one
 * pattern ever played at a time. Now that all 4 lanes run their own
 * independent playhead simultaneously, each needs its own slot -- indexed
 * by lane (0..OP_SEQ_NUM_LANES-1) everywhere below, never by
 * s_seq_edit_lane implicitly (that's an EDIT/display concern only; see
 * active_pattern() vs. pattern_for_lane() further down). */
/* Real feedback: "play and stop are independent per active pattern. the
 * only thing global is tap tempo or midi tempo." Each lane's own play/
 * stop, toggled by "-"/"+" while that lane is the one being VIEWED (see
 * handle_transport_and_length()'s own sequencer branch) -- switching
 * which lane the bank shows you never touches this for any lane but the
 * one you just switched away from/to. midi_clock.h's own tiles_midi_
 * clock_set_running()/is_running() stays exactly what it already was
 * (ONE shared flag for whether pulse_count is ticking at all, driven by
 * whatever tempo source exists) -- this is a second, per-lane gate
 * layered on top inside seq_advance_clock() below, not a replacement for
 * it. Derived, not independent: tiles_midi_clock_set_running(true) is
 * called the moment any lane here goes from stopped to running (so the
 * shared pulse_count starts advancing if it wasn't already), and
 * set_running(false) only once EVERY lane here has stopped. */
/* __uninitialized_ram -- see s_active_mode's own comment above; part of
 * "play state" surviving a crash-recovery reboot. Restoring this alone
 * isn't enough to make a previously-running lane audible again though --
 * see tiles_op_mode_init()'s own restore-path comment for why it also
 * has to call tiles_midi_clock_set_running(true) directly instead of
 * waiting for the normal stopped->running edge that only fires from a
 * live "-"/"+" press. */
static bool __uninitialized_ram(s_seq_lane_running)[OP_SEQ_NUM_LANES];
static uint8_t s_seq_current_step[OP_SEQ_NUM_LANES]; /* 0..23 */
static bool s_seq_note_sounding[OP_SEQ_NUM_LANES];
static uint8_t s_seq_sounding_pad[OP_SEQ_NUM_LANES]; /* 1..24, valid iff s_seq_note_sounding[lane] */
static uint8_t s_seq_sounding_channel[OP_SEQ_NUM_LANES];
/* This step's whole fired cluster -- see OP_SEQ_MAX_NOTES_PER_STEP's
 * own comment. Valid entries are [0..s_seq_sounding_note_count[lane]-1]. */
static uint8_t s_seq_sounding_notes[OP_SEQ_NUM_LANES][OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_sounding_note_count[OP_SEQ_NUM_LANES];
/* Real feedback: "in regular melodic mode when pattern is still
 * playing haptics react to melodic not to the patterns in the
 * background... only the displayed one has the haptics overide." A
 * reversal of this file's own prior deliberate tradeoff (seq_end_
 * current_note()'s old comment accepted background-lane haptics as
 * "rare, momentary, cosmetic only") -- captured once at fire time
 * (seq_lane_haptics_visible(), below), not re-checked at end time,
 * for the same reason s_seq_sounding_channel/_note are captured
 * rather than re-derived: the active mode or edit lane can change
 * mid-note, and ending a note must always undo exactly what starting
 * it actually did, never what the CURRENT state would now imply. */
static bool s_seq_sounding_haptics[OP_SEQ_NUM_LANES];
static uint32_t s_seq_step_started_at_pulse[OP_SEQ_NUM_LANES];
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
 * double-stop rewind). Per-lane now: all 4 lanes share one global
 * transport (minus/plus start/stop/rewind everything at once, same as
 * before -- see handle_transport_and_length()'s own sequencer branch),
 * but EACH lane still needs its own pending-start/restart bookkeeping
 * since seq_advance_clock() reads/clears these once per lane per scan. */
static bool s_seq_pending_start[OP_SEQ_NUM_LANES];
static bool s_seq_pending_restart[OP_SEQ_NUM_LANES];

/* Ratchet playback state for the CURRENT step only -- reset every time a
 * new step is entered (seq_enter_step()), consumed by seq_advance_clock()
 * firing additional sub-hits within that same step's own pulse window.
 * s_seq_ratchet_remaining counts hits still owed AFTER the one seq_enter_
 * step() already fired directly. Per-lane, same reasoning as the
 * pending-start state just above. */
static uint8_t s_seq_ratchet_remaining[OP_SEQ_NUM_LANES];
static uint32_t s_seq_ratchet_interval_pulses[OP_SEQ_NUM_LANES];
static uint32_t s_seq_next_ratchet_pulse[OP_SEQ_NUM_LANES];

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
 * Triangle+shift cancels out of any of the three with no change
 * (handle_triangle_click()'s own shift branch -- this used to be
 * diamond's job before diamond became a dedicated transport remote).
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

/* ---- Pattern/channel picker: REMOVED -----------------------------------
 * Real feedback: "make sure the shift scasle works on chord melodic mode
 * and on sequewndcer as well measning remove whatever aux menu we had in
 * sequencer mode." Triangle+shift now universally opens the scale picker
 * in every mode that has one (melodic, chord, sequencer -- see
 * handle_triangle_click()'s own shift branch), no per-mode branching to a
 * different sub-menu anymore. The underlying multi-pattern DATA MODEL
 * (s_seq_pattern[OP_SEQ_NUM_PATTERNS], active_pattern(), per-pattern MIDI
 * channel) is deliberately left in place, not reverted -- only its
 * switching UI is gone, so s_seq_active_pattern now stays permanently 0
 * (pattern 0's own channel, at the TOP of the 15 MPE Member Channels,
 * per this file's own "Multi-pattern bank" section) until/unless a
 * future round gives pattern-switching a new access point. Also removed
 * with it: the plain-circle-click-while-the-picker-was-open gesture that
 * toggled a pattern's probability_enabled master switch (see this file's
 * git history for handle_circle_tap()'s own removed branch) -- that
 * setting has no other access point right now and is effectively inert
 * until pattern-switching (or some replacement UI for it) comes back. */

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
static bool s_chord_pad_touched[TILES_NUM_PADS];
static bool s_chord_pad_sounding[TILES_NUM_PADS];

/* ---- Chord voicing: fixed triad + bass, velocity from strike speed -----
 * Real feedback, chord mode heard on real hardware, across several
 * rounds: an ADAPTIVE voicing drifted ("the chord shapes evolve... it
 * transports the chords to different parts of the range"), replaced
 * with a STATIC, pressure-TIERED design instead -- a light tap gave a
 * plain triad, pressing past halfway escalated live to a full rootless
 * jazz voicing, morphing both ways within the same held note. Once
 * that could actually be tried: "the tap and then complex chord is not
 * working nice so lets simplify to velocity sensitive chords with bass
 * note not dual type of chord or light tap to chord." Collapsed to ONE
 * consistent voicing per pad -- always the plain triad+bass shape the
 * old tier system's own "tap" case used, the jazz escalation removed
 * outright, not tuned -- with STRIKE VELOCITY (how fast the touch
 * reached a real press -- standard "speed of travel" velocity-
 * sensitive-keybed convention, the exact same already-tuned curve
 * services/expression.c's own melodic notes use, see tiles_expression_
 * velocity_from_strike()) now doing what depth used to: never which
 * notes play, only how loud. Quality (major/minor/diminished) is still
 * read directly off the actual root-to-third/root-to-fifth intervals
 * tiles_note_map_get_chord_notes() returns, not hardcoded per scale
 * degree, so it automatically tracks whichever diatonic mode is
 * selected -- unchanged from before, it just no longer changes which
 * voices are present, only open_third's own interval. */

/* Root note pushed one extra octave down from the chord register for
 * the dedicated bass voice, on top of note_map.c's own CHORD_OCTAVE_
 * DOWN_SEMITONES -- real feedback: "octave lower bass note." */
#define OP_CHORD_BASS_EXTRA_OCTAVE_SEMITONES 12
#define OP_CHORD_NUM_VOICES 4u /* bass + root + fifth + open_third, always */

static uint8_t s_chord_pad_notes[TILES_NUM_PADS][OP_CHORD_NUM_VOICES];
/* Strike-velocity tracking, one independent copy per chord pad --
 * mirrors services/expression.c's own PAD_STATE_AWAITING_STRIKE
 * bookkeeping (touch-start timestamp, peak depth reached since then)
 * closely enough to feed its already-tuned velocity curve -- see
 * handle_chord_pad_taps()'s own comment for the two ways this
 * deliberately simplifies that file's fuller state machine. */
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

/* Always the same 4-voice shape now -- see this section's own header
 * comment. */
static void build_chord_voicing(const uint8_t raw[TILES_NOTE_MAP_CHORD_NUM_NOTES],
                                 uint8_t out_notes[OP_CHORD_NUM_VOICES]) {
    uint8_t root = raw[0];
    uint8_t third = raw[1];
    uint8_t fifth = raw[2];

    uint8_t bass = clamp_midi_note((int)root - OP_CHORD_BASS_EXTRA_OCTAVE_SEMITONES);
    uint8_t open_third = clamp_midi_note((int)third + 12);

    out_notes[0] = bass;
    out_notes[1] = root;
    out_notes[2] = fifth;
    out_notes[3] = open_third;
}

static void chord_pad_note_off(uint8_t pad) {
    if (!s_chord_pad_sounding[pad - 1u]) {
        return;
    }
    for (uint8_t i = 0; i < OP_CHORD_NUM_VOICES; i++) {
        tiles_midi_note_off(OP_CHORD_CHANNEL, s_chord_pad_notes[pad - 1u][i]);
    }
    tiles_haptics_stop(pad);
    s_chord_pad_sounding[pad - 1u] = false;
}

/* Strikes `pad` fresh at `velocity` -- called once a strike has
 * actually been measured (see handle_chord_pad_taps() below). */
static void chord_pad_strike(uint8_t pad, uint8_t velocity) {
    chord_pad_note_off(pad);
    uint8_t raw[TILES_NOTE_MAP_CHORD_NUM_NOTES];
    tiles_note_map_get_chord_notes(pad, raw);
    build_chord_voicing(raw, s_chord_pad_notes[pad - 1u]);
    for (uint8_t i = 0; i < OP_CHORD_NUM_VOICES; i++) {
        tiles_midi_note_on(OP_CHORD_CHANNEL, s_chord_pad_notes[pad - 1u][i], velocity);
    }
    tiles_haptics_trigger_kick(pad, velocity);
    s_chord_pad_sounding[pad - 1u] = true;
}

/* Releases every currently-sounding chord pad -- called whenever chord
 * mode stops being the active mode (set_active_mode() below), the same
 * "clean up whatever's sounding the instant a mode hands off control"
 * rule this file's own seq_end_current_note()/set_active_mode() pairing
 * already established for the sequencer. Also clears any in-flight
 * strike measurement -- a touch that was mid-way to becoming a chord
 * when the mode changed shouldn't retroactively fire once it's gone. */
static void chord_end_all_notes(void) {
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        chord_pad_note_off(pad);
        s_chord_pad_awaiting_strike[pad - 1u] = false;
    }
}

/* Real feedback: "lets simplify to velocity sensitive chords with bass
 * note." Mirrors services/expression.c's own strike-detection shape
 * (touch-start timestamp, peak Hall depth tracked every scan until it
 * first crosses TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA) closely
 * enough to feed that file's already-tuned tiles_expression_velocity_
 * from_strike() curve, deliberately simplified two ways: no extra
 * VELOCITY_FOLLOWTHROUGH_MS wait after crossing (real Hall depth under
 * an actual finger press crosses a meaningful threshold within single-
 * digit milliseconds of first contact, well under perceptible latency,
 * so skipping that refinement keeps chords feeling as immediate as the
 * old always-fires-on-touch-down design while still measuring a real
 * strike speed instead of guessing one), and no release-triggered
 * fallback commit for a touch that never crosses the threshold (it
 * just stays silent -- simpler than services/expression.c's own rule,
 * an accepted difference for a chord pad rather than a single note). */
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

/* The pattern the player is currently LOOKING AT/editing -- the main step
 * view, per-step pitch/probability/ratchet edit, the pattern bank's own
 * selection, capture mode, and length-adjust all read/write through this
 * one. Deliberately NOT what plays each lane's own audio -- see
 * pattern_for_lane() below for that -- so switching which lane you're
 * viewing never has to fight over which pattern struct playback itself is
 * independently reading. */
static op_seq_pattern_t *active_pattern(void) {
    return &s_seq_pattern[s_seq_edit_lane][s_seq_active_alt[s_seq_edit_lane]];
}

/* The pattern actually playing on a given lane right now -- used only by
 * the playback engine below (seq_fire_note()/seq_enter_step()/
 * seq_advance_clock() and friends), each call parameterized by lane since
 * all OP_SEQ_NUM_LANES now run independently and simultaneously. When
 * `lane == s_seq_edit_lane` this happens to be the exact same pattern
 * active_pattern() above also resolves to -- no special-casing needed for
 * that overlap, both just read the same s_seq_active_alt[lane]. */
static op_seq_pattern_t *pattern_for_lane(uint8_t lane) {
    return &s_seq_pattern[lane][s_seq_active_alt[lane]];
}

/* Real feedback: "if the sequenfcer is reduced to 16 steps then auto
 * align the layout of the steps to the left meaning a 4x4 grid[,]
 * anything else still ads or reduces steps in the curent full layout.
 * thats a signle snap layout change." A special case ONLY at exactly
 * 16 -- every other length keeps the plain linear pad==step+1 mapping,
 * using however many of the 6 columns per row that length happens to
 * fill (a partial last row when it doesn't divide evenly by 6, exactly
 * as it always has). At 16 specifically, the plain mapping leaves row
 * 3 two-thirds full and row 4 entirely dark -- an awkward shape for a
 * genuinely common step count real hardware sequencers usually give a
 * clean square-ish grid instead. These two functions are the ONLY
 * place that decides this -- every touch-handling/rendering site that
 * needs to go from a step to the pad that shows it, or a touched pad
 * to the step it represents, goes through one of them instead of
 * computing pad<->step directly, so the special case can never drift
 * out of sync between what lights up and what a touch actually arms.
 * Take the pattern explicitly rather than reading active_pattern()
 * internally -- seq_fire_note() below needs pattern_for_lane(lane) for
 * whichever lane is actually firing (any of the 4, not necessarily
 * s_seq_edit_lane), and that lane's own length is what decides ITS OWN
 * layout; every other caller in this file only ever cares about the
 * viewed lane and just passes active_pattern() straight through. */
static bool seq_uses_4x4_layout(const op_seq_pattern_t *pat) {
    return pat->length == 16u;
}

/* Which pad (1..24) renders/arms `step` right now, for `pat`'s own
 * layout. */
static uint8_t seq_pad_for_step(const op_seq_pattern_t *pat, uint8_t step) {
    /* step < 16u guard matters: a 4x4 layout only exists because length
     * == 16, so step should never legitimately reach 16-23 while this
     * is true, but a generic "walk every step 0..23" loop (edit_exit()'s
     * own touch re-sync, for one) does so anyway, defensively, for
     * steps that don't actually matter right now. Without this guard,
     * row/col above would compute row 5, which board_pad_for_row_col()
     * has no bounds checking against at all -- a real out-of-range pad
     * number (25+) silently handed to a caller that trusts 1..24. */
    if (seq_uses_4x4_layout(pat) && step < 16u) {
        uint8_t row = (uint8_t)(step / 4u);
        uint8_t col = (uint8_t)(step % 4u);
        return board_pad_for_row_col((uint8_t)(row + 1u), (uint8_t)(col + 1u));
    }
    return (uint8_t)(step + 1u);
}

/* Which step `pad` represents right now, if any, for `pat`'s own
 * layout -- false (out_step untouched) for a pad that isn't part of
 * the grid at all, which can only happen in the 4x4 layout: the
 * rightmost 2 columns of every row, deliberately left dark rather than
 * mapped to steps 16-23 that wouldn't exist in a genuinely 16-long
 * pattern anyway. Every pad is always part of the grid otherwise, even
 * ones beyond the active length -- those still resolve to a real step
 * index, just one render_sequencer() shows as dark/"out of the loop"
 * same as always. */
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

static void edit_enter(uint8_t step, uint32_t started_ms); /* defined below, used by seq_handle_step_taps()'s own hold detection */
static void edit_enter_ratchet(uint8_t step); /* defined below, used by seq_handle_step_taps()'s own circle+touch detection */

/* True iff `lane`'s own playback is the one thing actually shown on
 * the grid right now -- sequencer mode is the active mode AND this is
 * the lane currently being viewed/edited. See s_seq_sounding_haptics'
 * own comment above for why this matters and why it's captured once
 * rather than re-checked. */
static bool seq_lane_haptics_visible(uint8_t lane) {
    return s_active_mode == OP_MODE_SEQUENCER && lane == s_seq_edit_lane;
}

/* Uses the channel/note captured at note-on time (below), not whatever
 * pattern_for_lane(lane) currently resolves to -- correctness never
 * depends on that lane's s_seq_active_alt staying the same between a note
 * firing and this ending it (the pattern bank can switch it mid-note --
 * see handle_pattern_bank_taps()'s own comment -- and this stays correct
 * either way). */
static void seq_end_current_note(uint8_t lane) {
    if (!s_seq_note_sounding[lane]) {
        return;
    }
    /* Every note this step's cluster fired gets its own Note-Off --
     * see s_seq_sounding_notes[]'s own declaration comment. */
    for (uint8_t i = 0; i < s_seq_sounding_note_count[lane]; i++) {
        tiles_midi_note_off(s_seq_sounding_channel[lane], s_seq_sounding_notes[lane][i]);
    }
    /* Only undoes the haptic if this note actually triggered one --
     * see s_seq_sounding_haptics' own comment on why that's the fact
     * captured at fire time, not a live re-check. The MIDI note-off
     * just above always fires regardless -- background lanes keep
     * sounding normally, only their HAPTIC feedback is suppressed. One
     * kick/stop per STEP, not per note in its cluster -- a chord is
     * still one physical strike. */
    if (s_seq_sounding_haptics[lane]) {
        tiles_haptics_stop(s_seq_sounding_pad[lane]);
    }
    s_seq_note_sounding[lane] = false;
    s_seq_sounding_note_count[lane] = 0u;
}

/* Fires ONE note for `step` on `lane` -- shared by seq_enter_step() (the
 * step's first hit) and seq_advance_clock() (any additional ratchet
 * sub-hits within that same step) so both go through identical logic.
 * Does NOT touch s_seq_current_step[lane] or roll probability -- those are
 * seq_enter_step()'s own concerns, once per step, not per ratchet hit. */
static void seq_fire_note(uint8_t lane, uint8_t step) {
    seq_end_current_note(lane);
    op_seq_pattern_t *pat = pattern_for_lane(lane);
    /* seq_pad_for_step(pat, ...), this lane's OWN pattern -- not
     * active_pattern()/the viewed lane's. Matters for two things below:
     * the live note-resolve fallback (so an un-overridden step's default
     * pitch matches wherever it visually sits in THIS lane's own 16-step
     * 4x4 layout, if that's what this lane happens to be using, exactly
     * the same "arm a fresh step, get the pitch of the pad you touched"
     * rule seq_handle_step_taps() already follows) and the haptic-kick
     * pad below -- harmless either way for a background lane (seq_lane_
     * haptics_visible() only ever returns true for s_seq_edit_lane, so
     * this exact pad number is never actually felt for any other one),
     * but correct regardless of which lane this happens to be. */
    uint8_t pad = seq_pad_for_step(pat, step);
    /* Real feedback: "not universaly in a way that alters the underlying
     * pattern but it alters the real time playing pattern. so no
     * rewriting lyust aproximating to the locked scale selected." A
     * step's OWN frozen pitch (step_note[], set once at arm/assign time --
     * see seq_handle_step_taps()'s own comment on why it's frozen rather
     * than a live lookup) is quantized to whatever the universal scale
     * currently is EVERY time it actually plays, here, rather than ever
     * being rewritten in storage -- change the scale, change how the
     * pattern sounds, instantly and reversibly, with the underlying data
     * exactly as programmed the whole time. The NOT-overridden branch
     * (tiles_note_map_get_note()) needs no such treatment -- it already
     * resolves live against the current scale on every call, never frozen
     * in the first place. */
    uint8_t channel = s_seq_lane_channel[lane];
    /* Real feedback: "sequencer real time and note select should allow
     * for multiple notes per step." Every note in this step's cluster
     * fires together -- an unarmed/not-yet-overridden step still falls
     * back to a single live-resolved note from its own pad, same as
     * before (there's no "cluster" to speak of until one's actually
     * been recorded). count capped defensively against a corrupt/
     * future-format value rather than trusting stored flash data blindly
     * for a loop bound. */
    uint8_t count;
    if (pat->step_pitch_override[step]) {
        count = pat->step_note_count[step];
        if (count > OP_SEQ_MAX_NOTES_PER_STEP) {
            count = OP_SEQ_MAX_NOTES_PER_STEP;
        }
        for (uint8_t i = 0; i < count; i++) {
            uint8_t note = tiles_note_map_quantize_to_scale(pat->step_notes[step][i]);
            tiles_midi_note_on(channel, note, OP_SEQ_VELOCITY);
            s_seq_sounding_notes[lane][i] = note;
        }
    } else {
        uint8_t note = tiles_note_map_get_note(pad);
        tiles_midi_note_on(channel, note, OP_SEQ_VELOCITY);
        s_seq_sounding_notes[lane][0] = note;
        count = 1u;
    }
    /* Only this lane's own haptic feedback is gated -- the MIDI notes
     * above always fire regardless, so every enabled lane keeps
     * sounding exactly as before. See s_seq_sounding_haptics' own
     * comment. One kick, not one per note in the cluster -- a chord is
     * still one physical strike. */
    s_seq_sounding_haptics[lane] = seq_lane_haptics_visible(lane);
    if (s_seq_sounding_haptics[lane]) {
        tiles_haptics_trigger_kick(pad, OP_SEQ_VELOCITY);
    }
    s_seq_note_sounding[lane] = true;
    s_seq_sounding_pad[lane] = pad;
    s_seq_sounding_channel[lane] = channel;
    s_seq_sounding_note_count[lane] = count;
}

/* Real feedback: "yes per step probablility but we should be able to
 * turn that on and off, 2 retrigger yess but we need to be able to
 * control that feature." Rolls this step's probability (once per step
 * occurrence, not per ratchet hit -- a whole step either fires or it
 * doesn't, matching how real hardware "trig probability" works) and, if
 * it fires, arms however many additional ratchet hits it's set for --
 * seq_advance_clock() below fires those on schedule via seq_fire_note(). */
static void seq_enter_step(uint8_t lane, uint8_t step) {
    seq_end_current_note(lane);
    s_seq_current_step[lane] = step;
    s_seq_ratchet_remaining[lane] = 0u;
    op_seq_pattern_t *pat = pattern_for_lane(lane);
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

/* The other half of s_seq_pending_restart's distinction -- resumes
 * whatever step the playhead was already parked on (re-firing it fresh,
 * including a new probability roll/ratchet cycle, since from the
 * player's perspective this IS a new occurrence of that step) instead of
 * jumping back to step 0. */
static void seq_resume_current_step(uint8_t lane, uint32_t now_pulse) {
    s_seq_step_started_at_pulse[lane] = now_pulse;
    seq_enter_step(lane, s_seq_current_step[lane]);
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
    /* Real feedback: "sequencer should not stop if mode is changed. it
     * should be able to run in the background." seq_advance_clock() now
     * runs every scan regardless of s_active_mode (see tiles_op_mode_
     * scan()'s own comment), so re-entering sequencer mode no longer
     * means "the sequencer was paused the whole time I was away" -- it
     * may already be genuinely running. Forcing the transport reset
     * below unconditionally, like this function always used to, would
     * audibly restart the pattern from step 0 every time the player
     * just glances back at sequencer mode to check on it, exactly the
     * "stop if mode is changed" symptom this fix removes elsewhere --
     * skip the transport reset entirely when it's already running, only
     * touching the view-level state that's actually about THIS mode
     * becoming visible again, not about any lane's own playback.
     * Checks s_seq_lane_running[s_seq_edit_lane] specifically now, not
     * the shared clock -- real feedback: "play and stop are independent
     * per active pattern." The lane you're about to look at might be
     * stopped while a DIFFERENT lane keeps the shared clock ticking in
     * the background; resetting THIS one's view is harmless exactly when
     * it's the one actually stopped, regardless of what any other lane
     * is doing. */
    if (!s_seq_lane_running[s_seq_edit_lane]) {
        s_seq_current_step[s_seq_edit_lane] = 0u;
        s_seq_note_sounding[s_seq_edit_lane] = false;
        s_seq_step_started_at_pulse[s_seq_edit_lane] = 0u;
        s_seq_pending_start[s_seq_edit_lane] = true;
        s_seq_pending_restart[s_seq_edit_lane] = true; /* fresh entry always starts from step 0 */
    }
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    /* seq_pad_for_step(), not a bare i+1u -- see edit_exit()'s own
     * comment on why these two arrays are step-indexed. */
    op_seq_pattern_t *layout_pat = active_pattern();
    for (uint8_t step = 0; step < OP_SEQ_NUM_STEPS; step++) {
        s_seq_prev_pad_touched[step] = tiles_touch_is_touched(seq_pad_for_step(layout_pat, step));
        s_seq_step_touch_started_ms[step] = 0u;
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
        uint8_t step;
        /* Real feedback: "if the sequenfcer is reduced to 16 steps then
         * auto align the layout... 4x4 grid" -- see seq_step_for_pad()'s
         * own comment. A pad outside the active grid in that layout
         * (the rightmost 2 columns of every row) simply isn't part of
         * the step grid at all right now, same as it visually shows
         * dark -- skipped here rather than tracked/armed. */
        if (!seq_step_for_pad(pat, pad, &step)) {
            continue;
        }
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
                /* Real feedback: "when steps are turned off they are not
                 * saving the assigned pitch. they should always save the
                 * pitch they last had when on in case of retrigger."
                 * step_pitch_override[]/step_note[] were never actually
                 * cleared when a step turned off -- that data already
                 * survived untouched -- but re-arming unconditionally
                 * overwrote it below regardless, discarding whatever
                 * pitch this step remembered from before the instant you
                 * turned it back on. Only resolve+freeze a fresh pitch
                 * the first time a step is EVER armed (no override yet
                 * at all); a step that already has one keeps it exactly
                 * as it was, however many times it gets toggled off and
                 * back on. The pitch-edit view (hold a step) remains the
                 * deliberate way to actually change an already-frozen
                 * step's pitch. */
                if (pat->step_armed[step] && !pat->step_pitch_override[step]) {
                    /* Real feedback: "changing scale on a melodic modes
                     * or other sequences should not affect other
                     * sequences that are already set up or playing
                     * meaning fully scale independent sequences." A
                     * plain tap-to-arm used to leave step_pitch_override
                     * false, meaning seq_fire_note() re-resolved this
                     * step's pitch from tiles_note_map_get_note() -- the
                     * LIVE global scale/octave/key -- every single time
                     * it played, so changing the scale anywhere later
                     * (melodic mode, a different pattern's own scale
                     * pick) silently retuned every already-programmed
                     * step everywhere. Freezing the resolved note here,
                     * the instant a step is armed, and always setting
                     * step_pitch_override true, closes that gap the
                     * exact same way seq_capture_mode already does per
                     * step (see seq_capture_advance_clock()'s own
                     * commit) -- every armed step is now permanently
                     * absolute from the moment it's armed, never
                     * re-reading the live scale again. Re-arming a step
                     * later re-freezes it fresh at THAT moment's scale,
                     * which is correct: that's a deliberate new edit,
                     * not a passive drift. */
                    /* Single note -- a plain tap-to-arm is a quick
                     * toggle, not the "hold a cluster" gesture that
                     * actually builds a multi-note chord (see
                     * handle_edit_mode()'s own pitch-edit rewrite and
                     * seq_capture_handle_taps() for the two gestures
                     * that do). */
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

/* Takes the clock snapshot as a parameter rather than calling
 * tiles_midi_clock_get_state() itself -- that function consumes
 * (clears) start_edge as a side effect, and tiles_op_mode_scan() also
 * needs the same snapshot for the beat flash (see this file's own
 * "Master tap tempo" section) -- fetching it twice in one scan would
 * silently drop a real start_edge on whichever call ran second. Called
 * once per LANE, same snapshot every time (see tiles_op_mode_scan()'s own
 * loop) -- all OP_SEQ_NUM_LANES share one global transport (start/stop/
 * tempo), each just tracks its own phase against it independently. */
static void seq_advance_clock(uint8_t lane, tiles_midi_clock_state_t clock) {
    /* Real feedback: "play and stop are independent per active pattern."
     * Checked FIRST, ahead of even start_edge -- a stopped lane must
     * never fire its step-0 note just because some OTHER lane's start (or
     * a fresh external MIDI Start) happens to land while this one is
     * sitting stopped. When this lane is LATER started via "+" (see
     * handle_transport_and_length()'s own sequencer branch), that already
     * arms s_seq_pending_start[lane] itself -- the pending-start handling
     * further down picks it up cleanly at that point, so nothing here
     * needs to pre-sync a stopped lane's position in the meantime. */
    if (!s_seq_lane_running[lane]) {
        seq_end_current_note(lane);
        return;
    }

    if (clock.start_edge) {
        seq_reset(lane, clock.pulse_count);
        return;
    }

    if (!clock.running) {
        /* The shared tempo itself stopped (e.g. a real external Stop) --
         * silences this lane too even though ITS OWN s_seq_lane_running
         * flag was never explicitly toggled off, and leaves that flag
         * alone: the moment the tempo resumes, this lane picks back up
         * on its own, matching how a real hardware sequencer slaved to
         * an external clock keeps remembering which tracks were enabled
         * through a pause. */
        seq_end_current_note(lane);
        return;
    }

    if (s_seq_pending_start[lane]) {
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
         * (resume exactly where a plain stop left it, no reset).
         * Real feedback: "quantize is off, its always waiting for the
         * next beat, it should measure if it can snap to the last beat
         * as well so its accuarte similar to how other devices do it."
         * Used to require phase_in_beat == 0 exactly -- landing even one
         * pulse past a boundary meant waiting nearly a full beat for the
         * next one. Same nearest-boundary measurement seq_capture_
         * handle_taps() already uses for which STEP a captured note
         * targets (see that function's own "Nearest-step quantization"
         * comment) applied here to which BEAT a pending start resolves
         * against: past the halfway point of the current beat, keep
         * waiting for the next one same as before; within the first
         * half, close enough to the one that just passed to snap to it
         * and start now instead of sitting through most of a beat of
         * dead air first. */
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

    /* Real feedback: "2 retrigger yess but we need to be able to control
     * that feature." Fires any ratchet sub-hits due WITHIN the current
     * step BEFORE checking for a step boundary below, so a hit scheduled
     * right at the edge of the step never gets skipped. Guarded, not an
     * unbounded while(), matching this function's own "handle more than
     * one due" pattern elsewhere. */
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
    /* Handles more than one step's worth of pulses having accumulated
     * between scans (robust regardless of main-loop iteration rate vs
     * incoming clock rate), not just the common one-step case. */
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
            /* Real feedback: "if the sequenfcer is reduced to 16 steps
             * then auto align the layout... 4x4 grid" -- see seq_step_
             * for_pad()'s own comment. The rightmost 2 columns of every
             * row simply aren't part of the grid at all in that layout,
             * dark same as a step beyond the active length always was. */
            if (!seq_step_for_pad(pat, pad, &step)) {
                tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
                continue;
            }
            bool is_current = (step == s_seq_current_step[s_seq_edit_lane]);
            if (length_flashing) {
                /* See OP_SEQ_LENGTH_FLASH_DURATION_MS's own comment --
                 * briefly replaces the normal step coloring entirely so
                 * the new length reads clearly, not blended with
                 * whatever armed/cursor state those same pads also
                 * happen to have right now. */
                if (step < pat->length) {
                    tiles_lighting_set_standby_pad_rgb(pad, OP_MENU_MELODIC_R, OP_MENU_MELODIC_G, OP_MENU_MELODIC_B);
                } else {
                    tiles_lighting_set_standby_pad_rgb(pad, 0.0f, 0.0f, 0.0f);
                }
                continue;
            }
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
    /* Diamond isn't touched by this standby-LED loop at all anymore (a
     * no-op regardless, since s_standby_active is false by the time this
     * normal-play render runs) -- its LED is now handle_diamond_
     * transport()'s own persistent override, showing Ableton transport
     * state continuously rather than anything menu-related. Historical:
     * real feedback "the led for modes should light up on menu on not
     * alwayus" once applied to diamond back when it glowed continuously
     * for the whole time any non-melodic mode was active; that's now
     * triangle's own OP_TRIANGLE_LED_MENU_LEVEL, lit only while the
     * top-level mode picker is actually open. */
    for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
        float level = 0.0f;
        if (col == TILES_CIRCLE_BUTTON_COL) {
            /* Real feedback: "flash that light as the tempo." */
            level = beat_flash_level;
        } else if (col == TILES_MINUS_BUTTON_COL || col == TILES_PLUS_BUTTON_COL) {
            /* Real feedback: "the +- transport controls shoudl be the
             * ones with the flashing logic while in sequencer mode" --
             * moved off diamond (which reverted to a plain capture-
             * pulse-or-dark indicator now that it's busy triggering
             * capture/pattern-bank access -- see handle_diamond_
             * transport()'s own render section) onto here instead. "+"
             * keeps its established "lit means running" role: solid
             * while this lane is actually playing, pulsing while the
             * shared clock is going but this lane hasn't joined in,
             * off otherwise. "-"'s own split was corrected by later real
             * feedback: "when the sequence is stopped but not brought
             * back to the start make the - pulse" -- the first "-" press
             * pauses in place (s_seq_lane_running false, s_seq_current_
             * step untouched), and only a SECOND press while already
             * stopped rewinds it to 0 (see handle_transport_and_length()'s
             * own comment on that double-stop gesture) -- so the correct
             * solid/pulse split is the STEP POSITION while stopped, not
             * lane_running (which is already false in both cases and
             * can't tell them apart). Off entirely while the lane is
             * running -- "-" has nothing to show then. */
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
    seq_end_current_note(s_seq_edit_lane);
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
    seq_end_current_note(s_seq_edit_lane);
    s_seq_edit_mode = OP_SEQ_EDIT_RATCHET;
    s_seq_edit_step = step;
}

static void edit_exit(void) {
    s_seq_edit_mode = OP_SEQ_EDIT_NONE;
    /* Re-syncs seq_handle_step_taps()'s own touch tracking so a pad
     * that's still touched the instant control hands back doesn't misread
     * as a fresh arm-toggle -- same pattern this file already uses
     * whenever a sub-view that owned the grid closes.
     * s_seq_prev_pad_touched[]/s_seq_step_touch_started_ms[] are indexed
     * by STEP, not raw pad number (see seq_handle_step_taps()'s own use)
     * -- seq_pad_for_step(), not a bare i+1u, so this stays correct in
     * the 16-step 4x4 layout too, where they're no longer the same
     * value. */
    op_seq_pattern_t *layout_pat = active_pattern();
    for (uint8_t step = 0; step < OP_SEQ_NUM_STEPS; step++) {
        s_seq_prev_pad_touched[step] = tiles_touch_is_touched(seq_pad_for_step(layout_pat, step));
        s_seq_step_touch_started_ms[step] = 0u;
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
 * pickers use. Triangle+shift is the escape hatch for "back out with no
 * change at all" (handle_triangle_click()'s own shift branch -- this
 * used to be diamond's job before diamond became a dedicated transport
 * remote).
 * Probability/ratchet: the OPPOSITE shape, a live DIAL -- Hall depth of
 * the SAME held pad maps continuously to the value while still held
 * (see probability_percent_from_depth()/ratchet_count_from_depth()
 * above), and releasing simply leaves whatever the last-read value was.
 * This needs the one pad to stay down the whole time, unlike pitch --
 * but unlike pitch, there's no second pad to also reach for, so it never
 * has pitch's original "two fingers, one of them pinned down" problem. */
static void handle_edit_mode(uint32_t now_ms) {
    /* seq_pad_for_step(), not a raw +1u -- see that function's own
     * comment on the 16-step 4x4 layout; must resolve to the SAME
     * physical pad the step actually renders/armed at right now, or
     * touch release detection below would watch the wrong sensor. */
    uint8_t edit_pad = seq_pad_for_step(active_pattern(), s_seq_edit_step);
    bool edit_pad_touched = tiles_touch_is_touched(edit_pad);

    if (s_seq_edit_mode == OP_SEQ_EDIT_PITCH) {
        if (edit_pad_touched && (now_ms - s_seq_edit_started_ms) >= OP_SEQ_PROBABILITY_HOLD_MS) {
            /* Real feedback: "yes per step probablility... 2 retrigger
             * yess but we need to be able to control that feature" --
             * escalates exactly like services/standby.h's own circle-hold
             * (4s screensaver -> 8s deep sleep) rather than a new gesture. */
            s_seq_edit_mode = OP_SEQ_EDIT_PROBABILITY;
            /* Real bug found from real feedback: "the chance porcentage
             * when holding a step is not functioning properly its not
             * adctually doing the chance." Root cause: seq_enter_step()
             * only ever rolls probability when active_pattern()->
             * probability_enabled is true (see that function's own
             * check), but the ONLY thing that used to set it true was a
             * circle-click on the now-removed pattern/channel picker --
             * see this file's own "Pattern/channel picker: REMOVED"
             * section -- so it has stayed permanently false, for every
             * pattern, since that picker's removal: every dialed-in
             * percentage below was being faithfully stored and rendered
             * but never actually consulted at playback time. Entering
             * this edit view is itself a deliberate, unambiguous "I want
             * this step's chance to matter" signal, so it's the natural
             * place to also turn the master switch on -- no separate
             * access point needed. Never turned back off automatically;
             * matches this struct's own "master switch a performer can
             * flip a whole pattern back to fully deterministic" framing
             * closer than an implicit auto-disable would. */
            active_pattern()->probability_enabled = true;
            return;
        }
        for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
            bool touched = tiles_touch_is_touched(pad);
            bool was_touched = s_pitch_edit_prev_pad_touched[pad - 1u];
            if (touched && !was_touched) {
                op_seq_pattern_t *pat = active_pattern();
                if (pad == edit_pad) {
                    /* Real feedback: "sequencer real time and note
                     * select should allow for multiple notes per step."
                     * Tapping any OTHER pad first (below) now ADDS to --
                     * or, if already present, removes from -- a growing
                     * cluster instead of committing and closing
                     * immediately the way a single-note pick used to;
                     * tapping THIS step's own pad is what actually
                     * closes now. Repurposes exactly the gesture this
                     * function's own header comment already anticipated
                     * as a harmless no-op ("tapping your own step again
                     * re-commits it to its own current note") into the
                     * explicit close a multi-note cluster genuinely
                     * needs -- a hold-to-close gesture was already
                     * explicitly rejected: "it should be a toggle to set
                     * pitch... not a momentary thing." A step nothing
                     * was ever added to still needs SOME note on close,
                     * so this falls back to its own live-resolved note,
                     * same as an unarmed step already would. */
                    if (pat->step_note_count[s_seq_edit_step] == 0u) {
                        pat->step_notes[s_seq_edit_step][0] = tiles_note_map_get_note(edit_pad);
                        pat->step_note_count[s_seq_edit_step] = 1u;
                    }
                    pat->step_pitch_override[s_seq_edit_step] = true;
                    tiles_haptics_trigger_touch_pulse(pad);
                    edit_exit();
                    return; /* grid ownership just changed under this loop -- stop iterating it */
                }
                uint8_t note = tiles_note_map_get_note(pad);
                uint8_t count = pat->step_note_count[s_seq_edit_step];
                bool removed = false;
                for (uint8_t i = 0; i < count; i++) {
                    if (pat->step_notes[s_seq_edit_step][i] == note) {
                        for (uint8_t j = i; (uint8_t)(j + 1u) < count; j++) {
                            pat->step_notes[s_seq_edit_step][j] = pat->step_notes[s_seq_edit_step][j + 1u];
                        }
                        pat->step_note_count[s_seq_edit_step] = (uint8_t)(count - 1u);
                        removed = true;
                        break;
                    }
                }
                if (!removed && count < OP_SEQ_MAX_NOTES_PER_STEP) {
                    pat->step_notes[s_seq_edit_step][count] = note;
                    pat->step_note_count[s_seq_edit_step] = (uint8_t)(count + 1u);
                }
                pat->step_pitch_override[s_seq_edit_step] = true;
                tiles_haptics_trigger_touch_pulse(pad);
                /* Stays open -- see the edit_pad branch above for what
                 * actually closes this view now. */
                s_pitch_edit_prev_pad_touched[pad - 1u] = touched;
                return;
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

/* Real feedback: "the +- transport controls shoudl be the ones with
 * the flashing logic while in sequencer mode" -- same logic as render_
 * sequencer()'s own inline copy just above (see that one's comment for
 * the full reasoning, including the later correction of "-"'s own
 * solid/pulse split to key off step position rather than lane_running),
 * duplicated rather than shared since that one has its own circle-
 * column beat-flash handling in the same loop this function was never
 * given. */
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

/* Reuses services/lighting.c's own melodic-idle note-role coloring
 * (root=magenta, natural=white, sharp=off, via the same
 * tiles_note_map_is_root_pad()/_is_natural_pad() accessors) so the "pick a
 * note" surface looks like a natural extension of melodic play rather
 * than a new visual language -- the step's currently-assigned note pulses
 * white instead, this file's own established "selected" language. */
static void render_pitch_edit(uint32_t now_ms, bool transport_running) {
    float pulse = menu_selected_pulse_level(now_ms);
    op_seq_pattern_t *pat = active_pattern();
    /* Real feedback: "sequencer real time and note select should allow
     * for multiple notes per step" -- highlights every pad in the
     * CURRENT cluster now, not just one. An un-overridden step (nothing
     * added to it yet at all) still previews its own live-resolved
     * fallback note, same as before. */
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

/* A simple linear "meter" across all 24 pads -- how many are lit is
 * directly proportional to the live value, so pressing deeper/shallower
 * gives immediate, legible visual feedback of exactly what Hall depth is
 * currently dialing in. */
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
 * already established for the scale picker's own reserved slots -- see
 * handle_menu_taps() below for the "not selectable" half. */
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
        /* Triangle, not diamond -- this sub-menu now opens via
         * triangle+shift, see handle_triangle_click()'s own comment. */
        float level = (col == TILES_TRIANGLE_BUTTON_COL) ? OP_TRIANGLE_LED_MENU_LEVEL : 0.0f;
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

/* Real feedback: sequencer patterns tried a PER-PATTERN scale (temporarily
 * swapping note_map.c's global scale slot while this picker was open, then
 * writing the pick back into the pattern and restoring the real global on
 * exit) -- real feedback reversed that call: "i need it to be a universal
 * scale for now." One scale, note_map.c's own global, shared by melodic,
 * chord, AND every sequencer pattern; shift+triangle always edits that
 * single slot directly regardless of s_active_mode now, matching how
 * melodic/chord mode already worked before sequencer mode had its own
 * per-pattern branch. Existing pattern data is untouched by a scale
 * change -- see seq_fire_note()'s own tiles_note_map_quantize_to_scale()
 * call for how already-programmed steps instead get approximated to
 * whatever's currently selected, live, at the moment they actually play. */
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
    /* Real feedback: "why does pressing a pattern then a scale sewnd me
     * back to a broken melodic layout? is the return to melodic broken?
     * it should only do that on melodic not in other modes or sequencer
     * mode." Sequencer mode keeps standby active for its ENTIRE duration
     * -- render_sequencer() is rendered THROUGH that same mechanism (see
     * set_active_mode()'s own OP_MODE_SEQUENCER branch and pattern_bank_
     * exit()'s identical guard) -- so turning it off unconditionally here
     * killed the step view's own rendering the instant this picker closed,
     * even though s_active_mode was still genuinely OP_MODE_SEQUENCER.
     * menu_exit() (the top-level mode picker) already needed this exact
     * guard for the identical reason; this picker just never got it. */
    if (s_active_mode != OP_MODE_SEQUENCER) {
        tiles_lighting_set_standby_active(false);
        tiles_buttons_set_standby_active(false);
    }
    /* See menu_exit()'s own comment (same bug, same fix, same root
     * cause) -- real feedback: "the light behabes weird for triangle,
     * when scale is selected the light stays on." Triangle now owns
     * this sub-menu's LED column too (see handle_triangle_click()'s
     * shift branch) and has a PERMANENT override claimed, so buttons.c's
     * refresh_all_button_leds() (run by tiles_buttons_set_standby_
     * active(false) just above) deliberately skips it -- nothing else
     * repaints it back to off without this explicit write. Unconditional
     * regardless of the guard above -- a separate, narrower fix for a
     * different real bug. */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
}

/* ---- Pattern persistence (flash) ---------------------------------------
 * Real feedback: "to save patterns to memory before shutdown in the
 * pattern selector menu we click shift and the pattern. that saves
 * it, to delete or clear pattern we hold shift and patterrn for 3
 * seconds." The gesture itself lives in handle_pattern_bank_taps()
 * below (shift/circle held at touch-down turns that touch into a
 * save/delete candidate instead of the bank's normal select-on-depth
 * gesture) -- this section is just the flash mechanics underneath it.
 *
 * Storage: everything (both lanes' worth of "is this slot saved" plus
 * all 24 patterns' full step data) fits in one 4096-byte flash sector
 * with room to spare (~2.9KB used of 4096) -- see tiles_pattern_
 * store_t below. Lives in the LAST sector of the chip's flash
 * (TILES_PATTERN_FLASH_OFFSET), as far as possible from the app image
 * at the very start of flash, so the app can grow substantially before
 * ever needing to worry about this colliding with it. A magic number +
 * version guard against reading garbage on a never-saved-before board
 * (flash's own erased state is all-0xFF, which won't match) or a
 * future incompatible layout change.
 *
 * Real hardware constraint, not a bug: RP2040/2350 flash is memory-
 * mapped and executed from directly (XIP) -- nothing can read
 * instructions (or data) FROM flash while it's being erased/
 * programmed, which means every interrupt (USB included) must stay
 * disabled for the whole span, and hardware_flash.h's own
 * flash_range_erase()/flash_range_program() do NOT do this
 * automatically (confirmed reading pico-sdk's own flash.c -- they
 * assume the caller already arranged it). pattern_store_write_all()
 * below wraps both calls in save_and_disable_interrupts()/restore_
 * interrupts(), the correct single-core pattern (this firmware never
 * runs anything on core 1). Pets the watchdog immediately before and
 * after, not during -- watchdog_update() is ordinary flash-resident
 * code, unsafe to call from inside that same disabled window, but a
 * 4KB erase + program on this board's own W25Q-family chip
 * (board_init.c's own boot2_name) comfortably finishes in well under
 * DEBUG_WATCHDOG_TIMEOUT_MS even from a cold pet. This DOES mean a
 * save/delete causes a genuine, brief (tens of milliseconds) pause in
 * everything -- MIDI, touch, USB -- exactly once, at the moment it's
 * triggered; an inherent property of writing flash on this chip, not
 * something worth working around for a deliberate, occasional action
 * like this one. */
#define TILES_PATTERN_STORE_MAGIC 0x454c4954u /* "TILE" -- matches services/debug_mode.c's own crash-magic convention */
/* Bumped for the multi-note-per-step change (op_seq_pattern_t's
 * step_note[]->step_notes[][]/step_note_count[] -- see that struct's
 * own comment) -- a real layout change, not just new fields appended,
 * so version 1 flash data must never be reinterpreted against it.
 * pattern_store_load_all()'s own version check already treats any
 * mismatch as "never saved before on this board," the same safe
 * fallback a first-ever boot gets -- existing saved patterns are lost
 * across this specific update, not corrupted. */
#define TILES_PATTERN_STORE_VERSION 2u
#define TILES_PATTERN_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint32_t version;
    /* One bit per [lane][alt] slot (24 of the 32 bits used) -- packed
     * rather than a plain bool[4][6] (24 bytes) specifically to claw
     * back the header room OP_SEQ_MAX_NOTES_PER_STEP going from 1 to 2
     * needed (see that constant's own comment): the whole store must
     * fit in one 4096-byte flash sector, and this struct was landing 16
     * bytes over that budget at N=2 before this -- see pattern_store_
     * pack_slot_saved()/_unpack_slot_saved() below for the conversion,
     * kept out of this struct itself so s_pattern_slot_saved (the
     * actual runtime array, used constantly, indexed directly) never
     * has to change shape just to satisfy the on-flash layout. */
    uint32_t slot_saved_mask;
    op_seq_pattern_t pattern[OP_SEQ_NUM_LANES][OP_SEQ_ALTS_PER_LANE];
} tiles_pattern_store_t;

/* Must fit in exactly one flash sector -- see TILES_PATTERN_FLASH_
 * OFFSET's own comment for why growing this to span a second sector
 * isn't a safe fallback for a future overflow, and OP_SEQ_MAX_NOTES_
 * PER_STEP's own comment for how tight this already is at 2. A hard
 * compile error here beats a silent memcpy() past the end of
 * pattern_store_write_all()'s own write buffer -- exactly what
 * happened, caught only by a compiler warning, while building this
 * feature in the first place. */
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

/* Rewrites the WHOLE store every time, even though only one slot
 * usually changed -- flash can only be erased a whole sector at a
 * time, so a partial update needs the same read-modify-write shape
 * regardless; simpler to always start from current RAM state (already
 * the full, correct picture) than to read-back/merge against whatever
 * flash currently holds. static, not stack: ~2.9KB is too large to
 * comfortably put on this main loop's own stack frame for what's
 * already a rare, deliberate call. */
static void pattern_store_write_all(void) {
    static uint8_t s_write_buf[FLASH_SECTOR_SIZE];
    memset(s_write_buf, 0, sizeof(s_write_buf));
    tiles_pattern_store_t *store = (tiles_pattern_store_t *)s_write_buf;
    store->magic = TILES_PATTERN_STORE_MAGIC;
    store->version = TILES_PATTERN_STORE_VERSION;
    store->slot_saved_mask = pattern_store_pack_slot_saved();
    memcpy(store->pattern, s_seq_pattern, sizeof(store->pattern));

    watchdog_update();
    uint32_t prev_interrupts = save_and_disable_interrupts();
    flash_range_erase(TILES_PATTERN_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TILES_PATTERN_FLASH_OFFSET, s_write_buf, sizeof(s_write_buf));
    restore_interrupts(prev_interrupts);
    watchdog_update();
}

/* Real feedback: "we need a flash in green to confirm when a pattern
 * is saved. flash green twice in underglow and pad. and for delete
 * flash red twice in pad and underglow." render_pattern_bank() (below)
 * checks this every frame it draws and, while active, overrides BOTH
 * the one cell just acted on and every underglow anchor to blink
 * green/red twice before falling back to the bank's own normal
 * per-cell rendering -- see that function's own comment for the exact
 * blink timing. */
static bool s_pattern_flash_active;
static bool s_pattern_flash_is_delete; /* false = green (saved), true = red (deleted) */
static uint32_t s_pattern_flash_start_ms;
static uint8_t s_pattern_flash_pad; /* 1..24, the one cell just saved/deleted */

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

/* Called once, from tiles_op_mode_init(), AFTER that function's own
 * pattern-defaults loop has already run -- only ever overwrites a slot
 * this same flash data already claims to own (slot_saved[lane][alt]),
 * leaving every other slot at the normal fresh-boot default untouched.
 * A plain memory-mapped read (flash is directly addressable for
 * reading, unlike writing -- no erase/program machinery needed here at
 * all), so unlike the write path, this is exactly as fast/safe as
 * reading any other constant and needs no special handling. */
static void pattern_store_load_all(void) {
    const tiles_pattern_store_t *store = (const tiles_pattern_store_t *)(XIP_BASE + TILES_PATTERN_FLASH_OFFSET);
    if (store->magic != TILES_PATTERN_STORE_MAGIC || store->version != TILES_PATTERN_STORE_VERSION) {
        return; /* never saved before on this board, or an incompatible future layout */
    }
    pattern_store_unpack_slot_saved(store->slot_saved_mask);
    for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
        for (uint8_t alt = 0; alt < OP_SEQ_ALTS_PER_LANE; alt++) {
            if (s_pattern_slot_saved[lane][alt]) {
                s_seq_pattern[lane][alt] = store->pattern[lane][alt];
            }
        }
    }
    printf("[op_mode] loaded saved patterns from flash\n");
}

/* ---- Pattern bank (SW4/diamond+shift, sequencer mode only) -------------
 * Real feedback: "in sequencer mode shift plus triangle opens up the
 * pattern bajnk... sequence selector should have all 24 pads as possible
 * sequences... lets do 4 independent sequences that can be assigned to 4
 * channels selectable by each row of 6 alternatives... make each row a
 * different color in seelctor to signify 4 lanes," later moved off
 * triangle entirely: "i want shift plus diamond in sequencer only to be
 * the pattern selector... capture mode is triggered by diamond in
 * sequencer mode" (see handle_diamond_transport()'s own release branch
 * -- shift+diamond toggles this bank; plain diamond, no shift, toggles
 * capture mode instead -- and shift+triangle for the new per-pattern
 * scale picker, see handle_triangle_click()'s own comment). Every one of
 * the 24 pads is a real, individually selectable slot -- row = LANE (see
 * this file's own "Multi-lane pattern bank" section), column = which of
 * that lane's 6 alternatives. Tapping a cell does two things at once:
 * picks that alternative as the lane's currently PLAYING pattern
 * (s_seq_active_alt[lane]), and makes that lane the one shown/edited in
 * the main step view (s_seq_edit_lane) -- one gesture unambiguously
 * specifies both "which lane" and "which alternative for it," so no
 * separate lane-select gesture is needed.
 * Visual language, standardized further per real feedback -- see
 * render_pattern_bank()'s own comment for the full per-cell breakdown
 * (selected = red flash; playing elsewhere = white flash; has content =
 * that lane's own dim color; genuinely empty = off). */
#define OP_PATTERN_BANK_FLASH_MS 300u
/* Four maximally-distinguishable hues, deliberately avoiding red (reserved
 * for "selected" above) and Sentia's own brand magenta (reserved
 * elsewhere in this file for capture mode's playhead / the length-change
 * flash, so that color keeps one unambiguous meaning). Unmeasured -- a
 * starting guess at legibility, same as most of this file's other
 * real-hardware-tuned color constants. */
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
/* Save/delete hold-tracking -- see handle_pattern_bank_taps()'s own
 * comment on the shift+touch gesture these back. Recorded once, at
 * touch-down, matching every other modifier gesture in this file
 * (checked at press-start, e.g. handle_diamond_transport()'s own
 * circle_held capture) rather than continuously re-checked, so
 * releasing shift partway through a hold that already committed to
 * being a save/delete candidate can't flip it back into a plain
 * select. */
static bool s_pattern_bank_touch_started_with_shift[TILES_NUM_PADS];
static uint32_t s_pattern_bank_touch_started_ms[TILES_NUM_PADS];
static bool s_pattern_bank_delete_fired[TILES_NUM_PADS];
/* Real feedback: "to delete or clear pattern we hold shift and
 * patterrn for 3 seconds" -- matches this codebase's other established
 * 3-second holds (services/expression_control.h's own mute combo)
 * rather than inventing a new duration. */
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

/* Real feedback: "shift diamond does pattern opicker but only full or
 * enabeled patterns are on, rn i see all of them on, the idea is for 4
 * patterns to be able to run at once on different signal channels. if a
 * pattern is empty there is no light but lights will appear if pattern
 * is filled or modified. the flashing red indicator only applies for the
 * selected patten at the time and flashing white appeards for the
 * playing but not selectedd pattern. patterns with notes are led on
 * respectively and emptu ones are off." One unified rule now (the
 * previous round's separate stopped/running display was wrong -- real
 * feedback corrected it), checked per cell, most-specific first:
 * - The ONE cell that is both this lane's own current pick AND the lane
 *   currently shown in the main step view (s_seq_edit_lane) -- hard
 *   on/off RED FLASH, always, regardless of whether it has content yet
 *   (you need to see your own cursor even on a still-empty slot you're
 *   about to record into).
 * - Any OTHER lane's own current pick, while that lane is actually
 *   RUNNING (s_seq_lane_running[lane], independent per lane -- real
 *   feedback: "play and stop are independent per active pattern...
 *   remeber 4 patterns can play at once") -- FLASHING WHITE. Up to 3 of
 *   these can be lit at once, on rows you aren't currently viewing.
 * - Any cell with real content (pattern_has_content() above), whether or
 *   not it's currently picked for its lane -- that lane's own dim
 *   identity color (lane_color()).
 * - Otherwise (genuinely empty, not selected, not playing) -- OFF. */
/* Real feedback: "we need a flash in green to confirm when a pattern
 * is saved. flash green twice in underglow and pad. and for delete
 * flash red twice in pad and underglow." Two full on/off cycles,
 * timed independently of OP_PATTERN_BANK_FLASH_MS's own selection-
 * indicator blink just above -- this one needs to actually FINISH
 * (stop on its own after exactly twice), not blink for as long as some
 * ongoing state stays true. */
#define OP_PATTERN_FLASH_BLINK_MS 150u
#define OP_PATTERN_FLASH_COUNT 2u
#define OP_PATTERN_FLASH_TOTAL_MS (OP_PATTERN_FLASH_BLINK_MS * 2u * OP_PATTERN_FLASH_COUNT)

/* Real feedback: "touching the pad with shift is doing the delete and
 * save but the led indication is not working there is no underglow
 * and no pad flash confirmation either." The pad half was already
 * correct (render_pattern_bank() below has always set it); the
 * underglow half never had a real chance to show at all -- services/
 * lighting.c's own tiles_lighting_service() only ever calls the plain
 * write_underglow() that WOULD have shown this while NEITHER crash nor
 * debug mode's own overrides are active, and debug mode has been armed
 * for nearly this entire session specifically to catch crash reports.
 * Whenever it's on, its magenta pulse unconditionally owns underglow,
 * silently swallowing this confirmation's green/red the whole time,
 * exactly matching what was reported. Read-only (unlike render_
 * pattern_bank()'s own copy of this same elapsed/blink math, this one
 * deliberately does NOT clear s_pattern_flash_active on expiry -- that
 * stays render_pattern_bank()'s own job, since it's the one guaranteed
 * to run every scan the bank is actually visible; this getter might be
 * called from services/lighting.c at a different point in the same
 * scan, or not at all if the bank isn't even open). */
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

    bool save_flash_showing = false;
    bool save_flash_on = false;
    if (s_pattern_flash_active) {
        uint32_t elapsed = now_ms - s_pattern_flash_start_ms;
        if (elapsed >= OP_PATTERN_FLASH_TOTAL_MS) {
            s_pattern_flash_active = false;
        } else {
            save_flash_showing = true;
            save_flash_on = ((elapsed / OP_PATTERN_FLASH_BLINK_MS) % 2u) == 0u;
        }
    }

    for (uint8_t row = TILES_GRID_MIN_ROW + 1u; row <= TILES_GRID_MAX_ROW; row++) {
        uint8_t lane = (uint8_t)(row - (TILES_GRID_MIN_ROW + 1u));
        float lr, lg, lb;
        lane_color(lane, &lr, &lg, &lb);
        for (uint8_t col = TILES_GRID_MIN_COL; col <= TILES_GRID_MAX_COL; col++) {
            uint8_t alt = (uint8_t)(col - TILES_GRID_MIN_COL);
            uint8_t pad = board_pad_for_row_col(row, col);
            bool is_active = (alt == s_seq_active_alt[lane]);
            if (save_flash_showing && pad == s_pattern_flash_pad) {
                /* Takes priority over every other cell state below --
                 * confirming a save/delete just happened matters more,
                 * for this brief window, than this cell's own normal
                 * selected/playing/has-content status. */
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
        /* Same "which button got you here" language every other
         * sub-menu in this file uses -- diamond's own column now, not
         * triangle's, since this bank moved to shift+diamond (see this
         * file's own "Pattern bank" section header). */
        float level = (col == TILES_DIAMOND_BUTTON_COL) ? OP_TRIANGLE_LED_MENU_LEVEL : 0.0f;
        tiles_buttons_set_standby_led(board_button_for_col(col), level);
    }
    /* Real feedback (after the first fix attempt): "the save pattern and
     * dleete patter still do not do the pulse underglow." Root cause of
     * THAT: this loop used to write the flash color directly to hardware
     * here (via the standby setter's own immediate write-on-change), on
     * every tiles_op_mode_scan() call -- but main.c's loop calls tiles_
     * lighting_service() again right after every single scan, and
     * THAT'S the one tiles_op_mode_pattern_flash_underglow_color() was
     * wired into (above debug mode -- see that function's own comment).
     * Debug mode had been armed for nearly this entire session, so its
     * override was written a few instructions after this one, every
     * iteration, unconditionally clobbering whatever this loop had just
     * put on the strip before a single frame of it could ever appear --
     * pads never had this problem since they have no such second writer.
     * Fix: this loop now only ever writes the OFF/idle case itself, and
     * defers to tiles_lighting_service()'s own priority chain (the
     * already-correctly-ordered one) for the flash colors, so there is
     * exactly one writer for that state and nothing left to race. */
    for (uint8_t i = 0; i < TILES_NUM_UNDERGLOW_ANCHORS; i++) {
        if (!save_flash_showing) {
            tiles_lighting_set_standby_underglow_rgb(i, 0.0f, 0.0f, 0.0f);
        }
    }
}

/* Same touch-click + push-past-50%-selects gesture every picker in this
 * file already uses -- UNLESS shift/circle is already held at the
 * moment a cell is first touched, in which case that touch becomes a
 * save/delete candidate instead (real feedback: "in the pattern
 * selector menu we click shift and the pattern. that saves it, to
 * delete or clear pattern we hold shift and patterrn for 3 seconds"):
 * a quick shift+touch (released before OP_PATTERN_DELETE_HOLD_MS)
 * saves that slot to flash on release; holding it past that threshold
 * clears/deletes it instead, firing once (edge-latched via s_pattern_
 * bank_delete_fired) rather than repeatedly for as long as the hold
 * continues, same one-shot shape this file's other hold gestures use.
 * Shift must be down at touch-DOWN specifically, not just at some
 * point during the touch or checked live every scan -- matches every
 * other modifier gesture in this file (checked once, at press-start)
 * and means letting go of shift partway through an already-committed
 * hold can't un-arm it or fall back to a plain select. */
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
                    /* Same cross-pattern ratchet mix-up guard the
                     * original single-lane bank already established. */
                    s_seq_ratchet_remaining[lane] = 0u;
                    s_seq_active_alt[lane] = alt;
                    /* Real bug caught auditing this: s_seq_current_step
                     * belongs to whichever pattern was PREVIOUSLY active
                     * on this lane, not the one just switched to -- if
                     * this lane is currently stopped, seq_advance_clock()
                     * never gets a chance to normalize it (that function
                     * returns immediately while !s_seq_lane_running[lane],
                     * before ever reaching pending-start handling), so a
                     * LATER "+" press would resume the NEW pattern from
                     * whatever step index the OLD one happened to be left
                     * at -- reset explicitly here so the newly-picked
                     * pattern always starts clean from step 0 regardless
                     * of when (or whether) this lane is next started. */
                    s_seq_current_step[lane] = 0u;
                    /* Quantizes the swap to the next beat boundary via
                     * the SAME pending-start mechanism a fresh Start/
                     * resume already uses (see seq_advance_clock()'s own
                     * pending-start handling) instead of jumping straight
                     * to step 0 at a possibly-mid-beat instant -- also
                     * avoids a second tiles_midi_clock_get_state() call
                     * here, which would silently steal a real start_edge
                     * from the ONE call tiles_op_mode_scan() already made
                     * this scan (that function consumes it as a side
                     * effect -- see seq_advance_clock()'s own header
                     * comment). Only actually consumed if this lane is
                     * currently running -- otherwise harmlessly inert
                     * until a future "+" press starts it, at which point
                     * the step-0 reset just above is what makes that
                     * first playthrough correct. */
                    s_seq_pending_start[lane] = true;
                    s_seq_pending_restart[lane] = true;
                    printf("[op_mode] lane %u pattern -> %u\n", (unsigned)lane, (unsigned)alt);
                }
                s_seq_edit_lane = lane;
                pattern_bank_exit();
                return; /* grid ownership just changed under this loop -- stop iterating it */
            }
            s_pattern_bank_prev_pad_touched[pad - 1u] = touched;
        }
    }
}

static void pattern_bank_enter(void) {
    /* Defensive: a per-step pitch/probability/ratchet edit is a genuine
     * TOGGLE that can sit open with no pad touched (release the
     * originally-held step and it just waits, see this file's own
     * "Per-step editing" section) -- easy to leave open, then reach for
     * this pattern-bank gesture (plain diamond, no shift, as of the
     * diamond/shift swap -- see handle_diamond_transport()'s own
     * comment) with a free hand. Without this, tiles_op_mode_scan()'s
     * dispatch (which checks s_pattern_bank_visible before s_seq_edit_
     * mode) would show the bank while the edit view stayed silently
     * "open" underneath, popping back up the instant the bank closes. */
    if (s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
        edit_exit();
    }
    /* Silences whatever's currently sounding on the EDITED lane the
     * instant the bank opens -- the other 3 lanes keep playing right
     * through this, same "runs in the background" precedent as every
     * other sub-view in this file now (see tiles_op_mode_scan()'s own
     * unconditional seq_advance_clock() loop) -- without this, a note
     * struck on the edited lane right before opening the bank would
     * otherwise just hang audibly for as long as the bank stays open. */
    seq_end_current_note(s_seq_edit_lane);
    s_pattern_bank_visible = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_pattern_bank_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
        /* A finger already resting on a pad at the exact instant the
         * bank opens can never retroactively count as a fresh shift+
         * touch-down (the loop above already prevents that from being
         * misread as any kind of touch-down at all), so this is purely
         * defensive tidiness, not a real reachable gap -- avoids ever
         * showing a stale true from some earlier session while auditing
         * this state later. */
        s_pattern_bank_touch_started_with_shift[i] = false;
        s_pattern_bank_delete_fired[i] = false;
    }
}

static void pattern_bank_exit(void) {
    s_pattern_bank_visible = false;
    /* Real feedback: "selectring a new sequence takes that initial press
     * as a note on for that step, lets fix that" -- the finger that just
     * tapped a pattern to select it is often still down the instant this
     * returns control to the normal step view; without this resync,
     * seq_handle_step_taps() would see that same pad go touched=true with
     * its OWN prev-touched tracking still stale at false (never updated
     * while the bank owned the grid) and misread it as a fresh arm-toggle
     * touch landing on the NEWLY selected pattern. Exact same fix
     * edit_exit() already applies for the identical reason -- see that
     * function's own comment -- including seq_pad_for_step(), not a
     * bare i+1u, for the same 16-step 4x4-layout reason. */
    op_seq_pattern_t *layout_pat = active_pattern();
    for (uint8_t step = 0; step < OP_SEQ_NUM_STEPS; step++) {
        s_seq_prev_pad_touched[step] = tiles_touch_is_touched(seq_pad_for_step(layout_pat, step));
        s_seq_step_touch_started_ms[step] = 0u;
    }
    /* Deliberately does NOT touch standby_active, unlike scale_menu_
     * exit() -- sequencer mode already keeps buttons/lighting standby-
     * active for its ENTIRE duration (see set_active_mode()'s own
     * OP_MODE_SEQUENCER branch), so turning it off here would kill the
     * normal step view's own rendering the instant the bank closes,
     * not just this sub-view's. Still need the same triangle-LED fix
     * every other sub-view here needs (see scale_menu_exit()'s own
     * comment for the full bug/root-cause) -- refresh_all_button_leds()
     * won't run from this exit either way (standby never toggles off
     * here), but writing it unconditionally is harmless and correct
     * once standby genuinely does end later, same reasoning as the old
     * picker's own version of this exact function. */
    tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, 0.0f);
}

/* Both declared for real further down (with the rest of capture mode's
 * state/functions) -- forward-declared here only so set_active_mode()'s
 * defensive safety-net check below can see them. */
static bool s_seq_capture_mode_active;
static void seq_capture_mode_exit(void);
/* Same reason, for the cross-mode "capture into lane 3" feature just
 * below it -- see that section's own header comment. */
static bool s_cross_capture_active;
static void cross_capture_exit(void);

static void set_active_mode(tiles_op_mode_t mode) {
    if (s_cross_capture_active && mode != s_active_mode) {
        /* Checked BEFORE the bare seq_capture_mode_exit() below, and as
         * an else against it -- cross_capture_exit() ALSO restores
         * s_seq_edit_lane and clears s_cross_capture_active, neither of
         * which the bare call touches; calling both (or the wrong one
         * first) would leave this feature's own state stuck pointing at
         * lane 3. Real feedback: "captures from melodic mode or chord
         * mode or any mode into lane 3... on command" -- this only ever
         * exists WHILE some non-sequencer mode is active, so any genuine
         * mode change at all -- including into sequencer mode itself --
         * ends it, since the bank a capture targets is tied to whichever
         * mode was active when it started. */
        cross_capture_exit();
    } else if (s_seq_capture_mode_active && mode != OP_MODE_SEQUENCER) {
        /* Defensive: capture mode is normally only ever left via its own
         * shift/diamond exit gestures (see seq_capture_mode_exit()'s own
         * call sites), but nothing currently stops a plain triangle
         * click from opening the top-level menu WHILE it's active too --
         * if that menu then commits a DIFFERENT mode, capture mode must
         * not stay latched true underneath it. Safe against seq_capture_
         * mode_enter()'s own set_active_mode(OP_MODE_SEQUENCER) call:
         * that always requests sequencer specifically, and s_seq_
         * capture_mode_active isn't set true until after it returns, so
         * this branch can never fire from that call. */
        seq_capture_mode_exit();
    }
    /* Deliberately does NOT seq_end_current_note() on leaving sequencer
     * mode anymore -- real feedback: "sequencer should not stop if mode
     * is changed. it should be able to run in the background." Whatever
     * note is currently sounding keeps sounding, and seq_advance_clock()
     * (now called every scan regardless of s_active_mode -- see
     * tiles_op_mode_scan()'s own comment) keeps ending/firing notes on
     * its own schedule exactly as if sequencer mode were still the one
     * displayed; switching the DISPLAY away from it no longer implies
     * stopping its PLAYBACK. */
    if (s_active_mode == OP_MODE_CHORD && mode != OP_MODE_CHORD) {
        chord_end_all_notes();
    }
    if (mode != OP_MODE_MELODIC) {
        /* Melodic's own sub-menu can't stay open once melodic isn't the
         * active mode anymore -- no swap to unwind now that the scale is
         * universal (see scale_menu_enter()'s own comment), just close
         * the view; this bypasses the normal scale_menu_exit() call
         * entirely (no LED/standby cleanup here, matching this function's
         * existing light-touch force-close). */
        s_scale_menu_visible = false;
    }
    if (mode != OP_MODE_SEQUENCER) {
        /* Same reasoning, sequencer's own two sub-views: the per-step
         * edit view, and the pattern bank that came back to replace the
         * original pattern/channel picker (see this file's own
         * "Pattern/channel picker: REMOVED" and "Pattern bank"
         * sections). */
        s_seq_edit_mode = OP_SEQ_EDIT_NONE;
        s_pattern_bank_visible = false;
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

/* ---- Sequencer capture mode (SW6/shift + SW4/diamond) ------------------
 * Real feedback: "make a sequencer capture mode when sifht and diamoind
 * clicked together. this means the sequencer turns into the regular
 * chromatic scale and captures the lplayed melody into sequecer in the
 * current tempo quantized but also do allow overlap. this makes the
 * diamond flash glow and then exit into sequencer is by shift or by
 * diamond, not directly to the menu. the curent step of the sequencer
 * should light up pink sentia when the sequencer is at that step."
 *
 * Entry auto-switches into sequencer mode from wherever the player
 * currently is (real feedback's own "exit INTO sequencer" phrasing only
 * makes sense if entry can start from somewhere else) rather than only
 * being reachable from within it already. Unlike the pattern bank above,
 * this stays fully self-contained -- it does NOT claim a sub-view flag
 * that routes through the normal sequencer dispatch, it owns its own
 * three pieces (seq_capture_handle_taps(), seq_capture_advance_clock(),
 * render_seq_capture()) called directly from tiles_op_mode_scan() below.
 *
 * Deliberately does NOT reuse services/expression.c's real touch/note
 * pipeline the way melodic/chord/guitar do -- capture mode still needs
 * tiles_op_mode_owns_pad_grid() to stay true (unchanged, same as every
 * other sequencer sub-view) so a touch here is never ALSO read as a
 * normal melodic strike underneath, which means it needs its own direct
 * note-on/off, mirroring how services/op_mode.c already drives chord
 * mode's own 8 chord-strip pads directly instead of going through
 * expression.c for those either. A simpler, single-velocity trigger
 * (OP_SEQ_VELOCITY, the same fixed value every existing sequencer note
 * already uses) rather than expression.c's full velocity/pitch-bend/
 * aftertouch pipeline -- capture mode is for sketching a melody's
 * NOTES quickly, not a nuanced performance capture.
 *
 * "allow overlap": a fresh touch always wins over whatever was already
 * sounding (end the old note, start the new one, same "hold the trig,
 * play the note" simplicity this file's own per-step pitch-assignment
 * view already established) -- overlapping touches never reject or
 * glitch, they just hand off cleanly to whichever pad was touched most
 * recently, both for the audible note AND for which note gets written
 * into the step currently being recorded. */
/* RE-ENABLED -- real feedback: "lets re work capture mode into
 * sequencer witgh the diamond button." Previously disabled after
 * "something made it freeze and crash in sequwencer mode with ableton
 * midi clock" (reported while a pattern was just running, no
 * capture-mode interaction that session), then "for now also disabel
 * the live capture stuff" out of caution even though the confirmed
 * cause was unrelated printf() flooding in services/haptics.c's own
 * per-note hot path (see that file's own history in this section's
 * README entry), not capture mode itself. The entry gesture (diamond,
 * plain click, in sequencer mode) is unchanged -- this flag only ever
 * gated reachability, never the logic itself, which is why re-enabling
 * it needed no changes here beyond flipping this back to 1 plus the
 * rendering/quantization rework real feedback asked for alongside it
 * (see render_seq_capture()'s and s_seq_capture_target_step's own
 * comments). Still genuinely new to real playing time again as of this
 * change -- worth watching closely on the next real-hardware pass,
 * same as everything else reintroduced this session. */
#define OP_SEQ_CAPTURE_MODE_ENABLED 1
static tiles_scale_mode_t s_seq_capture_prev_scale;
static bool s_seq_capture_prev_pad_touched[TILES_NUM_PADS];
/* Accumulator for the step currently being recorded -- reset at the
 * start of each step's window, committed into active_pattern()'s real
 * step data the moment the NEXT step boundary arrives (see
 * seq_capture_advance_clock() below). This is what makes capture
 * "quantized": a touch's real timing only ever determines WHICH step's
 * window it fell in, not a sub-step offset.
 * Real feedback: "sequencer real time and note select should allow for
 * multiple notes per step so if i play a cluster of notes we should be
 * able to save those in that single step." Grows as more touches land
 * while still targeting the SAME step (see seq_capture_handle_taps())
 * instead of the single note this used to hold -- a touch resolving to
 * a DIFFERENT target step starts a fresh cluster instead of appending
 * to a stale one. count == 0 means nothing armed at all, replacing the
 * old separate bool. */
static uint8_t s_seq_capture_armed_notes[OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_capture_armed_count;
/* Real feedback: "make sure to quatize capture mode to closest step."
 * Computed once, at the moment of the FIRST touch starting a new
 * cluster (see seq_capture_handle_taps() below), from how far into the
 * CURRENT step's own window that touch actually landed -- past the
 * halfway point, the touch is nearer the step about to START than the
 * one about to end, so it's committed there instead. Without this, a
 * touch struck slightly early (a real, common thing -- anticipating the
 * beat) always landed on whichever step happened to still be in
 * progress, rounding every early hit down/backward rather than to
 * whichever step it actually meant. */
static uint8_t s_seq_capture_target_step;
/* Direct-drive sounding-note state, mirroring seq_end_current_note()'s
 * own s_seq_sounding_pad/note/s_seq_note_sounding shape but kept
 * separate -- capture mode's own notes are a live PERFORMANCE, not
 * scheduled playback, and the two must never be confused for each
 * other. Multiple entries now (see OP_SEQ_MAX_NOTES_PER_STEP's own
 * comment) -- a held cluster should all sound together, and releasing
 * one finger should only end THAT note, not the whole chord. */
static uint8_t s_seq_capture_live_pads[OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_capture_live_notes[OP_SEQ_MAX_NOTES_PER_STEP];
static uint8_t s_seq_capture_live_count;

/* Ends just the ONE live note `pad` owns, if any -- a no-op if `pad`
 * isn't currently sounding (already released, or never was). */
static void seq_capture_end_one_sounding_note(uint8_t pad) {
    for (uint8_t i = 0; i < s_seq_capture_live_count; i++) {
        if (s_seq_capture_live_pads[i] != pad) {
            continue;
        }
        tiles_midi_note_off(s_seq_lane_channel[s_seq_edit_lane], s_seq_capture_live_notes[i]);
        tiles_haptics_stop(pad);
        for (uint8_t j = i; (uint8_t)(j + 1u) < s_seq_capture_live_count; j++) {
            s_seq_capture_live_pads[j] = s_seq_capture_live_pads[j + 1u];
            s_seq_capture_live_notes[j] = s_seq_capture_live_notes[j + 1u];
        }
        s_seq_capture_live_count--;
        return;
    }
}

/* Ends every currently-live capture note at once -- capture mode
 * exiting, or the pattern otherwise needing a clean slate, same as a
 * hand lifting off the whole cluster together. */
static void seq_capture_end_all_sounding_notes(void) {
    for (uint8_t i = 0; i < s_seq_capture_live_count; i++) {
        tiles_midi_note_off(s_seq_lane_channel[s_seq_edit_lane], s_seq_capture_live_notes[i]);
        tiles_haptics_stop(s_seq_capture_live_pads[i]);
    }
    s_seq_capture_live_count = 0u;
}

/* Only ever called while s_active_mode is ALREADY OP_MODE_SEQUENCER --
 * see handle_diamond_transport()'s own release branch, which gates this
 * whole call behind sequencer_active. Real feedback: "capture mode is
 * triggered by diamond in sequencer mode" -- unlike an earlier round,
 * there's no longer a "jump into capture from any mode" gesture to
 * support, so this no longer needs to force a mode switch itself. */
static void seq_capture_mode_enter(void) {
    /* Defensive: shift+diamond (pattern bank) and plain diamond (this)
     * are two separate, independent gestures now -- nothing stops
     * opening the bank, releasing, then a later PLAIN diamond click
     * entering capture mode without ever closing the bank first.
     * Without this, capture mode would silently start taking over
     * s_seq_edit_lane's background playback while the VISIBLE view (and
     * touch routing) stayed on the pattern bank, since tiles_op_mode_
     * scan()'s dispatch checks s_pattern_bank_visible before s_seq_
     * capture_mode_active. Closing it first keeps the two mutually
     * exclusive. */
    if (s_pattern_bank_visible) {
        pattern_bank_exit();
    }
    /* Same defensive reasoning, same reachability gap, for the scale
     * picker (shift+triangle) instead of the pattern bank -- open it,
     * release, then a later plain diamond click enters capture mode
     * without ever closing the picker first, and capture mode's own
     * chromatic scale swap (see seq_capture_mode_enter()'s own comment
     * further below) would start underneath a sub-view that still thinks
     * it owns the grid/rendering. Calling the real scale_menu_exit() here,
     * not just clearing the flag, correctly unwinds its LED/standby state
     * before capture mode claims the grid for itself. */
    if (s_scale_menu_visible) {
        scale_menu_exit();
    }
    /* Same reachability gap a third time: a per-step pitch/probability/
     * ratchet edit (hold a step) can be left open, then a later plain
     * diamond click enters capture mode without ever backing out of it
     * first -- seq_handle_step_taps() (and so normal step taps) never
     * runs while this is active, but the edit view itself would keep
     * showing and keep consuming touches instead of capture mode's
     * note-input, since tiles_op_mode_scan()'s dispatch checks s_seq_
     * edit_mode before s_seq_capture_mode_active. No scale/channel side
     * effect to unwind here (pitch-assign doesn't touch note_map.c's
     * scale), but the same "don't let two exclusive sub-views both think
     * they own the grid" invariant still applies. */
    if (s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
        edit_exit();
    }
    /* Real feedback reversed this: "capture mode should not mute the
     * midi notes that are playing underneath[,] it should be additive
     * and real time." Used to force-end whatever the normal playback
     * engine had sounding on the edited lane right here, on entry --
     * reasoned at the time as "capture mode takes over this ONE lane,"
     * but in practice that's an abrupt, audible cutoff the instant
     * capture starts, not the smooth hand-off a real "start layering
     * in on top of what's already going" performance needs. Simply not
     * calling this lets whatever was already ringing keep ringing
     * (and end on its own normal note-off timing) while newly captured
     * content layers in additively from here on -- seq_capture_advance_
     * clock() below already handles ending its OWN previously-fired
     * notes as capture's own steps advance, same as normal playback
     * always has; this was only ever about the ONE note active at the
     * exact moment of entry, not an ongoing lifecycle concern. */
    s_seq_capture_mode_active = true;
    s_seq_capture_prev_scale = tiles_note_map_get_scale();
    tiles_note_map_set_scale(TILES_SCALE_CHROMATIC);
    s_seq_capture_armed_count = 0u;
    s_seq_capture_live_count = 0u;
    /* Real bug caught auditing this: seq_capture_advance_clock() only
     * ever checks the SHARED clock's own running state, never this
     * lane's own s_seq_lane_running -- so exiting capture mode used to
     * hand back to the normal seq_advance_clock() with this lane still
     * marked stopped, silently freezing the pattern you just recorded
     * the instant you left. Marking it running here (mirroring exactly
     * what "+" already does for a fresh start -- see handle_transport_
     * and_length()'s own sequencer branch) is what makes the freshly
     * captured pattern keep looping once you exit. */
    s_seq_lane_running[s_seq_edit_lane] = true;
    tiles_midi_clock_set_running(true);
    /* Same quantized-start behavior seq_start() itself already
     * establishes for entering sequencer mode fresh -- waits for the
     * next beat boundary (see seq_capture_advance_clock() below) rather
     * than starting to record at some arbitrary mid-phrase pulse count. */
    s_seq_pending_start[s_seq_edit_lane] = true;
    for (uint8_t i = 0; i < TILES_NUM_PADS; i++) {
        s_seq_capture_prev_pad_touched[i] = tiles_touch_is_touched((uint8_t)(i + 1u));
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

/* ---- Cross-mode capture into lane 3 ------------------------------------
 * Real feedback: "i also want to add a feature that captures from
 * melodic mode or chord mode or any mode into lane 3 sequencer on
 * command and each new capture from each mode goes into a different
 * bank of lane 3 effectively making it possible to run multiple
 * sequences for each lane at once... for trigger capture mode lets use
 * a push of shift and diamond if not in use already by another
 * function. this will make the steps start counting like in sequencer
 * flashing under the current layout and the playing gets saved."
 * Lane 3 (index 2, 0-based -- "3 is now capture" per that same message's
 * own lane-numbering reminder) specifically, not a new configurable
 * target -- this reuses the EXACT same seq_capture_mode_enter()/_exit(),
 * seq_capture_handle_taps(), seq_capture_advance_clock() this file
 * already has for sequencer mode's own plain-diamond capture, just by
 * temporarily repointing s_seq_edit_lane at lane 3 for the duration --
 * every one of those functions already reads s_seq_edit_lane/
 * active_pattern() internally, so none of them needed a single change
 * to work here too. What IS genuinely new: this runs while s_active_
 * mode is melodic/chord/guitar, not sequencer, and the current mode's
 * own grid must stay fully visible/playable underneath -- see this
 * feature's own dispatch in tiles_op_mode_scan() (falls through to the
 * normal per-mode render instead of seq_capture_mode's own render_seq_
 * capture(), which would replace the grid entirely) and write_cross_
 * capture_underglow() in services/lighting.c (an underglow-only
 * indicator, since the pad grid itself is spoken for). */
#define OP_CROSS_CAPTURE_LANE 2u
static uint8_t s_cross_capture_saved_edit_lane;

/* Real feedback: "each new capture from each mode goes into a
 * different bank of lane 3" -- a fixed mode->bank mapping, not a
 * separately-tracked "next free bank" counter: capturing from melodic
 * always lands in the same bank, so re-capturing from melodic later
 * deliberately overwrites that same bank rather than accumulating a
 * new one every time (matches every other pattern slot's own "re-
 * arming replaces what was there" behavior elsewhere in this file).
 * OP_MODE_SEQUENCER has no case -- this feature isn't reachable from
 * sequencer mode at all (sequencer_active is checked before either
 * gesture that could enter it), so it can never actually hit that
 * default. */
static uint8_t cross_capture_bank_for_mode(tiles_op_mode_t mode) {
    switch (mode) {
    case OP_MODE_MELODIC:
        return 0u;
    case OP_MODE_CHORD:
        return 1u;
    case OP_MODE_GUITAR:
        return 2u;
    default:
        return 0u;
    }
}

bool tiles_op_mode_cross_capture_is_active(void) {
    return s_cross_capture_active;
}

static void cross_capture_enter(void) {
    s_cross_capture_saved_edit_lane = s_seq_edit_lane;
    s_seq_edit_lane = OP_CROSS_CAPTURE_LANE;
    /* Deliberately NOT restored on exit -- this is the actual point of
     * the feature, not a temporary borrow: lane 3 keeps playing
     * whatever alt this capture just recorded into, same as any other
     * lane's own s_seq_active_alt persists once changed. */
    s_seq_active_alt[OP_CROSS_CAPTURE_LANE] = cross_capture_bank_for_mode(s_active_mode);
    s_cross_capture_active = true;
    seq_capture_mode_enter();
}

static void cross_capture_exit(void) {
    if (!s_cross_capture_active) {
        return;
    }
    seq_capture_mode_exit();
    s_seq_edit_lane = s_cross_capture_saved_edit_lane;
    s_cross_capture_active = false;
}

static void seq_capture_handle_taps(tiles_midi_clock_state_t clock) {
    uint8_t lane = s_seq_edit_lane;
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        bool touched = tiles_touch_is_touched(pad);
        bool was_touched = s_seq_capture_prev_pad_touched[pad - 1u];
        if (touched && !was_touched) {
            /* Real feedback: "sequencer real time and note select
             * should allow for multiple notes per step so if i play a
             * cluster of notes we should be able to save those in that
             * single step." Every new touch now sounds ALONGSIDE
             * whatever's already held (a real chord/cluster) instead of
             * cutting the previous one off first -- see this file's own
             * seq_capture_end_one_sounding_note() for how a single
             * finger lifting only ends its OWN note now, not the whole
             * cluster.
             * A chord-region pad, while chord mode is the mode this
             * capture is running from, contributes its OWN bass+root
             * (build_chord_voicing()'s first 2 of 4 voices -- the two
             * most foundational, and OP_SEQ_MAX_NOTES_PER_STEP's own
             * flash-capacity cap leaves no room for all 4 anyway) as
             * TWO notes from this one touch, not the plain melodic-
             * mapped single note tiles_note_map_get_note() would
             * otherwise resolve for that same pad -- without this,
             * capturing from chord mode would silently record the
             * wrong notes entirely, defeating the entire reason chord
             * mode is one of this feature's own named capture sources. */
            uint8_t notes[OP_SEQ_MAX_NOTES_PER_STEP];
            uint8_t note_count;
            if (tiles_note_map_is_chord_mode_active() && tiles_note_map_is_chord_region_pad(pad)) {
                uint8_t raw[TILES_NOTE_MAP_CHORD_NUM_NOTES];
                uint8_t voices[OP_CHORD_NUM_VOICES];
                tiles_note_map_get_chord_notes(pad, raw);
                build_chord_voicing(raw, voices);
                note_count = OP_SEQ_MAX_NOTES_PER_STEP;
                for (uint8_t i = 0; i < note_count; i++) {
                    notes[i] = voices[i];
                }
            } else {
                notes[0] = tiles_note_map_get_note(pad);
                note_count = 1u;
            }
            tiles_haptics_trigger_kick(pad, OP_SEQ_VELOCITY);
            for (uint8_t i = 0; i < note_count; i++) {
                tiles_midi_note_on(s_seq_lane_channel[lane], notes[i], OP_SEQ_VELOCITY);
                if (s_seq_capture_live_count < OP_SEQ_MAX_NOTES_PER_STEP) {
                    s_seq_capture_live_pads[s_seq_capture_live_count] = pad;
                    s_seq_capture_live_notes[s_seq_capture_live_count] = notes[i];
                    s_seq_capture_live_count++;
                }
            }
            /* Nearest-step quantization -- see s_seq_capture_target_step's
             * own comment. Whatever s_seq_pending_start[lane] leaves in
             * s_seq_step_started_at_pulse[lane] pre-start is meaningless
             * timing-wise, but harmlessly so: seq_capture_advance_clock()
             * unconditionally discards whatever's pending the moment the
             * quantized start actually resolves, same as it always did,
             * so a target computed from garbage here is discarded right
             * along with it, never actually committed anywhere. */
            uint8_t length = active_pattern()->length;
            if (length < 1u) {
                length = 1u;
            }
            uint32_t elapsed_in_step = clock.pulse_count - s_seq_step_started_at_pulse[lane];
            bool nearest_is_next_step = (elapsed_in_step * 2u) >= OP_SEQ_CLOCKS_PER_STEP;
            uint8_t target =
                nearest_is_next_step ? (uint8_t)((s_seq_current_step[lane] + 1u) % length) : s_seq_current_step[lane];
            /* Same target as whatever's already accumulating -- this
             * touch is another note in the SAME cluster, append. A
             * different (or no) target yet -- this is the first touch
             * of a fresh cluster, replacing whatever stale one was
             * pending (already committed or abandoned by now). */
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
    }
}

/* Deliberately its own function rather than a branch inside
 * seq_advance_clock() -- that function's probability/ratchet/note-
 * firing logic (seq_enter_step()/seq_fire_note()) is all about REPLAYING
 * already-programmed steps, none of which applies while RECORDING new
 * ones; keeping them fully separate means neither has to reason about
 * the other's state. Reuses s_seq_current_step[s_seq_edit_lane]/
 * s_seq_step_started_at_pulse[s_seq_edit_lane]/s_seq_pending_start[s_seq_
 * edit_lane], the SAME per-lane fields the normal engine uses for that
 * lane, so switching in and out of capture mode doesn't need its own
 * parallel copy of "where is the playhead right now" -- and so the other
 * 3 lanes' own identical fields are never touched by this at all. */
static void seq_capture_advance_clock(tiles_midi_clock_state_t clock) {
    uint8_t lane = s_seq_edit_lane;
    if (clock.start_edge) {
        s_seq_current_step[lane] = 0u;
        s_seq_step_started_at_pulse[lane] = clock.pulse_count;
        s_seq_capture_armed_count = 0u;
        s_seq_pending_start[lane] = false;
        return;
    }
    /* Real feedback: "the live capture mode within the sequencer mode
     * is still not sending the midi signals, it is capturing but its
     * not live playing looping." Root cause: this function used to
     * early-return here on !clock.running, same as seq_advance_clock()
     * -- but that check makes sense THERE (an already-recorded pattern
     * genuinely should pause when the transport stops) and does NOT
     * make sense HERE, where it silently blocked the actual commit-to-
     * pattern logic below on every single scan whenever real clock
     * bytes were arriving but tiles_midi_clock_is_running() itself
     * still read false -- exactly what happens whenever seq_capture_
     * mode_enter()'s own tiles_midi_clock_set_running(true) call turns
     * out to be a no-op (services/midi_clock.c's own "real clock always
     * wins" guard against a live external source, regardless of
     * whether THAT source's transport is actually playing yet). Live-
     * preview notes in seq_capture_handle_taps() fire purely from touch
     * events, completely unaffected by any of this, which is exactly
     * why capture could sound like it was working while nothing ever
     * actually landed in the pattern. clock.pulse_count itself keeps
     * advancing on every real Clock byte regardless of clock.running
     * (see midi_clock.c's own MIDI_REALTIME_CLOCK case) -- capture only
     * ever needs pulse_count progression for its own quantization, not
     * a belief about whether the DAW considers itself "playing," so
     * removing this check costs nothing here. */
    if (s_seq_pending_start[lane]) {
        /* Same nearest-boundary fix as seq_advance_clock()'s own pending-
         * start check above -- see that one's comment. */
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

    /* Commits the pending note only into the step it was actually
     * quantized to (s_seq_capture_target_step, set at touch time -- see
     * seq_capture_handle_taps()) -- a touch struck late in THIS step
     * targets the NEXT one instead, and stays pending (armed, untouched
     * here) until that boundary arrives.
     * Real feedback correcting this file's own prior assumption here:
     * "capture mode['s]... additive and accumulates. i[t] shouldnt just
     * override empty space. it a[d]ds whatever is being played on top
     * not cle[a]ring previous steps." A step nothing targeted THIS pass
     * is left completely untouched now -- whatever it already held
     * (from an earlier capture pass, or manually armed beforehand)
     * stays exactly as it was. Capturing a new melody over an existing
     * pattern only ever ADDS/overwrites the specific steps actually
     * played this time; it never silently erases everything else. */
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
    s_seq_current_step[lane] = (uint8_t)((s_seq_current_step[lane] + steps_to_advance) % length);
}

/* Real feedback: "the curent step of the sequencer should light up pink
 * sentia when the sequencer is at that step" -- reuses this file's own
 * established Sentia-magenta brand constants (OP_MENU_MELODIC_R/G/B,
 * the same ones the length-change flash uses), pulsing rather than
 * solid so it reads as "the playhead is here" rather than blending into
 * a plain selection state. Everything else follows the SAME root/
 * natural/off idle language melodic mode's own idle grid and this
 * file's per-step pitch-assignment view already use, so the pad-to-note
 * layout looks and feels identical to playing melodic normally -- the
 * whole point of "turns into the regular chromatic scale" is that
 * capture mode shouldn't feel like a different instrument.
 *
 * Later real feedback, once this could actually be tried: "it should
 * still display the pattern playing with the leds under the scale
 * melodic layout leds that will show like the moving sequencer will
 * appear and be red on enabeled steps." The moving playhead above was
 * already there; this adds the one thing it was missing -- an armed
 * (already-recorded) step, not currently the playhead and not
 * currently sounding, now shows OP_SEQ_DIM_RED_LEVEL, the exact same
 * dim red render_sequencer() already uses for "armed at rest" in the
 * normal step view (not reusing its own probability/ratchet tinting --
 * capture mode has no access to either while recording, so a plain,
 * untinted red is the honest answer here). Armed status now takes
 * priority over a step's own root/natural coloring: knowing "this step
 * already has something recorded" reads as more useful while actively
 * recording than that pad's scale role, which still matters for every
 * OTHER, not-yet-armed pad you might play next. */
static void render_seq_capture(uint32_t now_ms) {
    float pulse = menu_selected_pulse_level(now_ms);
    op_seq_pattern_t *pat = active_pattern();
    for (uint8_t pad = 1u; pad <= TILES_NUM_PADS; pad++) {
        /* Real feedback: "if the sequenfcer is reduced to 16 steps then
         * auto align the layout... 4x4 grid" -- see seq_step_for_pad()'s
         * own comment. Unlike render_sequencer()/seq_handle_step_taps(),
         * a pad outside the active grid in that layout still needs its
         * normal note coloring below (capture mode plays a note from
         * ANY of the 24 pads regardless of the step-grid layout) -- only
         * the step-cursor/armed highlighting is gated on has_step, not
         * skipped entirely. */
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

/* ---- Triangle (+ shift) click, diamond transport, top-level scan ------- */

/* Real feedback: "lets put the scale menu into the mode menu when
 * triangle plus shift pressed. freeing up diamond from everything for
 * now," briefly made universal ("make sure the shift scasle works on
 * chord melodic mode and on sequewndcer as well measning remove
 * whatever aux menu we had in sequencer mode"), then split by mode for
 * the pattern bank ("in sequencer mode shift plus triangle opens up the
 * pattern bajnk"), then unified again once the pattern bank moved to
 * shift+diamond instead ("i want shift plus diamond in sequencer only to
 * be the pattern selector" -- see handle_diamond_transport()'s own shift
 * branch): shift+triangle is now the scale picker UNCONDITIONALLY, no
 * per-mode branch at all -- ONE universal scale, melodic/chord/every
 * sequencer pattern alike (an earlier per-pattern-while-in-sequencer-mode
 * design got reversed -- see scale_menu_enter()'s own comment). A plain
 * solo click keeps its existing meaning (toggle the
 * top-level mode picker). While any per-step edit (pitch/probability/
 * ratchet) owns the grid, the shift gesture still cancels it with no
 * change instead of opening the scale picker on top of it -- the escape
 * hatch a toggle-style gesture needs (real feedback: "it should be a
 * toggle to set pitch of sequencer note, not a momentary thing").
 *
 * s_triangle_press_was_shift is edge-latched true the first time circle
 * is seen held during this triangle press (not re-checked fresh at
 * release) -- same "won't always release in the same tick" reasoning
 * s_triangle_press_had_conflict already relies on. Square joining too
 * escalates to a full conflict instead (game_mode.h's reserved 4-button
 * combo is SW3+SW4+SW5+SW6; triangle+circle+square held together is
 * three of those four, clearly progressing toward the secret combo, not
 * a genuine 2-button shift gesture) -- diamond joining already sets
 * s_triangle_press_had_conflict via the existing check below regardless
 * of shift, for the identical reason. */
static void handle_triangle_click(void) {
    bool held = tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID);
    bool circle_held = tiles_button_is_pressed(TILES_CIRCLE_BUTTON_ID);

    if (held && !s_triangle_was_held) {
        s_triangle_press_had_conflict = false;
        s_triangle_press_was_shift = false;
    }
    if (held && tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID)) {
        /* See this file's header + s_triangle_press_had_conflict's own
         * comment -- part of game_mode.h's reserved 4-button combo, not
         * a genuine solo triangle press. */
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
                    /* Real feedback: "i want shift plus diamond in
                     * sequencer only to be the pattern selector" moved
                     * the pattern bank off THIS button entirely (see
                     * handle_diamond_transport()'s own shift branch) --
                     * shift+triangle is now the scale picker everywhere,
                     * no per-mode branch needed at all, editing ONE
                     * universal scale regardless of s_active_mode (see
                     * scale_menu_enter()'s own comment). */
                    if (s_active_mode == OP_MODE_SEQUENCER && s_seq_edit_mode != OP_SEQ_EDIT_NONE) {
                        /* Still needs its own escape hatch -- real
                         * feedback: "it should be a toggle to set pitch
                         * of sequencer note, not a momentary thing."
                         * Checked first since a per-step edit owns the
                         * grid exclusively; opening the scale picker on
                         * top of it would be ambiguous. */
                        edit_exit();
                    } else if (s_seq_capture_mode_active) {
                        /* Defensive, same reachability gap as seq_
                         * capture_mode_enter()'s own comment about the
                         * pattern bank: nothing stops a fresh shift+
                         * triangle tap while a capture session is already
                         * running. Opening the scale picker on top would
                         * fight capture mode's own chromatic override for
                         * grid ownership -- exit capture mode instead of
                         * opening anything, the same "shift+triangle
                         * cancels whatever sequencer sub-state owns the
                         * grid" role this branch already plays for
                         * per-step edit just above. cross_capture_exit(),
                         * not the bare seq_capture_mode_exit(), when this
                         * is the cross-mode capture variant -- that one
                         * ALSO restores s_seq_edit_lane and clears
                         * s_cross_capture_active, neither of which the
                         * bare call touches; calling the wrong one here
                         * would leave both stuck pointing at lane 3. */
                        if (s_cross_capture_active) {
                            cross_capture_exit();
                        } else {
                            seq_capture_mode_exit();
                        }
                    } else if (s_scale_menu_visible) {
                        scale_menu_exit();
                    } else {
                        /* Real feedback: "make sure the shift scasle
                         * works on chord melodic mode and on
                         * sequewndcer as well" -- chord mode's own
                         * melody columns already read note_map.c's
                         * global scale setting the same way melodic's
                         * idle grid does (see this file's own "Chord
                         * mode" section), so picking a scale is exactly
                         * as meaningful from chord as it always was from
                         * melodic. */
                        scale_menu_enter();
                    }
                }
            } else if (s_menu_visible) {
                menu_exit();
            } else {
                /* Real feedback: "why does a click of triangle send to
                 * melodic mode? in other modes? it should just bring
                 * menu up." This used to force-jump straight back to
                 * melodic from any other active mode instead of opening
                 * the picker -- render_menu()'s own col_is_current_mode()
                 * check already correctly pulses whichever mode is
                 * ACTUALLY active right now regardless of what it is, so
                 * opening the menu from sequencer/chord/guitar works the
                 * identical way it always has from melodic; there was
                 * never a real need for the special case. */
                menu_enter();
            }
        }
    }

    s_triangle_was_held = held;
}

/* Diamond, freed from every menu-related duty above, as a dedicated DAW
 * transport remote instead -- real feedback: "the diamond for now will
 * play and stop in ableton like a toggle and stop brings back to the
 * start always. if we hold it for 2 sec it arms record and when we let
 * go it counts down metronome into record play," then, once the first
 * version (System Realtime Start/Stop only) turned out not to actually
 * do anything: "diamond is still not doing anything why is it not
 * sending transport controls to daw. look online for how other things
 * do that like the novation lounch key."
 *
 * Real feedback, later: "capture mode is triggered by diamond in
 * sequencer mode. transport controls disable on sequencer mode." The
 * DAW-transport-remote role below (CC sends, record-arm hold, the
 * 5-state LED language) is now ENTIRELY SUSPENDED while sequencer mode
 * is active -- diamond has a completely different job there instead:
 * plain click toggles sequencer capture mode (see this file's own
 * "Sequencer capture mode" section) for s_seq_edit_lane, no hold needed
 * at all (a previous round briefly hold-gated this against shift+
 * diamond's pattern-bank meaning, back when the two shared one release
 * condition -- no longer needed now that plain vs. shift alone cleanly
 * separates them). Shift+diamond in sequencer mode stays the pattern
 * bank toggle, unconditional on hold duration for the same reason.
 * Outside sequencer mode, diamond's DAW-transport role is fully back to
 * how it was: neither capture mode nor the pattern bank apply there at
 * all (sequencer-only concepts), so shift+diamond outside sequencer is
 * simply a no-op.
 *
 * That research changed the actual wire approach, not just the
 * troubleshooting: Ableton's own "Synchronizing via MIDI" docs confirm
 * System Realtime Start/Stop DO drive its transport, but only once that
 * MIDI input is genuinely in EXTERNAL SYNC (Preferences -> Link/MIDI,
 * Sync on for the port, AND Live's own transport-bar Ext button both
 * on) -- and external sync fundamentally means slaving to a continuous
 * MIDI Clock (0xF8) stream too, which this device has never sent (only
 * ever RECEIVED, for its own sequencer -- see services/midi_clock.h).
 * Isolated Start/Stop bytes with no clock behind them landing on a port
 * that was never fully in that state explains "not doing anything"
 * better than assuming the one-time Ext toggle was simply missed again.
 *
 * Checking how real hardware actually does this instead of guessing
 * again: a Novation Launchkey's own transport buttons "send MIDI
 * Control Change events on Channel 16" -- plain, mappable CCs, not
 * System Realtime bytes at all. Ableton's Play/Stop/Record ARE each
 * individually MIDI-mappable via generic Map Mode (Cmd/Ctrl+M -- "click
 * the parameter you want to map... press your MIDI controller button"),
 * and that same generic "MIDI learn" concept exists in effectively
 * every other DAW too (Cubase's MIDI Remote, Reaper's Action List MIDI
 * binding, etc.) -- unlike the Sync/Ext mechanism, this needs no clock
 * output, no per-DAW transport-specific feature, and no assumption
 * about what else is enabled. Play/Stop now each get their own
 * momentary CC trigger, the exact same shape OP_TRANSPORT_RECORD_CC
 * already used below -- three consistent, independently-mappable
 * triggers instead of one CC plus two special-case Realtime bytes. The
 * Realtime Start/Stop sends stay too (harmless, and still a real win on
 * the rarer setup that does have Sync/Ext genuinely engaged), but the
 * CC triggers are now the primary, verified-by-research path -- map
 * ALL THREE (Play, Stop, Record) via Ableton's Map Mode (or the
 * equivalent in whatever DAW is actually in use) for this to do
 * anything.
 *
 * Short click: toggles s_transport_playing, sending Stop-CC+Realtime-
 * Stop to go playing->stopped, or Play-CC+Realtime-Start the other way
 * (never Continue -- see tiles_midi_send_start()'s own comment in
 * midi_out.h for why that alone makes "stop brings back to the start
 * always" true on the Realtime path, for free).
 *
 * Held >= OP_TRANSPORT_RECORD_ARM_HOLD_MS: arms (s_diamond_record_armed,
 * edge-latched so it can only fire once per hold) -- LED starts
 * blinking (see render below), nothing sent yet. On release while
 * armed, instead of the short-click toggle: sends OP_TRANSPORT_RECORD_CC
 * once as a momentary trigger. Live's own Count-In preference
 * (Preferences -> Record/Warp/Launch) then handles "counts down
 * metronome into record play" automatically once Record engages --
 * nothing about counting beats needs to happen in firmware at all.
 * Recording implies playing, so s_transport_playing is set true here
 * too, same as a plain Start would leave it. */
#define OP_TRANSPORT_RECORD_ARM_HOLD_MS 2000u
/* Momentary CC triggers for Play/Stop/Record, each sent on the Zone
 * Master Channel as value 127 then immediately 0 (a clean on/off pair,
 * not a value left dangling at 127) -- see handle_diamond_transport()'s
 * own comment for the research behind using CCs at all instead of only
 * System Realtime bytes. 102/103/104 are drawn from the MIDI spec's own
 * "Undefined" generic-controller CC range (102-119) -- not a copy of
 * any specific real device's exact numbers (a Launchkey's own Play/
 * Stop/Record CC assignments weren't confirmed to this precision), just
 * the same CONVENTIONAL range real transport-control hardware already
 * draws from, chosen deliberately over arbitrary numbers for that
 * reason. CCs, not Note-Ons, specifically so a stray/unmapped receive
 * can never sound an actual note the way a Note-On on the Zone Master
 * Channel might on a receiver that isn't strictly MPE-aware. Each needs
 * a ONE-TIME manual MIDI-Map step in whatever DAW is actually in use
 * (Ableton: Key/MIDI Map Mode, Cmd/Ctrl+M, click the target transport
 * button, then trigger this device's matching gesture once) -- doing
 * this for only one of the three and assuming the others "should just
 * work" the same way is the most likely way this still reads as "not
 * doing anything" after this change too. */
#define OP_TRANSPORT_PLAY_CC 102u
#define OP_TRANSPORT_STOP_CC 103u
#define OP_TRANSPORT_RECORD_CC 104u

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
            /* Same "three of game_mode.h's four combo buttons held
             * together" escalation-to-conflict reasoning as triangle's
             * own shift detection -- see handle_triangle_click()'s own
             * comment. */
            s_diamond_press_had_conflict = true;
        } else {
            s_diamond_press_was_shift = true;
        }
    }
    /* Record-arm only applies OUTSIDE sequencer mode now -- real
     * feedback: "transport controls disable on sequencer mode" (see this
     * function's own header comment). */
    if (held && !sequencer_active && !s_diamond_press_had_conflict && !s_diamond_press_was_shift &&
        !s_diamond_record_armed && (now_ms - s_diamond_press_start_ms) >= OP_TRANSPORT_RECORD_ARM_HOLD_MS) {
        s_diamond_record_armed = true;
    }

    if (!held && s_diamond_was_held) {
        if (!s_diamond_press_had_conflict) {
            if (sequencer_active) {
                /* Real feedback: "for concistency i wanna swap in
                 * sequencer mode the diamond with shift to capture and
                 * the diamond alone to pattern selector." Swapped from
                 * the original "capture mode is triggered by diamond in
                 * sequencer mode... shift diamond does pattern picker"
                 * -- shift+diamond now means capture EVERYWHERE, matching
                 * the same gesture this file's own cross-mode "capture
                 * into lane 3" feature uses outside sequencer mode (see
                 * that feature's own section), instead of meaning
                 * capture only here and pattern-bank there. Plain click
                 * (no shift) is a simple toggle now -- no hold needed at
                 * all, since shift alone already cleanly separates this
                 * from capture above. */
                if (s_diamond_press_was_shift) {
                    if (s_seq_capture_mode_active) {
                        seq_capture_mode_exit();
                    } else if (OP_SEQ_CAPTURE_MODE_ENABLED &&
                               (tiles_midi_clock_tap_tempo_established() || tiles_midi_clock_external_active(now_ms))) {
                        /* Real gap caught auditing this: without this
                         * gate, capture mode could be entered with no
                         * tempo at all -- seq_capture_advance_clock()
                         * would then just sit inert forever (it needs
                         * clock.running, same as "+" already requires
                         * below), so live touches would audibly sound
                         * but NEVER actually commit into the pattern,
                         * with no indication anything was wrong. Same
                         * tempo-exists check "+" already uses one level
                         * up (see handle_transport_and_length()'s own
                         * sequencer branch) -- a diamond click is simply
                         * a no-op until a tempo genuinely exists, exactly
                         * like "+" already is. */
                        seq_capture_mode_enter();
                    }
                } else if (s_pattern_bank_visible) {
                    pattern_bank_exit();
                } else {
                    pattern_bank_enter();
                }
            } else if (s_diamond_press_was_shift) {
                /* Real feedback: "i also want to add a feature that
                 * captures from melodic mode or chord mode or any mode
                 * into lane 3 sequencer on command... for trigger
                 * capture mode lets use a push of shift and diamond if
                 * not in use already by another function." Shift+
                 * diamond outside sequencer mode was never its own
                 * distinct gesture before this -- it fell through to
                 * the exact same play/stop/record toggle a plain click
                 * already does (the record-ARM check above already
                 * excludes shift, but nothing stopped a quick shift+
                 * diamond CLICK, too short to arm, from still landing
                 * here) -- genuinely free to claim. See this file's own
                 * "Cross-mode capture into lane 3" section for
                 * cross_capture_enter()/_exit(). Same tempo-exists gate
                 * sequencer mode's own capture entry above already
                 * requires, for the identical reason: entering with no
                 * tempo at all would just sit inert forever. */
                if (s_cross_capture_active) {
                    cross_capture_exit();
                } else if (tiles_midi_clock_tap_tempo_established() || tiles_midi_clock_external_active(now_ms)) {
                    cross_capture_enter();
                }
            } else if (s_diamond_record_armed) {
                tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_RECORD_CC, 127u);
                tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_RECORD_CC, 0u);
                s_transport_playing = true;
                s_transport_recording = true;
            } else if (s_transport_playing || s_transport_recording) {
                /* A plain click always means "stop everything," matching
                 * a real transport's single Stop control -- stopping
                 * while recording doesn't leave recording somehow still
                 * armed in the background. CC first, then the Realtime
                 * byte -- see handle_diamond_transport()'s own comment
                 * for why the CC is the primary, verified path and the
                 * Realtime send is a harmless bonus for a Sync/Ext
                 * setup -- but ONLY that: real feedback found "the midi
                 * clock lights [in Ableton] not being in perfect sync
                 * from ableton internal clock and the clock its
                 * reciveing in return," and confirmed Ableton's own
                 * Track/Sync/Remote input is enabled on this same port
                 * (bidirectional) -- meaning Ableton is ALREADY the one
                 * driving the clock TILES is following (tiles_midi_
                 * clock_external_active()) exactly when this button
                 * would otherwise ALSO send it a raw Stop/Start byte on
                 * that identical port, a redundant, self-referential
                 * signal Ableton's own Sync input has no good reason to
                 * receive from a device it's already the master of.
                 * Skipped specifically then -- the CC (the primary,
                 * verified path, MIDI-Mapped rather than Sync-
                 * interpreted) still fully stops/starts Ableton's
                 * transport either way, so nothing is lost; this only
                 * removes the part that was confusing Ableton's own
                 * clock display, not the feature. Still sent normally
                 * whenever no external clock is active -- e.g. using
                 * this button to start Ableton from fully stopped,
                 * where TILES genuinely is the one initiating. */
                tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_STOP_CC, 127u);
                tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_STOP_CC, 0u);
                if (!tiles_midi_clock_external_active(now_ms)) {
                    tiles_midi_send_stop();
                }
                s_transport_playing = false;
                s_transport_recording = false;
            } else {
                tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_PLAY_CC, 127u);
                tiles_midi_send_cc(TILES_MIDI_MPE_MASTER_CHANNEL, OP_TRANSPORT_PLAY_CC, 0u);
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
        /* Real feedback: "the +- transport controls shoudl be the ones
         * with the flashing logic while in sequencer mode" -- the four-
         * state transport display that USED to live here moved to
         * render_sequencer()'s/render_transport_toggle_leds()'s own "-"/
         * "+" LEDs instead (see either one's own comment for the full
         * four-state mapping); diamond's own role in sequencer mode is
         * triggering capture/pattern-bank access now (see this
         * function's own diamond/shift-swap comment above), not passively
         * displaying transport state, so back to a plain "capture pulse
         * while active, dark otherwise" indicator -- no DAW-transport
         * states apply here at all, matching the pattern-bank branch's
         * own equally simple LED just above it. */
        float led_level = s_seq_capture_mode_active ? menu_selected_pulse_level(now_ms) : 0.0f;
        tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, led_level);
    } else {
        /* Four-state DAW-transport LED language -- real feedback: "armed
         * record and stopped is blik twice and pause then again, play is
         * on, stopped is off. record is pulsing in the same fashon as
         * the deep sleep for shift button." Checked most-specific-state-
         * first: armed (a transient hold-in-progress state) overrides
         * recording/playing; recording overrides playing (both can be
         * true at once -- recording implies playing, see s_transport_
         * recording's own comment -- and recording's own pulse is what
         * should show). Playing (bottom two cases) now reads tiles_midi_
         * clock_is_running() instead of s_transport_playing -- same "pull
         * from the real signal" fix as sequencer mode above, for the
         * same reason. s_transport_playing itself stays (still needed to
         * decide what a click sends -- Stop vs Play/Record -- which is
         * inherently about intent, not something a clock signal alone
         * could ever answer), just no longer drives this LED directly. */
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
        tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, led_level);
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
 * Also excludes any per-step edit view from `mode_ok` -- tapping a tempo
 * while mid-edit doesn't make sense anyway. (Used to also exclude the
 * pattern picker, for the same reason plus freeing up a plain circle
 * click there to mean something else instead -- both the picker and
 * that click's own meaning are gone now, see this file's own "Pattern/
 * channel picker: REMOVED" section.)
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
        bool mode_ok = (s_active_mode == OP_MODE_SEQUENCER && s_seq_edit_mode == OP_SEQ_EDIT_NONE);
        bool combo_conflict = tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_TRIANGLE_BUTTON_ID) ||
                               tiles_button_is_pressed(TILES_SQUARE_BUTTON_ID);
        s_circle_press_pending_tap = mode_ok && !combo_conflict && !tiles_midi_clock_external_active(now_ms);
    }

    if (held && s_circle_press_pending_tap &&
        (tiles_button_is_pressed(TILES_MINUS_BUTTON_ID) || tiles_button_is_pressed(TILES_PLUS_BUTTON_ID) ||
         tiles_button_is_pressed(TILES_DIAMOND_BUTTON_ID) || any_pad_touched())) {
        /* This hold became a length-adjust/ratchet-edit combo, or a
         * diamond combo (pattern bank or capture, whichever shift is
         * currently pointing at -- see handle_diamond_transport()'s own
         * comment on the swap) -- diamond joining mid-hold still means
         * a genuine 2-button combo is forming here, same "circle
         * pressed first, then the other button joins" ordering length-
         * adjust already needed this exact fix for. Cancel candidacy so
         * it doesn't ALSO register as a spurious tap. */
        s_circle_press_pending_tap = false;
    }

    if (!held && s_circle_was_held) {
        /* Real feedback: "capture mode is triggered by diamond in
         * sequencer mode" -- circle/shift is no longer part of capture
         * mode's own entry or exit at all (a previous round's "exit via
         * shift or diamond" convenience only made sense back when shift+
         * diamond WAS the entry combo); a solo circle release just
         * registers a tap-tempo tap like normal now, same as any other
         * time. */
        if (s_circle_press_pending_tap) {
            tiles_midi_clock_register_tap(s_circle_press_ms);
        }
        /* A plain-circle-click-while-the-pattern-picker-was-open gesture
         * used to live here too (toggled a pattern's probability_enabled
         * master switch) -- removed along with the picker itself, see
         * this file's own "Pattern/channel picker: REMOVED" section;
         * that setting has no other access point right now. */
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
    /* Also excludes capture mode now -- found auditing this round's
     * changes: capture mode already owns s_seq_edit_lane exclusively via
     * its own seq_capture_advance_clock() (see that function and this
     * file's own dispatch loop), and it ALSO happens to consume s_seq_
     * pending_start/_restart, the exact same fields "+" writes to below
     * -- an in-capture "+" press would have silently restarted the live
     * recording from step 0 mid-take instead of doing nothing or
     * something clearly intentional. Same "exclusive sub-state suspends
     * transport/length" treatment per-step edit already gets. */
    bool active =
        (s_active_mode == OP_MODE_SEQUENCER) && s_seq_edit_mode == OP_SEQ_EDIT_NONE && !s_seq_capture_mode_active;
    bool guitar_active = (s_active_mode == OP_MODE_GUITAR);

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
            /* Real feedback: "we need a button that starts and stops
             * sequencer... play position of head should reset when stop
             * click twice," later refined: "play and stop are
             * independent per active pattern. the only thing global is
             * tap tempo or midi tempo." Operates on s_seq_edit_lane only
             * -- s_seq_lane_running[edit_lane] (not the shared clock)
             * decides "stop while playing" (pause in place) from "stop
             * while ALREADY stopped" (a second stop -- Ableton-style
             * rewind to the top, without resuming playback), scoped to
             * THIS lane; the other 3 are untouched either way. */
            uint8_t lane = s_seq_edit_lane;
            if (s_seq_lane_running[lane]) {
                s_seq_lane_running[lane] = false;
                s_seq_pending_start[lane] = false;
                /* Only turns off the shared pulse_count once EVERY lane
                 * has stopped -- see s_seq_lane_running's own comment. */
                if (!any_lane_running()) {
                    tiles_midi_clock_set_running(false);
                }
            } else {
                s_seq_current_step[lane] = 0u;
                s_seq_note_sounding[lane] = false;
                s_seq_pending_start[lane] = false;
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
            uint8_t lane = s_seq_edit_lane;
            if (s_seq_lane_running[lane]) {
                /* Real feedback: "if playing and play again it starts
                 * from the top again" -- a retrigger, not a no-op.
                 * Re-arming pending-start quantizes the restart to the
                 * next beat boundary exactly like a fresh start would
                 * (see seq_advance_clock()), rather than snapping to step
                 * 0 mid-beat. pending_restart=true is what makes this
                 * land back on step 0 rather than replaying wherever the
                 * playhead already was. Scoped to THIS lane. */
                s_seq_pending_start[lane] = true;
                s_seq_pending_restart[lane] = true;
            } else if (tiles_midi_clock_tap_tempo_established() || tiles_midi_clock_external_active(now_ms)) {
                /* Only meaningful once a tempo actually exists --
                 * otherwise this would set this lane running with
                 * nothing to ever advance pulse_count, an inert state
                 * rather than "playing." s_seq_pending_start quantizes
                 * the resume to the next beat boundary instead of fast-
                 * forwarding through however many pulses accumulated
                 * while stopped (pulse_count keeps advancing even while
                 * !running -- see midi_clock.h's own header).
                 * pending_restart=false: real feedback: "when stopped
                 * makes play" -- resumes exactly where a plain stop left
                 * the playhead (or step 0, if a double-stop rewound it
                 * first), never resetting position on its own. Sets the
                 * SHARED clock running too (harmless no-op if some other
                 * lane already had it running) -- see s_seq_lane_
                 * running's own comment for why that's still needed
                 * alongside this lane's own flag. */
                s_seq_lane_running[lane] = true;
                tiles_midi_clock_set_running(true);
                s_seq_pending_start[lane] = true;
                s_seq_pending_restart[lane] = false;
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

void tiles_op_mode_init(bool crash_recovered) {
    if (!crash_recovered) {
        s_active_mode = OP_MODE_MELODIC;
    }
    s_menu_visible = false;
    s_triangle_was_held = false;
    s_triangle_press_had_conflict = false;
    s_triangle_press_was_shift = false;
    s_scale_menu_visible = false;
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
        /* Claims from the TOP of the 15 MPE Member Channels downward --
         * see this file's own "Multi-lane pattern bank" section. Lane 0
         * = nibble 15, exactly today's original single-pattern behavior,
         * unchanged for anyone never touching the bank. */
        s_seq_lane_channel[lane] =
            (uint8_t)(TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL + TILES_MIDI_MPE_NUM_MEMBER_CHANNELS - 1u - lane);
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
    /* See handle_diamond_transport()'s own comment -- diamond's LED is
     * now a persistent transport-state indicator, not a "follows press"
     * default, same override mechanism triangle already uses above. */
    tiles_buttons_set_override_active(TILES_DIAMOND_BUTTON_ID, true);
    tiles_buttons_set_override_led(TILES_DIAMOND_BUTTON_ID, OP_TRANSPORT_LED_STOPPED_LEVEL);
    s_boot_relight_guard_until_ms = to_ms_since_boot(get_absolute_time()) + OP_BOOT_RELIGHT_GUARD_MS;
    /* Must run after every pattern slot above has already been reset to
     * its normal fresh-boot default -- only ever overwrites a slot
     * flash itself claims to own; see pattern_store_load_all()'s own
     * comment. A plain read, not a write, so unlike saving/clearing
     * this needs none of that path's interrupt-disable/watchdog care --
     * safe and cheap to do unconditionally on every boot, crash-
     * recovery included. */
    pattern_store_load_all();

    if (crash_recovered) {
        /* s_active_mode/s_seq_active_alt[]/s_seq_lane_running[] were all
         * deliberately left untouched above -- see their own
         * __uninitialized_ram declarations for why they already hold
         * whatever they were the instant before the crash. Replaying
         * set_active_mode() with the mode already in place re-syncs
         * note_map.c's guitar/chord flags and the standby/lighting
         * ownership that go with it -- that function's own "!=" guards
         * correctly no-op every "leaving" branch and only run the
         * "entering" ones when old and new are the same value, so this
         * is safe to call here even though nothing is actually being
         * left. */
        set_active_mode(s_active_mode);
        for (uint8_t lane = 0; lane < OP_SEQ_NUM_LANES; lane++) {
            if (s_seq_lane_running[lane]) {
                /* Mirrors the transition handle_transport_and_length()
                 * makes on a live stopped->running edge (see s_seq_lane_
                 * running[]'s own declaration comment) -- restoring the
                 * per-lane flag alone doesn't do this, and without it
                 * tiles_midi_clock_is_running() stays false forever
                 * (that flag is an ordinary static, not persisted the
                 * same way), leaving this lane silently stuck waiting
                 * for a clock source that already thinks nothing wants
                 * it, instead of actually resuming playback. */
                tiles_midi_clock_set_running(true);
                break;
            }
        }
    }
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
         * scan while frozen here is harmless. Loops all OP_SEQ_NUM_LANES
         * now that each has its own independent sounding-note state. */
        for (uint8_t lane = 0u; lane < OP_SEQ_NUM_LANES; lane++) {
            if (s_seq_note_sounding[lane]) {
                seq_end_current_note(lane);
            }
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
    handle_diamond_transport(now_ms);
    handle_circle_tap(now_ms);
    handle_transport_and_length(now_ms);

    /* Fetched exactly once per scan -- tiles_midi_clock_get_state()
     * consumes start_edge as a side effect (see midi_clock.h's own
     * comment), and both the sequencer's own clock-advance below and the
     * beat-flash computation need to see the SAME snapshot. */
    tiles_midi_clock_state_t clock = tiles_midi_clock_get_state();
    float beat_flash_level = compute_beat_flash_level(now_ms, clock);

    /* Real feedback: "tap tempo should auto triggere the current
     * pattern." tiles_midi_clock_register_tap() (midi_clock.c) already
     * autostarts the shared clock/transport itself on the first 4-tap
     * establishment ("tap tempo should autostart sequence when 4 taps
     * detected even if stopped") -- but that's the CLOCK, not any
     * specific lane; a lane only ever starts via its own explicit "+"
     * (see handle_transport_and_length() above) or a fresh sequencer-
     * mode entry (seq_start()). Closes that last gap for the lane
     * actually being looked at: the same start_edge a real external
     * Start message would also produce (clock.start_edge, consumed
     * once per scan same as always) fires this specifically when it's
     * tap-tempo-sourced (clock.source_is_tap_tempo) -- deliberately
     * NOT extended to a real external Start too, since that's not what
     * was asked for and a DAW's own Start already has its own separate
     * transport meaning elsewhere in this file (handle_diamond_
     * transport()) that this shouldn't second-guess. Mirrors "+"'s own
     * fresh-start sequence above exactly (same 4 fields, same order)
     * rather than inventing a slightly different one. Gated on
     * sequencer mode even though handle_circle_tap() already only ever
     * registers a tap while sequencer mode is active -- the tap and
     * this edge landing can straddle a mode switch in principle, and
     * this should never fire for whichever mode the player has since
     * moved to. */
    if (s_active_mode == OP_MODE_SEQUENCER && clock.start_edge && clock.source_is_tap_tempo &&
        !s_seq_lane_running[s_seq_edit_lane]) {
        s_seq_lane_running[s_seq_edit_lane] = true;
        tiles_midi_clock_set_running(true);
        s_seq_pending_start[s_seq_edit_lane] = true;
        s_seq_pending_restart[s_seq_edit_lane] = false;
    }

    /* Real feedback: "ok we have clashing issues on modes, mode selectro
     * shouldnt pause sequencer... no stopping on playing sequences
     * regarless of manu displayed." A pattern already running in the
     * background must keep advancing regardless of which sub-view
     * currently owns the grid (top-level menu, scale menu, pattern bank,
     * or per-step edit) -- not just regardless of which TOP-LEVEL MODE is
     * displayed, which a previous round already fixed (see this file's
     * own "sequencer should not stop if mode is changed" section below).
     * Every one of those sub-views used to `return` before ever reaching
     * seq_advance_clock() at the bottom of this function, silently
     * freezing the playhead for as long as they stayed open -- moving the
     * call up here, unconditional, closes that gap for all four at once.
     * Loops all OP_SEQ_NUM_LANES -- every lane plays independently and
     * simultaneously now (see this file's own "Multi-lane pattern bank"
     * section). Capture mode is the one genuine exception, and only for
     * the ONE lane it currently owns (s_seq_edit_lane): it replaces that
     * lane's advance entirely with its own seq_capture_advance_clock()
     * (see that branch below), so the two must never both run for the
     * SAME lane in the same scan -- the other 3 lanes are unaffected and
     * keep advancing normally right through it. */
    for (uint8_t lane = 0u; lane < OP_SEQ_NUM_LANES; lane++) {
        if (s_seq_capture_mode_active && lane == s_seq_edit_lane) {
            continue;
        }
        /* Real feedback: "it might be the sequencer triggering notes or
         * clock or running" -- debug mode's own top suspect, so it gets
         * finer-grained tracing than the rest of this file: which LANE
         * (0-3) was being advanced, not just that op_mode_scan() as a
         * whole was running (see main.c's own 'S' marker). See services/
         * debug_mode.h for the full scheme. */
        tiles_debug_trace((char)('0' + lane));
        seq_advance_clock(lane, clock);
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
        /* Cross-mode capture (see that feature's own section) deliberately
         * does NOT return here, unlike sequencer mode's own plain-diamond
         * capture just below -- the whole point is that the current
         * mode's own grid stays fully visible/playable underneath, so
         * control falls through to the normal per-mode dispatch a few
         * lines down exactly as if this whole block hadn't run at all;
         * only the underglow (services/lighting.c's write_cross_
         * capture_underglow(), gated on tiles_op_mode_cross_capture_
         * is_active()) shows anything different. */
        if (!s_cross_capture_active) {
            /* Own dispatch branch, not routed through the normal
             * sequencer playback path -- see this file's own
             * "Sequencer capture mode" section for why it needs its own
             * advance/render entirely rather than reusing seq_advance_
             * clock()/render_sequencer(). */
            render_seq_capture(now_ms);
            return;
        }
    }

    if (s_active_mode == OP_MODE_SEQUENCER) {
        seq_handle_step_taps(now_ms);
        render_sequencer(beat_flash_level, clock.running);
    } else {
        /* Real feedback: "make trisngle fhash if pattern is playing and
         * we exit to a different screen than the playing pattern."
         * render_sequencer() above already shows the moving playhead
         * directly on the grid, so a background pattern is already
         * visible there -- this is only needed for every OTHER mode,
         * where nothing else on the board hints that a pattern is
         * still audibly running behind whatever's actually displayed.
         * Originally a hard on/off blink (this file's own "flash"
         * language, matching the pattern bank's own flashing red/white
         * cells) -- real feedback after trying it: "the flashing of
         * triangel is too fast for when sequencer is in the background
         * it should be a pulsing like the deep sleep pulse." Reworked
         * to a smooth sine pulse at services/standby.c's own deep-sleep
         * pacing (DEEP_SLEEP_PULSE_PERIOD_MS, 3000ms -- not shared code,
         * same "not shared, just the same convention" precedent this
         * codebase already uses for its OTHER pulse shapes, since that
         * one's own min/max are tuned for a barely-visible IDLE pad, not
         * a button meant to actually draw attention) -- deliberately
         * still not menu_selected_pulse_level()'s own faster, brighter
         * 900ms pulse, which means "this is the active/selected thing
         * right now"; a background pattern is a calmer, lower-urgency
         * signal than that. */
        float level = any_lane_running() ? background_pattern_pulse_level(now_ms) : 0.0f;
        tiles_buttons_set_override_led(TILES_TRIANGLE_BUTTON_ID, level);
    }
    /* See tiles_op_mode_owns_pad()'s own comment -- this pipeline
     * bypasses that accessor and services/expression.c's gate entirely,
     * so it needs its own explicit menu check rather than inheriting
     * one; skipped outright while either menu owns the grid instead of
     * letting a touch on a chord pad commit a real chord underneath it. */
    if (s_active_mode == OP_MODE_CHORD && !s_menu_visible && !s_scale_menu_visible) {
        handle_chord_pad_taps(now_ms);
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
 * chord-strip pads instead of claiming (or releasing) the whole grid.
 * Real feedback: "selecting a scale should not trigger midi sound when
 * slecting so no midi on select scale just menu input." Root cause,
 * chord-mode-specific: the narrowing below used to apply UNCONDITIONALLY
 * whenever chord was active, answering purely from chord-region
 * membership with no regard for whether a menu (scale or mode picker)
 * was ALSO currently open on top of it -- unlike every other mode, which
 * falls straight through to tiles_op_mode_owns_pad_grid() and so
 * already correctly suppressed new strikes the instant either menu's
 * own visible flag went true. Opening the scale picker while chord was
 * the active mode meant its answer for a melody pad never changed at
 * all, and handle_chord_pad_taps() (op_mode.c's own separate chord-pad
 * pipeline, which bypasses this accessor and services/expression.c's
 * gate entirely) never checked menu state either -- both the 16 melody
 * pads AND the 8 chord pads could still fire real notes underneath
 * either menu. Now checks menu state FIRST, before the chord-specific
 * narrowing -- while a menu is open every pad defers to the blanket
 * grid answer (true, suppressing all of them), exactly matching every
 * other mode's own behavior; the chord-specific narrowing only applies
 * once nothing is layered on top, unchanged from before for that case. */
bool tiles_op_mode_owns_pad(uint8_t logical_pad) {
    if (s_active_mode == OP_MODE_CHORD && !s_menu_visible && !s_scale_menu_visible) {
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
    /* Real feedback: "is there anything needed to stop stuck niotes?"
     * -- while investigating that, found a related gap: this used to
     * only check s_active_mode, but since seq_advance_clock() now runs
     * every scan regardless of which mode is DISPLAYED (see this file's
     * own "sequencer should not stop if mode is changed" fix), a
     * pattern can genuinely be playing in the background while a
     * DIFFERENT mode is active -- and services/standby.h's own longer
     * sequencer idle timeout (real feedback: "sleep screensaver should
     * be set to 20 minute in sequencer mode since its a more stratic
     * thing") would silently revert to the shorter default the instant
     * the display switched away, even with a pattern still audibly
     * running. Checking tiles_midi_clock_is_running() too closes that
     * gap -- not a stuck-note bug itself (the safety net in
     * tiles_op_mode_scan()'s own other_feature_owns_input() branch
     * still correctly ends whatever's sounding the moment standby DOES
     * engage), but directly related: a background pattern now keeps
     * its own longer runway before that engages at all. Checks any_lane_
     * running() now, not tiles_midi_clock_is_running() -- real feedback:
     * "play and stop are independent per active pattern." An external
     * clock can be present and ticking with every lane still individually
     * stopped (nothing started via "+" yet); this function's own point is
     * "is a pattern genuinely audible in the background," which any_lane_
     * running() answers directly instead of through the shared clock's
     * derived flag. */
    return s_active_mode == OP_MODE_SEQUENCER || any_lane_running();
}

bool tiles_op_mode_has_menu_open(void) {
    /* Real gap found auditing this round's changes: sequencer capture
     * mode can sit genuinely armed with no touch input at all for a
     * while (entered, quantized-start still pending because no tempo
     * exists yet, or the player just hasn't started playing) -- exactly
     * the same "reading/setting up takes no touch" situation every other
     * sub-view here already protects against standby's idle timeout for.
     * Without this, the screensaver could pop up over an armed-but-not-
     * yet-playing capture session. */
    return s_menu_visible || s_scale_menu_visible || s_pattern_bank_visible || s_seq_edit_mode != OP_SEQ_EDIT_NONE ||
           s_seq_capture_mode_active;
}

/* Real feedback: "is there anything needed to stop stuck niotes?" --
 * investigated the one real gap: services/expression.c's live-touch MPE
 * channel allocator (claim_mpe_channel()) and this file's own per-lane
 * channel assignment are two independent systems that don't know about
 * each other. Since seq_advance_clock() can now genuinely fire notes on
 * any of OP_SEQ_NUM_LANES channels WHILE a different mode is displayed
 * and live melodic touches are ALSO claiming channels from the same 1-15
 * pool, a live touch claiming the exact channel a lane is using for its
 * own background note would desync both sides -- that lane's own next
 * seq_fire_note() would end/steal whatever the live touch put there
 * without expression.c ever knowing, and that pad's own state machine
 * would still believe it owns a note that's already gone, never able to
 * send its own eventual note-off (the actual stuck-note failure mode).
 * A query rather than a single return value -- once all 4 lanes can be
 * simultaneously reserved (not just one pattern's worth), "the reserved
 * channel" stopped being a single number; claim_mpe_channel() asks this
 * once per CANDIDATE channel instead. Checks each lane's OWN s_seq_lane_
 * running flag now, not just whether the shared clock is ticking at all
 * -- real feedback: "play and stop are independent per active pattern."
 * A STOPPED lane can't have a note sounding (seq_advance_clock() ends it
 * the instant that lane stops, see that function's own comment) and
 * won't fire a new one, so its channel is genuinely free for live touch
 * to use -- reserving it anyway would just shrink live polyphony for no
 * real reason. */
bool tiles_op_mode_sequencer_channel_is_reserved(uint8_t channel) {
    for (uint8_t lane = 0u; lane < OP_SEQ_NUM_LANES; lane++) {
        if (s_seq_lane_running[lane] && s_seq_lane_channel[lane] == channel) {
            return true;
        }
    }
    return false;
}
