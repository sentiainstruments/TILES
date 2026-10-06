#include "midi_clock.h"

#include "midi_in.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* System Real-Time status bytes (single byte, no data). */
#define MIDI_REALTIME_CLOCK 0xF8u
#define MIDI_REALTIME_START 0xFAu
#define MIDI_REALTIME_CONTINUE 0xFBu
#define MIDI_REALTIME_STOP 0xFCu

/* No Real-Time byte for this long = no external clock. Even 40 BPM sends a
 * pulse every ~62.5 ms, so this is safe while still noticing a disconnect
 * quickly. */
#define EXTERNAL_CLOCK_TIMEOUT_MS 500u

/* Minimum taps before a tempo is set. */
#define TAP_TEMPO_MIN_TAPS 4u
/* Average over up to the last 8 taps: smooths one uneven tap without
 * making the estimate sluggish. */
#define TAP_TEMPO_MAX_HISTORY 8u
/* A gap this long between taps starts a new tap session (generous: slow
 * tempos can have over a second between taps). The running tempo keeps
 * going. */
#define TAP_TEMPO_SESSION_TIMEOUT_MS 2000u
/* A tap whose interval strays this far (fraction) from the session's
 * average is a new tempo attempt, not noise to average in. A first guess. */
#define TAP_TEMPO_CONSISTENCY_TOLERANCE 0.30f

static uint32_t s_pulse_count;
static bool s_running;
static bool s_start_edge;
static bool s_source_is_tap_tempo;
/* Whether external clock was active as of the last scan (edge detection). */
static bool s_external_was_active;

static uint32_t s_last_external_pulse_ms;
static bool s_ever_seen_external_pulse;

/* Beat-to-beat (every 24th Clock) timing, since Clock bytes carry no tempo.
 * 500 ms is only a placeholder until the first real beat is measured. */
static uint32_t s_last_external_beat_ms;
static float s_external_ms_per_beat = 500.0f;

static uint32_t s_tap_timestamps[TAP_TEMPO_MAX_HISTORY];
static uint8_t s_tap_count; /* taps in the CURRENT session, capped at TAP_TEMPO_MAX_HISTORY */
static uint32_t s_last_tap_ms;
static bool s_tap_tempo_established;
/* True once THIS session reached TAP_TEMPO_MIN_TAPS; unlike
 * s_tap_tempo_established (true for the rest of the boot), it resets, so
 * every fresh 4-tap sequence after a manual stop auto-starts playback. */
static bool s_session_hit_minimum;
static float s_tap_interval_ms; /* averaged ms per quarter note, valid once established */
static uint32_t s_next_virtual_pulse_due_ms;

/* Forward-declared for registration in init. */
static void midi_clock_on_realtime_byte(uint8_t realtime_byte, uint32_t now_ms);

void tiles_midi_clock_init(void) {
    s_pulse_count = 0u;
    s_running = false;
    s_start_edge = false;
    s_source_is_tap_tempo = false;
    s_last_external_pulse_ms = 0u;
    s_ever_seen_external_pulse = false;
    s_external_was_active = false;
    s_tap_count = 0u;
    s_last_tap_ms = 0u;
    s_tap_tempo_established = false;
    s_session_hit_minimum = false;
    s_tap_interval_ms = 0.0f;
    s_next_virtual_pulse_due_ms = 0u;
    /* Real-Time bytes arrive via midi/midi_in.h, the single reader of MIDI
     * input. */
    tiles_midi_in_register_realtime_callback(midi_clock_on_realtime_byte);
}

bool tiles_midi_clock_is_running(void) {
    return s_running;
}

float tiles_midi_clock_get_ms_per_beat(void) {
    if (s_source_is_tap_tempo && s_tap_tempo_established) {
        return s_tap_interval_ms;
    }
    return s_external_ms_per_beat;
}

bool tiles_midi_clock_external_active(uint32_t now_ms) {
    if (!s_ever_seen_external_pulse) {
        return false;
    }
    return (now_ms - s_last_external_pulse_ms) < EXTERNAL_CLOCK_TIMEOUT_MS;
}

