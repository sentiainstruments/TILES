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
/* An external Start (or a positioned Continue) came in; playback begins on
 * the next Clock (MIDI's rule: the first Clock after a Start is the
 * downbeat), at s_pending_position pulses into the song. */
static bool s_start_pending;
static uint32_t s_pending_position;
/* A Song Position Pointer since the last Start/Continue: where the next
 * Continue resumes, in pulses (6 per sixteenth). */
static bool s_spp_valid;
static uint32_t s_spp_pulses;
/* The pulse count at the song's position 0 (song_pulse in the state). */
static uint32_t s_song_origin;
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
/* In microseconds: whole milliseconds per pulse ran 120 BPM at 119. */
static uint64_t s_next_virtual_pulse_due_us;

/* Clock lead (setting midi.clock_lead_ms, tiles_midi_clock_set_lead_ms()):
 * following an external clock, each pulse is let through this long before
 * it is due, predicted from the spacing of the last Clocks, so the notes it
 * triggers make up the round trip they lose (the DAW's clock out, USB both
 * ways, this board's pass, the DAW stamping what comes back) and land on
 * the DAW's grid. At most LEAD_MAX_FRACTION of a pulse (never a whole pulse
 * early), and only once LEAD_MIN_CLOCKS Clocks have set the pace; the
 * downbeat after a Start can't be foreseen and comes as it arrives. */
#define LEAD_MAX_FRACTION 0.75f
#define LEAD_PACE_CLOCKS 25u /* up to 24 intervals: a beat */
#define LEAD_MIN_CLOCKS 3u
#define LEAD_MIN_PULSE_US 1000.0f /* faster = a burst, not a tempo */
static uint16_t s_lead_ms = TILES_MIDI_CLOCK_LEAD_MS_DEFAULT;
static uint32_t s_clock_us[LEAD_PACE_CLOCKS]; /* arrival times, a ring */
static uint8_t s_clock_us_head;               /* next slot written */
static uint8_t s_clock_us_count;
/* pulse_count as given out (with the lead): it never goes back. */
static uint32_t s_visible_count;

/* Forward-declared for registration in init. */
static void midi_clock_on_realtime_byte(uint8_t realtime_byte, uint32_t now_ms);
static void midi_clock_on_song_position(uint16_t sixteenths, uint32_t now_ms);

void tiles_midi_clock_init(void) {
    s_pulse_count = 0u;
    s_running = false;
    s_start_edge = false;
    s_start_pending = false;
    s_source_is_tap_tempo = false;
    s_last_external_pulse_ms = 0u;
    s_ever_seen_external_pulse = false;
    s_external_was_active = false;
    s_tap_count = 0u;
    s_last_tap_ms = 0u;
    s_tap_tempo_established = false;
    s_session_hit_minimum = false;
    s_tap_interval_ms = 0.0f;
    s_next_virtual_pulse_due_us = 0u;
    s_clock_us_count = 0u;
    s_clock_us_head = 0u;
    s_visible_count = 0u;
    /* Real-Time bytes arrive via midi/midi_in.h, the single reader of MIDI
     * input. */
    tiles_midi_in_register_realtime_callback(midi_clock_on_realtime_byte);
    tiles_midi_in_set_song_position_callback(midi_clock_on_song_position);
    s_start_pending = false;
    s_spp_valid = false;
    s_song_origin = 0u;
}

static void catch_up_to_visible(void);

/* Waits for the next Clock to start there, `position` pulses into the song,
 * with the counter set so that Clock's count has the song position's phase
 * in the beat (pulse_count % 24 matches the DAW's beats). Stopped
 * meanwhile, so nothing steps across the jump. */
static void start_on_next_clock(uint32_t position) {
    s_running = false;
    s_start_pending = true;
    s_pending_position = position;
    catch_up_to_visible();
    uint32_t before = (position + 23u) % 24u; /* the count just before it */
    s_pulse_count += (before + 24u - s_pulse_count % 24u) % 24u;
    s_clock_us_count = 0u; /* a fresh pace from there */
}

