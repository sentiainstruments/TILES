#pragma once

/*
 * Touch + Hall fusion: derives real MIDI velocity from the initial
 * strike's acceleration, and ongoing aftertouch from press depth while
 * held. Touch remains the authoritative gate for note-on/off timing
 * (capacitive contact is more reliable to detect than trying to infer
 * press/release purely from Hall depth); Hall supplies the expressive
 * data layered on top of it -- matches the hardware handoff's own
 * framing: "Touch should be used as a state and intention signal, not
 * as the only pressure measurement. Hall and touch data should be
 * fused."
 *
 * Per-pad state machine:
 *   IDLE            -- touch rising edge --> AWAITING_STRIKE
 *   AWAITING_STRIKE  -- enough Hall samples + min window elapsed, or
 *                       max window elapsed --> commit: fire note-on
 *                       (+ a haptic kick, same velocity value -- see
 *                       services/haptics.h) with velocity from peak
 *                       acceleration observed so far --> NOTE_ON
 *                    -- touch released before committing --> IDLE
 *                       (cancelled -- a light tap that never became a
 *                       real press never sends a note)
 *   NOTE_ON          -- touch falling edge --> note-off (+ haptic hard
 *                       stop) --> IDLE
 *                    -- while held: poly aftertouch on a meaningful
 *                       depth change (+ the same value drives haptic
 *                       sustain intensity)
 *
 * V1 caveat, stated plainly in expression.c: the acceleration->velocity
 * scale and the depth->aftertouch range are placeholder estimates, not
 * measured against real hardware -- there is no calibrated mT/LSB
 * relationship yet (see hall.h), so there's nothing to derive them
 * from yet. Expect to retune both once this can actually be played.
 */

void tiles_expression_init(void);

/* Runs the per-pad state machine described above for every pad. Call
 * every main-loop iteration, after tiles_touch_scan() and
 * tiles_hall_scan() so both have fresh data this iteration. */
void tiles_expression_scan(void);
