#pragma once

/* Touch + Hall fusion: touch gates note timing; Hall gives velocity, pressure
 * and tilt bend. Per pad:
 *   IDLE --touch--> AWAITING_STRIKE (touch tick)
 *   AWAITING_STRIKE --depth past threshold--> NOTE_ON (kick); --lift--> IDLE
 *   NOTE_ON --lift--> IDLE; --depth back near rest--> retrigger
 * Several constants are estimates from captured data (see expression.c). */

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

/* MPE on (default): a Member Channel per note. Off: every note on channel
 * 1, the latest held pad driving bend and pressure. Setting
 * expression.mpe_enabled; circle + square on the device. */
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

/* How far pad 1-24's sounding note is bent: 0 (centred, no note, or bend
 * off) to 1 (the pad's full bend, before the wire-range scaling), vibrato
 * included. For the LEDs' bend dimming (lighting.c). */
float tiles_expression_get_pad_bend_amount(uint8_t logical_pad);

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

/* The velocity curve, shared with op_mode.c's chord pads. strike_time_ms:
 * touch-down to the threshold crossing; peak_depth: the highest depth until
 * then. Both measured against TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA. */
uint8_t tiles_expression_velocity_from_strike(uint32_t strike_time_ms, float peak_depth);

/* The strike threshold the velocity curve is calibrated against. The
 * single source of truth: expression.c's MIN_STRIKE_DEPTH_DELTA is defined
 * from it. */
#define TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA 150.0f
/* Overshoot past it (by the end of the follow-through) at which the depth
 * signal maxes out; a strike this deep has nothing left to measure
 * (strike_settle.h). Unmeasured. */
#define TILES_EXPRESSION_STRIKE_OVERSHOOT_FULL_SCALE 550.0f
#define TILES_EXPRESSION_FULL_STRIKE_DEPTH \
    (TILES_EXPRESSION_MIN_STRIKE_DEPTH_DELTA + TILES_EXPRESSION_STRIKE_OVERSHOOT_FULL_SCALE)
