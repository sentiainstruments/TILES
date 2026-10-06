#pragma once

/* MIDI clock: the timing source for the sequencer (services/op_mode.h).
 * Two sources feed ONE pulse counter, so the sequencer never cares which
 * is driving it:
 *
 *   1. EXTERNAL: Real-Time bytes (0xF8 Clock, 0xFA Start, 0xFB Continue,
 *      0xFC Stop) from USB or DIN, delivered by midi/midi_in.h's callback
 *      (midi_in forwards only one source at a time, so two clocks can't
 *      double the tempo). Always wins the moment it is present.
 *   2. TAP TEMPO: op_mode.c feeds circle (shift) presses here while the
 *      sequencer is active and no external clock is present. After 4 taps
 *      the averaged interval (up to the last 8 taps) becomes the internal
 *      tempo, and scan synthesizes 24 PPQN pulses into the same counter.
 *      The first tempo of a tap session fires `start_edge` like a real
 *      Start. Each later tap re-syncs the phase to the tap. A pause longer
 *      than the session timeout starts a new tap session without stopping
 *      the running tempo. A tap far off the session's average also starts
 *      a new session (like Live's tap tempo).
 *
 * op_mode.c flashes circle's LED on every beat (pulse_count % 24 == 0)
 * whichever source is driving.
 *
 * `pulse_count` advances regardless of `running`, like real MIDI clock
 * (the master keeps clocking; Start/Continue/Stop set the slave's running
 * flag). Consumers diff the counter against their last value each scan,
 * which handles several pulses between scans. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Total pulses seen, regardless of `running`; diff against your last read.
     * Advanced by real 0xF8 bytes or, with no external clock, the tap-tempo
     * generator. */
    uint32_t pulse_count;
    /* Transport state: true from a Start/Continue (or a tap tempo starting
     * playback) until a Stop. */
    bool running;
    /* True only on the first get_state() call after a Start (0xFA, not
     * Continue) or a tap-tempo start; cleared by that read, like an edge flag.
     * Continue sets `running` without it (resume, don't reset position). */
    bool start_edge;
    /* True while the tap-tempo generator (not external clock) is driving
     * pulse_count. For diagnostics/UI. */
    bool source_is_tap_tempo;
} tiles_midi_clock_state_t;

void tiles_midi_clock_init(void);

/* Advances the tap-tempo generator by the elapsed time and handles
 * external clock appearing or disappearing. Call every main-loop pass,
 * after tiles_midi_in_scan() and before anything reads the clock state. */
void tiles_midi_clock_scan(void);

/* Clock/transport snapshot. Clears start_edge, so each consumer should
 * call it once per scan (op_mode is the only consumer). */
tiles_midi_clock_state_t tiles_midi_clock_get_state(void);

/* True if a real Real-Time byte arrived within EXTERNAL_CLOCK_TIMEOUT_MS:
 * a clock source is sending NOW. Checked before treating circle presses
 * as taps; register_tap() also no-ops when true. */
bool tiles_midi_clock_external_active(uint32_t now_ms);

/* Registers a tap at `now_ms`. op_mode decides what qualifies (sequencer
 * active, no external clock, not part of the 4-button combo). No-op while
 * external clock is active. */
void tiles_midi_clock_register_tap(uint32_t now_ms);

/* True once a tap tempo has been established (4+ taps). For UI. */
bool tiles_midi_clock_tap_tempo_established(void);

/* Sets the transport's running state (the sequencer's start/stop). No-op
 * while external clock is active: the DAW's Start/Stop rule then. `true`
 * does NOT set start_edge (resume, Continue-style). */
void tiles_midi_clock_set_running(bool running);

/* Running state from either source. Doesn't consume start_edge, so it's
 * safe to call anytime. op_mode uses it to tell stop-while-stopped
 * (rewind) from stop-while-playing, and play-while-playing (restart) from
 * play-while-stopped. */
bool tiles_midi_clock_is_running(void);

/* Current tempo in ms per quarter note, from whichever source is driving
 * (external: measured beat to beat; tap: the averaged interval). 500
 * (120 BPM) until either exists. op_mode's tempo-synced pulses read it. */
float tiles_midi_clock_get_ms_per_beat(void);
