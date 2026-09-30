#pragma once

/* Touch + Hall fusion: turns pad contact and magnet motion into notes and
 * expression. Touch is the gate for note timing (contact is more reliable
 * than inferring press/release from depth); Hall supplies velocity (time
 * to reach a real press; see expression.c "Velocity"), pressure from depth
 * while held, and optional pitch bend from sideways tilt. As the hardware
 * handoff puts it: touch is a state and intention signal, fused with Hall.
 *
 * Per-pad state machine:
 *   IDLE             -- touch starts --> AWAITING_STRIKE (+ a light touch
 *                       haptic pulse, services/haptics.h)
 *   AWAITING_STRIKE  -- depth crosses the press threshold --> note-on (+ a
 *                       haptic kick at the same velocity) --> NOTE_ON
 *                    -- touch ends first --> IDLE (a light tap never plays)
 *   NOTE_ON          -- touch ends --> note-off (+ haptic stop) --> IDLE
 *                    -- depth back near rest while still touched -->
 *                       retrigger: note-off, then AWAITING_STRIKE again
 *                    -- while held: pressure on meaningful depth changes
 *                       (also drives haptic sustain), and pitch bend
 *
 * Several constants (pressure range, velocity timing, bend sensitivity)
 * are estimates informed by captured data, not final measurements; each
 * section in expression.c says what would refine it. */

#include <stdbool.h>
#include <stdint.h>

void tiles_expression_init(void);

/* Runs every pad's state machine. Call every main-loop pass, after
 * tiles_touch_scan() and tiles_hall_scan(). No NEW strikes start while
 * the expression menu owns the grid
 * (tiles_expression_control_owns_pad_grid()); notes already sounding
 * finish normally. */
void tiles_expression_scan(void);

/* Toggles pitch bend (square click, services/expression_control.h).
 * Turning it off recenters any note currently bent. */
void tiles_expression_toggle_pitch_bend(void);

/* For square's pitch-bend LED. */
bool tiles_expression_is_pitch_bend_enabled(void);

/* MPE on (default): each note gets its own Member Channel, so bend and
 * pressure are per-note. MPE off: the most compatible plain-MIDI layout,
 * every note on channel 1 with channel-wide bend and pressure like a bend
 * wheel; the most recently struck held pad drives them, handing back to
 * an older held pad when it releases (s_non_mpe_owner_pad). Setting
 * `expression.mpe_enabled`, toggled on the device by holding circle +
 * square. See the definition for what happens to notes already held. */
void tiles_expression_set_mpe_enabled(bool enabled);
bool tiles_expression_is_mpe_enabled(void);

/* Sends the MPE zone declaration for the current setting: the Lower Zone
 * at its current size plus the bend range while MPE is on, a withdrawn
 * zone (0 members) while off. Called at boot after settings load, on every
 * USB mount, when MPE is toggled, and for the deferred re-declaration in
 * scan. Nothing else sends an MPE Configuration Message. */
void tiles_expression_announce_mpe_zone(void);

/* Sensitivities, set by the expression menu (rows 2 and 4) and the
 * settings table. Defaults and meaning: see expression.c. */
void tiles_expression_set_pitch_bend_sensitivity(float max_cosine_deviation);
float tiles_expression_get_pitch_bend_sensitivity(void);
void tiles_expression_set_aftertouch_sensitivity(uint16_t depth_full_scale);
uint16_t tiles_expression_get_aftertouch_sensitivity(void);

/* Melodic harmonics (hold one note, lightly touch other pads to pluck its
 * harmonics; expression.c "Melodic harmonics"). Setting
 * `features.melodic_harmonics`, default on; turning it off ends any
 * harmonic voices sounding. */
void tiles_expression_set_melodic_harmonics_enabled(bool enabled);
bool tiles_expression_is_melodic_harmonics_enabled(void);

/* Chord-vs-harmonic tuning (settings `features.harmonics.*`), so chords
 * and harmonics can share a session; see HARMONIC_ARM_MS_DEFAULT. */
void tiles_expression_set_harmonics_arm_ms(uint16_t ms);
uint16_t tiles_expression_get_harmonics_arm_ms(void);
void tiles_expression_set_harmonics_confirm_ms(uint16_t ms);
uint16_t tiles_expression_get_harmonics_confirm_ms(void);
void tiles_expression_set_harmonics_press_depth(uint16_t depth);
uint16_t tiles_expression_get_harmonics_press_depth(void);

/* Ends every note and returns every pad to IDLE. Called when game mode
 * starts: a pad caught mid-strike then is incidental contact (both hands
 * are on the combo), and left alone it could still commit a note and a
 * haptic kick mid-game from vibration. Other grid owners (menus) leave
 * in-flight notes alone because those are usually deliberate. */
void tiles_expression_force_release_all(void);

/* The velocity curve, shared with op_mode.c's chord pads (which bypass
 * this file's state machine) so both use one tuned curve.
 * `strike_time_ms`: time from touch-down to the sample where the depth
 * first reached TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA (travel speed, not
 * hold time). `peak_depth`: the highest depth seen up to then (a strike
 * can spring back). Both must be measured against that threshold; the
 * curve is calibrated to it. */
uint8_t tiles_expression_velocity_from_strike(uint32_t strike_time_ms, float peak_depth);

/* The strike threshold the velocity curve is calibrated against. The
 * single source of truth: expression.c's MIN_STRIKE_DEPTH_DELTA is defined
 * from it. */
#define TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA 150.0f