static void midi_clock_on_song_position(uint16_t sixteenths, uint32_t now_ms) {
    (void)now_ms;
    s_spp_valid = true;
    s_spp_pulses = (uint32_t)sixteenths * 6u;
}

bool tiles_midi_clock_is_running(void) {
    return s_running;
}

void tiles_midi_clock_set_lead_ms(uint16_t lead_ms) {
    s_lead_ms = lead_ms;
}

uint16_t tiles_midi_clock_get_lead_ms(void) {
    return s_lead_ms;
}

static uint64_t now_us(void) {
    return to_us_since_boot(get_absolute_time());
}

static uint64_t pulse_us_from_beat_ms(float ms_per_beat) {
    float us = ms_per_beat * 1000.0f / 24.0f;
    if (us < 1000.0f) {
        us = 1000.0f; /* a near-instant double tap mustn't spin the generator */
    }
    return (uint64_t)(us + 0.5f);
}

/* The lead's pace: Clock arrival times. A gap past the external timeout
 * (the clock stopped and came back) starts it over. */
static void pace_push(uint32_t t_us) {
    if (s_clock_us_count > 0u) {
        uint32_t newest = s_clock_us[(s_clock_us_head + LEAD_PACE_CLOCKS - 1u) % LEAD_PACE_CLOCKS];
        if (t_us - newest > EXTERNAL_CLOCK_TIMEOUT_MS * 1000u) {
            s_clock_us_count = 0u;
        }
    }
    s_clock_us[s_clock_us_head] = t_us;
    s_clock_us_head = (uint8_t)((s_clock_us_head + 1u) % LEAD_PACE_CLOCKS);
    if (s_clock_us_count < LEAD_PACE_CLOCKS) {
        s_clock_us_count++;
    }
}

/* The counter can sit one behind what was given out (the lead let a pulse
 * through before its Clock); before realigning it, catch it up. */
static void catch_up_to_visible(void) {
    if (s_pulse_count < s_visible_count) {
        s_pulse_count = s_visible_count;
    }
}

static uint32_t visible_pulse_count(void) {
    uint32_t count = s_pulse_count;
    if (s_running && !s_source_is_tap_tempo && s_lead_ms > 0u && s_clock_us_count >= LEAD_MIN_CLOCKS) {
        uint32_t newest = s_clock_us[(s_clock_us_head + LEAD_PACE_CLOCKS - 1u) % LEAD_PACE_CLOCKS];
        uint32_t oldest = s_clock_us[(s_clock_us_head + LEAD_PACE_CLOCKS - s_clock_us_count) % LEAD_PACE_CLOCKS];
        float pulse_us = (float)(newest - oldest) / (float)(s_clock_us_count - 1u);
        if (pulse_us >= LEAD_MIN_PULSE_US) {
            float lead_us = fminf((float)s_lead_ms * 1000.0f, LEAD_MAX_FRACTION * pulse_us);
            if ((float)((uint32_t)now_us() - newest) >= pulse_us - lead_us) {
                count++;
            }
        }
    }
    if (count < s_visible_count) {
        count = s_visible_count;
    }
    s_visible_count = count;
    return count;
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
    s_next_virtual_pulse_due_us = now_us() + pulse_us_from_beat_ms(s_tap_interval_ms);

    if (session_just_reached_minimum && !s_running) {
        /* Same contract as a real Start, so the sequencer resets to step 0. */
        s_running = true;
        s_start_edge = true;
        s_song_origin = s_pulse_count;
    }
}

bool tiles_midi_clock_tap_tempo_established(void) {
    return s_tap_tempo_established;
}