void tiles_midi_clock_register_tap(uint32_t now_ms) {
    if (tiles_midi_clock_external_active(now_ms)) {
        /* External clock wins. op_mode already gates on this; defensive. */
        return;
    }

    if (s_tap_count > 0u && (now_ms - s_last_tap_ms) > TAP_TEMPO_SESSION_TIMEOUT_MS) {
        /* Stale session: start counting toward 4 again. The established tempo and
         * transport are untouched, so pausing to think doesn't hiccup the clock. */
        s_tap_count = 0u;
        s_session_hit_minimum = false;
    }

    /* Consistency check before appending, against this session's average so
     * far (needs 2+ taps). A very different tap restarts the session from this
     * tap instead of blending two tempos. */
    if (s_tap_count >= 2u) {
        uint32_t latest_interval = now_ms - s_last_tap_ms;
        float sum_ms = 0.0f;
        for (uint8_t i = 1; i < s_tap_count; i++) {
            sum_ms += (float)(s_tap_timestamps[i] - s_tap_timestamps[i - 1u]);
        }
        float avg_interval = sum_ms / (float)(s_tap_count - 1u);
        if (avg_interval > 0.0f) {
            float deviation = fabsf((float)latest_interval - avg_interval) / avg_interval;
            if (deviation > TAP_TEMPO_CONSISTENCY_TOLERANCE) {
                s_tap_count = 0u;
                s_session_hit_minimum = false;
            }
        }
    }

    if (s_tap_count < TAP_TEMPO_MAX_HISTORY) {
        s_tap_timestamps[s_tap_count] = now_ms;
        s_tap_count++;
    } else {
        /* History full: drop the oldest. */
        for (uint8_t i = 0; i < TAP_TEMPO_MAX_HISTORY - 1u; i++) {
            s_tap_timestamps[i] = s_tap_timestamps[i + 1u];
        }
        s_tap_timestamps[TAP_TEMPO_MAX_HISTORY - 1u] = now_ms;
    }
    s_last_tap_ms = now_ms;

    if (s_tap_count < TAP_TEMPO_MIN_TAPS) {
        /* Fewer than 4 taps: no tempo yet. */
        return;
    }

    float sum_ms = 0.0f;
    for (uint8_t i = 1; i < s_tap_count; i++) {
        sum_ms += (float)(s_tap_timestamps[i] - s_tap_timestamps[i - 1u]);
    }
    s_tap_interval_ms = sum_ms / (float)(s_tap_count - 1u);
    if (s_tap_interval_ms < 1.0f) {
        /* Floor: a near-instant double tap mustn't spin the pulse generator. */
        s_tap_interval_ms = 1.0f;
    }

    /* Auto-start on this SESSION reaching 4 taps (not only the first tempo of
     * the boot), but never while already running, so refining the tempo live
     * doesn't yank the transport. */
    bool session_just_reached_minimum = !s_session_hit_minimum;
    s_session_hit_minimum = true;
    s_tap_tempo_established = true;

    /* Re-sync the generator's phase to this tap, so the beat snaps to the
     * tapping. */
    float virtual_pulse_interval_ms = s_tap_interval_ms / 24.0f; /* 24 clocks per quarter note */
    s_next_virtual_pulse_due_ms = now_ms + (uint32_t)(virtual_pulse_interval_ms + 0.5f);

    if (session_just_reached_minimum && !s_running) {
        /* Same contract as a real Start, so the sequencer resets to step 0. */
        s_running = true;
        s_start_edge = true;
    }
}

bool tiles_midi_clock_tap_tempo_established(void) {
    return s_tap_tempo_established;
}

void tiles_midi_clock_set_running(bool running) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (tiles_midi_clock_external_active(now_ms)) {
        /* External clock wins. */
        return;
    }
    s_running = running;
}

