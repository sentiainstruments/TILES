#pragma once

/*
 * Touch + Hall fusion: derives real MIDI velocity from elapsed time to
 * reach a real press (see expression.c's "Velocity: elapsed-time-to-
 * actuation" section for why this isn't acceleration-based), ongoing
 * aftertouch from press depth while held, and optional pitch bend from
 * sideways (Hall X) motion while held (see expression.c's "Pitch bend
 * from sideways motion" section). Touch remains the authoritative gate
 * for note-on/off timing (capacitive contact is more reliable to detect
 * than trying to infer press/release purely from Hall depth); Hall
 * supplies the expressive data layered on top of it -- matches the
 * hardware handoff's own framing: "Touch should be used as a state and
 * intention signal, not as the only pressure measurement. Hall and
 * touch data should be fused."
 *
 * Per-pad state machine:
 *   IDLE            -- touch rising edge --> AWAITING_STRIKE (+ a light
 *                       touch-only haptic pulse, independent of whether
 *                       this becomes a real press -- see
 *                       services/haptics.h)
 *   AWAITING_STRIKE  -- measured depth crosses the real-press threshold
 *                       --> commit: fire note-on (+ a haptic kick, same
 *                       velocity value -- see services/haptics.h) -->
 *                       NOTE_ON, and claim pitch bend ownership if
 *                       enabled
 *                    -- touch released before committing --> IDLE
 *                       (cancelled -- a light tap that never became a
 *                       real press never sends a note)
 *   NOTE_ON          -- touch falling edge --> note-off (+ haptic hard
 *                       stop, + release pitch bend ownership if held)
 *                       --> IDLE
 *                    -- depth easing back near true rest --> retrigger:
 *                       note-off then straight back into AWAITING_STRIKE
 *                       without touch itself ever going false
 *                    -- while held: poly aftertouch on a meaningful
 *                       depth change (+ the same value drives haptic
 *                       sustain intensity), and pitch bend if this pad
 *                       is the current owner
 *
 * V1 caveat, stated plainly in expression.c: several constants
 * (aftertouch's depth range, velocity's timing bounds, pitch bend's
 * sensitivity) are first-attempt or real-data-informed estimates, not
 * fully measured against real hardware -- see each section's own notes
 * on what would refine them.
 */

#include <stdbool.h>
#include <stdint.h>

void tiles_expression_init(void);

/* Runs the per-pad state machine described above for every pad. Call
 * every main-loop iteration, after tiles_touch_scan() and
 * tiles_hall_scan() so both have fresh data this iteration. New strikes
 * (IDLE -> AWAITING_STRIKE) are suppressed for as long as
 * services/expression_control.h's sub-menu owns the pad grid (see
 * tiles_expression_control_owns_pad_grid()), so a slider tap there never
 * also fires a MIDI note underneath -- a pad already mid-strike or held
 * when the sub-menu opens is left alone to finish normally rather than
 * being cut off. */
void tiles_expression_scan(void);

/* Called by services/expression_control.h on a genuine square ("sentia")
 * short click -- see expression.c's "Pitch bend from sideways motion"
 * section. Turning it off while a note currently owns the bend resets to
 * center immediately rather than leaving that note stuck bent. */
void tiles_expression_toggle_pitch_bend(void);

/* Current pitch-bend-enabled state, for services/expression_control.h to
 * drive the square button's persistent toggle-state LED glow. */
bool tiles_expression_is_pitch_bend_enabled(void);

/* Runtime sensitivity setters for services/expression_control.h's
 * expression sub-menu (rows 2 and 4) -- replace what used to be fixed
 * expression.c compile-time constants (PITCH_BEND_MAX_COSINE_DEVIATION,
 * DEPTH_TO_AFTERTOUCH_FULL_SCALE) so a pad tap in the sub-menu can adjust
 * them live. Both default to exactly their old fixed values (0.15f,
 * 900u) until changed -- see expression.c's own section comments for
 * what each value means and why those particular defaults were chosen. */
void tiles_expression_set_pitch_bend_sensitivity(float max_cosine_deviation);
void tiles_expression_set_aftertouch_sensitivity(uint16_t depth_full_scale);

/* Called by services/expression_control.h when "expression mute" (the
 * circle+square 3-second combo hold) toggles on/off. While muted, pitch
 * bend and poly aftertouch both stop being computed/sent -- if a note
 * currently owns pitch bend, it's reset to center immediately, the same
 * "never leave a note stuck bent" rule tiles_expression_toggle_pitch_bend
 * already follows. Note-on/off and velocity are NOT affected -- basic
 * MIDI keeps working while muted, only the expressive layer stops. */
void tiles_expression_set_muted(bool muted);