bool tiles_midi_clock_restart(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (tiles_midi_clock_external_active(now_ms)) {
        return false;
    }
    if (!s_tap_tempo_established) {
        s_tap_interval_ms = 500.0f; /* 120 BPM until a tempo is tapped */
        s_tap_tempo_established = true;
    }
    /* Beat 1 now: the pulse count jumps to the next beat boundary (every lane
     * re-anchors on start_edge, so the jump never plays steps), and the next
     * pulse is one pulse interval from now. */
    catch_up_to_visible();
    s_pulse_count += (24u - s_pulse_count % 24u) % 24u;
    s_song_origin = s_pulse_count;
    s_next_virtual_pulse_due_us = now_us() + pulse_us_from_beat_ms(s_tap_interval_ms);
    s_running = true;
    s_start_edge = true;
    return true;
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
        pace_push((uint32_t)now_us());
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        if (s_start_pending) {
            /* The downbeat, or the positioned Continue's first Clock. */
            s_start_pending = false;
            s_running = true;
            s_start_edge = true;
            s_song_origin = s_pulse_count - s_pending_position;
        }
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
        /* Playback starts on the next Clock, the downbeat: step 1 then, and
         * every later step a whole number of steps after it, on the DAW's grid
         * (starting here put every step after the first one Clock early when
         * the Start arrived a pass before its Clock). The counter is set so
         * that Clock lands on a beat boundary (pulse_count % 24 == 0 on the
         * DAW's beats), which the quantized starts and the beat flash read;
         * stopped meanwhile, so nothing steps across the jump. */
        s_spp_valid = false; /* a Start is always the top */
        start_on_next_clock(0u);
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        break;
    case MIDI_REALTIME_CONTINUE:
        printf("[midi_clock] real Continue (0xFB) received, pulse_count=%lu\n", (unsigned long)s_pulse_count);
        if (s_spp_valid) {
            /* After a Song Position Pointer (a DAW playing from mid-song): start
             * there, like a Start at that position, so steps land where they
             * would have if it had played from the top. */
            s_spp_valid = false;
            start_on_next_clock(s_spp_pulses);
            s_last_external_pulse_ms = now_ms;
            s_ever_seen_external_pulse = true;
            break;
        }
        /* No position: resume where playback was; no start_edge. */
        s_start_pending = false;
        s_running = true;
        s_clock_us_count = 0u;
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        break;
    case MIDI_REALTIME_STOP:
        printf("[midi_clock] real Stop (0xFC) received, pulse_count=%lu\n", (unsigned long)s_pulse_count);
        s_start_pending = false;
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
    if (external_active_now && !s_external_was_active && !s_start_pending) {
        /* (A Start waiting for its downbeat starts there instead.) The song
         * position isn't known: it counts from here. */
        s_running = true;
        s_start_edge = true;
        s_song_origin = s_pulse_count;
    }
    if (!external_active_now) {
        s_start_pending = false; /* a Start whose Clock never came */
    }
    s_external_was_active = external_active_now;

    /* Tap-tempo generator: only while no external clock is active
     * (external_active_now already includes any bytes from this scan). */
    if (!external_active_now && s_tap_tempo_established) {
        s_source_is_tap_tempo = true;
        uint64_t pulse_us = pulse_us_from_beat_ms(s_tap_interval_ms);
        uint64_t t_us = now_us();
        /* Bounded loop: catches up several due pulses without any way to spin. */
        uint32_t guard = 0u;
        while (t_us >= s_next_virtual_pulse_due_us && guard < 1000u) {
            s_pulse_count++;
            s_next_virtual_pulse_due_us += pulse_us;
            guard++;
        }
    } else {
        s_source_is_tap_tempo = false;
    }
}

tiles_midi_clock_state_t tiles_midi_clock_get_state(void) {
    uint32_t visible = visible_pulse_count();
    tiles_midi_clock_state_t state = {
        .pulse_count = visible,
        .song_pulse = visible - s_song_origin,
        .running = s_running,
        .start_edge = s_start_edge,
        .source_is_tap_tempo = s_source_is_tap_tempo,
    };
    s_start_edge = false;
    return state;
}