/* Runs from midi_in's scan. `now_ms` is midi_in's per-scan timestamp. */
static void midi_clock_on_realtime_byte(uint8_t realtime_byte, uint32_t now_ms) {
    switch (realtime_byte) {
    case MIDI_REALTIME_CLOCK:
        s_pulse_count++;
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        /* One beat of real pulses done: measure it, bounded to ~15-1200 BPM so a
         * stale timestamp from long ago (Start doesn't reset it) can't produce
         * nonsense. */
        if (s_pulse_count % 24u == 0u) {
            if (s_last_external_beat_ms != 0u) {
                uint32_t interval = now_ms - s_last_external_beat_ms;
                if (interval >= 50u && interval <= 4000u) {
                    s_external_ms_per_beat = (float)interval;
                }
            }
            s_last_external_beat_ms = now_ms;
        }
        break;
    case MIDI_REALTIME_START:
        printf("[midi_clock] real Start (0xFA) received, pulse_count=%lu\n", (unsigned long)s_pulse_count);
        s_running = true;
        s_start_edge = true;
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        break;
    case MIDI_REALTIME_CONTINUE:
        /* Resume where playback was; no start_edge (Continue isn't a reset). */
        printf("[midi_clock] real Continue (0xFB) received, pulse_count=%lu\n", (unsigned long)s_pulse_count);
        s_running = true;
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        break;
    case MIDI_REALTIME_STOP:
        printf("[midi_clock] real Stop (0xFC) received, pulse_count=%lu\n", (unsigned long)s_pulse_count);
        s_running = false;
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        break;
    default:
        /* Unreachable (midi_in only calls with the four above); keeps -Wswitch-
         * default quiet. */
        break;
    }
}

void tiles_midi_clock_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* External clock appearing counts as a Start. If Live was already playing
     * before TILES was listening (or the port reconnected), only Clock bytes
     * arrive, no Start, and the sequencer never latched on. So when external
     * clock becomes active: running = true (external clock has priority) and
     * start_edge = true, which re-anchors every lane's step phase to the
     * current pulse_count. Real Start/Stop still behave as before. */
    bool external_active_now = tiles_midi_clock_external_active(now_ms);
    if (external_active_now != s_external_was_active) {
        /* Log acquire/loss. If ACQUIRED never prints while Live plays, Live isn't
         * sending clock to this port (the port's Sync box in Live's MIDI
         * preferences is off) and TILES is running on tap tempo instead. */
        printf("[midi_clock] external clock %s (ms_per_beat=%.1f)\n", external_active_now ? "ACQUIRED" : "LOST",
               (double)s_external_ms_per_beat);
    }
    if (external_active_now && !s_external_was_active) {
        s_running = true;
        s_start_edge = true;
    }
    s_external_was_active = external_active_now;

    /* Tap-tempo generator: only while no external clock is active
     * (external_active_now already includes any bytes from this scan). */
    if (!external_active_now && s_tap_tempo_established) {
        s_source_is_tap_tempo = true;
        float virtual_pulse_interval_ms = s_tap_interval_ms / 24.0f;
        if (virtual_pulse_interval_ms < 1.0f) {
            virtual_pulse_interval_ms = 1.0f;
        }
        /* Bounded loop: catches up several due pulses without any way to spin. */
        uint32_t guard = 0u;
        while (now_ms >= s_next_virtual_pulse_due_ms && guard < 1000u) {
            s_pulse_count++;
            s_next_virtual_pulse_due_ms += (uint32_t)(virtual_pulse_interval_ms + 0.5f);
            guard++;
        }
    } else {
        s_source_is_tap_tempo = false;
    }
}

tiles_midi_clock_state_t tiles_midi_clock_get_state(void) {
    tiles_midi_clock_state_t state = {
        .pulse_count = s_pulse_count,
        .running = s_running,
        .start_edge = s_start_edge,
        .source_is_tap_tempo = s_source_is_tap_tempo,
    };
    s_start_edge = false;
    return state;
}
