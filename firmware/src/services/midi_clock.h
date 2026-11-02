#pragma once

/* MIDI clock, the sequencers' timing: two sources feed one pulse counter.
 *   EXTERNAL  Real-Time bytes from USB or DIN (one source at a time);
 *             always wins when present.
 *   TAP TEMPO circle taps while no external clock: 4 taps set the tempo
 *             (average of up to 8), 24 PPQN generated; later taps re-sync
 *             the phase; a pause or an off-tempo tap starts a new session.
 * pulse_count advances regardless of `running`, like real MIDI clock;
 * consumers diff it each scan. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Pulses since song position 0 (a Start, a Song Position Pointer, or an
     * internal start). On start_edge a sequencer's step comes from it, so it
     * lands where it would have from the top. */
    uint32_t song_pulse;
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

/* Clock/transport snapshot. Clears start_edge, so a start reaches one
 * reader: op_mode is the only one (its scan and its playback ticks share
 * the edge between them). */
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

/* Play with no external clock: restart the internal clock now as beat 1
 * (120 BPM until tapped). False while external clock rules. */
bool tiles_midi_clock_restart(void);

/* Running state from either source; doesn't consume start_edge. */
bool tiles_midi_clock_is_running(void);

/* Clock lead in ms (midi.clock_lead_ms): 15 = measured recording into Live
 * 12 (64-sample buffer, 48 kHz); 0 = off. */
#define TILES_MIDI_CLOCK_LEAD_MS_DEFAULT 15u
void tiles_midi_clock_set_lead_ms(uint16_t lead_ms);
uint16_t tiles_midi_clock_get_lead_ms(void);

/* Current tempo in ms per quarter note, from whichever source is driving
 * (external: measured beat to beat; tap: the averaged interval). 500
 * (120 BPM) until either exists. op_mode's tempo-synced pulses read it. */
float tiles_midi_clock_get_ms_per_beat(void);
