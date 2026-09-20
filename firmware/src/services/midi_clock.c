#include "midi_clock.h"

#include "midi_in.h"

#include "pico/time.h"

#include <math.h>
#include <stdio.h>

/* MIDI System Real-Time status bytes -- single-byte messages, no data
 * bytes ever follow. See this file's header for why a plain byte scan
 * (no running-status tracking) is sufficient and correct for finding
 * these. */
#define MIDI_REALTIME_CLOCK 0xF8u
#define MIDI_REALTIME_START 0xFAu
#define MIDI_REALTIME_CONTINUE 0xFBu
#define MIDI_REALTIME_STOP 0xFCu

/* No real 0xF8 (or Start/Continue/Stop -- any of the four counts as
 * "something's actually connected and talking") for this long means
 * "no external clock right now," not just "a slightly slow tempo" --
 * even a very slow 40 BPM sends a pulse every ~62.5ms (24 clocks/quarter
 * note per the MIDI spec), so this comfortably covers realistic tempos
 * while still detecting a genuine disconnect/silence reasonably
 * quickly. */
#define EXTERNAL_CLOCK_TIMEOUT_MS 500u

/* Real feedback: "requiere 4 taps to calcualte minimum." */
#define TAP_TEMPO_MIN_TAPS 4u
/* Averages over up to the last 8 taps once past the minimum -- smooths
 * out one uneven tap without making the estimate sluggish to update. */
#define TAP_TEMPO_MAX_HISTORY 8u
/* A gap this long between taps means "stopped tapping, this is a new
 * attempt," not "still the same phrase" -- starts a fresh accumulation
 * toward a new 4-tap minimum without touching whatever tempo/playback
 * was already established (see tiles_midi_clock_register_tap()'s own
 * comment). Deliberately generous -- a real tap-tempo gesture at a slow
 * tempo could have over a second between taps. */
#define TAP_TEMPO_SESSION_TIMEOUT_MS 2000u
/* Real feedback: "it stops capturing tempo if taps very inconsistent
 * similar to ableton lives tap tempo." A tap whose interval from the
 * previous one strays this far (as a fraction) from the running average
 * of this session's OWN intervals so far reads as a different tempo
 * attempt, not noise to blend in -- generous enough for ordinary human
 * timing looseness, tight enough to actually catch a genuinely different
 * tap rate. Unmeasured, a first attempt like every other timing constant
 * in this file. */
#define TAP_TEMPO_CONSISTENCY_TOLERANCE 0.30f

static uint32_t s_pulse_count;
static bool s_running;
static bool s_start_edge;
static bool s_source_is_tap_tempo;
/* tiles_midi_clock_external_active() as of the LAST scan -- see its
 * new use in tiles_midi_clock_scan() below. */
static bool s_external_was_active;

static uint32_t s_last_external_pulse_ms;
static bool s_ever_seen_external_pulse;

/* Beat-to-beat (every 24th Clock byte) timestamp + measured interval,
 * for tiles_midi_clock_get_ms_per_beat() below -- external Clock bytes
 * carry no tempo value of their own (just "another pulse happened"), so
 * unlike tap tempo's own already-averaged s_tap_interval_ms, this has
 * to be derived from real arrival timing. Defaults to 500ms (120bpm
 * equivalent) purely as a sane starting point before any real beat
 * interval has ever been measured -- never actually used as a "real"
 * tempo, just a placeholder nothing downstream should treat as
 * meaningful until a genuine beat interval overwrites it. */
static uint32_t s_last_external_beat_ms;
static float s_external_ms_per_beat = 500.0f;

static uint32_t s_tap_timestamps[TAP_TEMPO_MAX_HISTORY];
static uint8_t s_tap_count; /* taps in the CURRENT session, caps at TAP_TEMPO_MAX_HISTORY (oldest drops off) */
static uint32_t s_last_tap_ms;
static bool s_tap_tempo_established;
/* True once THIS session (since the last session-timeout/inconsistency
 * reset) has reached TAP_TEMPO_MIN_TAPS -- distinct from
 * s_tap_tempo_established (which, once true, never resets for the rest
 * of the boot). Real feedback: "tap tempo should autostart sequence when
 * 4 taps detected even if stopped" -- see this flag's own use in
 * tiles_midi_clock_register_tap() below for why a per-lifetime flag
 * wasn't enough: it only auto-started on the very first tempo ever
 * established, not on every fresh 4-tap sequence after a manual stop. */
static bool s_session_hit_minimum;
static float s_tap_interval_ms; /* averaged ms per quarter-note tap, valid only once established */
static uint32_t s_next_virtual_pulse_due_ms;

/* Defined below, right where the read loop it replaces used to live --
 * forward-declared here only so tiles_midi_clock_init() can register it
 * with midi/midi_in.h. */
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
    /* Real-Time bytes used to be read directly by this file's own
     * tiles_midi_clock_scan() -- refactored to register a callback with
     * midi/midi_in.h instead once that file needed to become the ONE
     * owner of the shared USB MIDI IN FIFO (see its own header comment
     * for why: services/op_mode.c's new Scene Launch mode needs to read
     * SysEx from the SAME stream, and two independent readers can't both
     * drain one FIFO without racing each other for bytes). The callback
     * body below is byte-for-byte what this file's own read loop used
     * to do inline. */
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
        /* Real clock always wins -- see this file's own header. Defensive:
         * services/op_mode.h is expected to already gate on this before
         * ever calling here, but a stray call from anywhere else can't
         * fight a real external clock this way. */
        return;
    }

    if (s_tap_count > 0u && (now_ms - s_last_tap_ms) > TAP_TEMPO_SESSION_TIMEOUT_MS) {
        /* Stale session -- start fresh accumulation toward a new 4-tap
         * minimum. Deliberately does NOT touch s_tap_tempo_established/
         * s_tap_interval_ms/s_running: whatever tempo was already
         * running keeps running smoothly while new taps accumulate,
         * rather than the clock hiccuping just because the player paused
         * to think. */
        s_tap_count = 0u;
        s_session_hit_minimum = false;
    }

    /* Real feedback: "it stops capturing tempo if taps very inconsistent
     * similar to ableton lives tap tempo." Checked BEFORE appending this
     * tap, against the average of this session's intervals SO FAR --
     * needs at least 2 taps already in the session (1 interval) to have
     * anything to compare against. A wildly different tap starts a fresh
     * session from just this tap, exactly like the timeout path above,
     * rather than corrupting the average by blending two different
     * tempos together. */
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
        /* History full -- drop the oldest, shift the rest down, append
         * this tap at the end. TAP_TEMPO_MAX_HISTORY is small (8), so
         * this is cheap. */
        for (uint8_t i = 0; i < TAP_TEMPO_MAX_HISTORY - 1u; i++) {
            s_tap_timestamps[i] = s_tap_timestamps[i + 1u];
        }
        s_tap_timestamps[TAP_TEMPO_MAX_HISTORY - 1u] = now_ms;
    }
    s_last_tap_ms = now_ms;

    if (s_tap_count < TAP_TEMPO_MIN_TAPS) {
        /* Not enough taps yet to compute anything -- real feedback:
         * "requiere 4 taps to calcualte minimum." */
        return;
    }

    float sum_ms = 0.0f;
    for (uint8_t i = 1; i < s_tap_count; i++) {
        sum_ms += (float)(s_tap_timestamps[i] - s_tap_timestamps[i - 1u]);
    }
    s_tap_interval_ms = sum_ms / (float)(s_tap_count - 1u);
    if (s_tap_interval_ms < 1.0f) {
        /* Sane floor -- guards a pathological near-instant double-tap
         * from producing a near-zero interval that would spin the
         * virtual-pulse generator below. */
        s_tap_interval_ms = 1.0f;
    }

    /* Real feedback: "tap tempo should autostart sequence when 4 taps
     * detected even if stopped" -- s_session_hit_minimum (this SESSION's
     * first time reaching the minimum), not s_tap_tempo_established
     * (this BOOT's first time ever), is what should trigger an autostart:
     * re-tapping a tempo after a manual stop is a fresh 4-tap sequence
     * that should also resume playback, not just silently update the
     * number. Guarded on !s_running so re-tapping WHILE already playing
     * (refining the tempo live, Ableton-style) never yanks the transport
     * -- matches real DAW tap-tempo behavior. */
    bool session_just_reached_minimum = !s_session_hit_minimum;
    s_session_hit_minimum = true;
    s_tap_tempo_established = true;

    /* Resync the internal generator's phase to THIS tap -- the next
     * virtual pulse starts counting fresh from right now, so the beat
     * visibly/audibly snaps to your tapping on every tap, not just the
     * tempo. */
    float virtual_pulse_interval_ms = s_tap_interval_ms / 24.0f; /* 24 clocks/quarter note, per the MIDI spec */
    s_next_virtual_pulse_due_ms = now_ms + (uint32_t)(virtual_pulse_interval_ms + 0.5f);

    if (session_just_reached_minimum && !s_running) {
        /* Same contract a real 0xFA Start already gives every consumer --
         * services/op_mode.h's sequencer reset-to-step-0 logic needs no
         * changes at all to also support this. */
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
        /* Real clock always wins -- see tiles_midi_clock_register_tap()'s
         * own identical guard. */
        return;
    }
    s_running = running;
}

/* Byte-for-byte the same logic this file's own read loop used to run
 * inline -- see tiles_midi_clock_init()'s own comment on why this is
 * now a callback registered with midi/midi_in.h instead. `now_ms` is
 * midi_in.c's own single-capture-per-scan timestamp, not re-read here. */
static void midi_clock_on_realtime_byte(uint8_t realtime_byte, uint32_t now_ms) {
    switch (realtime_byte) {
    case MIDI_REALTIME_CLOCK:
        s_pulse_count++;
        s_last_external_pulse_ms = now_ms;
        s_ever_seen_external_pulse = true;
        /* One beat's worth of real pulses just completed -- measure it,
         * sanity-bounded (~15-1200bpm) against a stale s_last_external_
         * beat_ms from a much earlier, unrelated tempo (a fresh Start
         * doesn't reset this timestamp, so the very first beat after a
         * long gap would otherwise compute nonsense) rather than
         * trusting every measurement blindly. */
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
        /* Resumes wherever playback already was -- deliberately does
         * NOT set start_edge (that's reset-to-step-zero, Continue is
         * the opposite of that). */
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
        /* midi_in.c only ever calls this for one of the four cases
         * above -- unreachable in practice, kept only so this switch
         * doesn't need a -Wswitch-default suppression. */
        break;
    }
}

void tiles_midi_clock_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Real feedback: "there is a sync issue between the clock on tiles
     * and ableton. its not auto latching to ableton clock. it should
     * auto switch to that clock when it detedcts it. midi clock has
     * priority over iinternal clock." The receive loop above already
     * gives a real Start (0xFA) byte start_edge -- correct when
     * Ableton's own transport begins while TILES is already listening
     * -- but a bare Clock (0xF8) byte never sets it, on purpose (a
     * pulse alone isn't "this is beat 1," see that case's own
     * comment). That leaves a real gap exactly matching this report:
     * if TILES starts (or resumes) receiving external clock WITHOUT
     * ever seeing the Start that began it -- Ableton was already
     * playing before TILES was listening, or before this port
     * reconnected -- external_active flips true off nothing but plain
     * Clock bytes, s_running never does (Clock alone doesn't set it,
     * same as always), and nothing ever re-anchors any lane's own
     * step-boundary phase to this new source -- indistinguishable from
     * "not auto-latching" from the outside, even though clock bytes
     * are genuinely arriving and being counted. Fixed the same way a
     * real Start already is handled, not a new mechanism: the instant
     * external_active transitions from false to true, treat it exactly
     * like one -- s_running = true (a real clock's mere presence
     * outranks whatever TILES's own internal state already assumed,
     * "midi clock has priority") and start_edge = true (reuses seq_
     * reset()'s own already-safe, already-tested phase realignment --
     * every running lane's step-boundary reference recaptures the
     * CURRENT pulse_count, no separate reset of pulse_count's own
     * absolute value needed or safe to do here, since lanes already
     * mid-flight are tracking against it). A later real Start/Stop
     * still behaves exactly as it always did; this only covers the
     * specific gap where external clock's own PRESENCE, not a
     * particular byte within it, is what should have triggered the
     * switch. */
    bool external_active_now = tiles_midi_clock_external_active(now_ms);
    if (external_active_now != s_external_was_active) {
        /* Direct visibility into whether a real external clock is
         * actually reaching this board at all -- real feedback: "midi
         * clock in sequencer is not syinking to ableton clock... always
         * at the right tempo but not quite synked." If ACQUIRED never
         * prints while Ableton is audibly playing, Ableton isn't
         * actually sending this port real MIDI Clock bytes at all (most
         * commonly: that port's own Sync output checkbox in Preferences
         * -> Link/Tempo/MIDI isn't ticked) -- TILES would then be
         * running on its own internal tap-tempo generator the whole
         * time, which can coincidentally land near the right BPM
         * without ever being phase-locked to Ableton's actual transport,
         * exactly matching this report. If ACQUIRED does print, the
         * clock genuinely is arriving and the gap is somewhere in this
         * file's own phase math instead. */
        printf("[midi_clock] external clock %s (ms_per_beat=%.1f)\n", external_active_now ? "ACQUIRED" : "LOST",
               (double)s_external_ms_per_beat);
    }
    if (external_active_now && !s_external_was_active) {
        s_running = true;
        s_start_edge = true;
    }
    s_external_was_active = external_active_now;

    /* Internal tap-tempo generator -- only ever advances pulse_count
     * while no real external clock is currently active; real bytes
     * above always take priority (the loop above already updated
     * s_last_external_pulse_ms if any arrived THIS scan, so this check
     * is already current). See this file's own header for the full
     * reasoning. */
    if (!external_active_now && s_tap_tempo_established) {
        s_source_is_tap_tempo = true;
        float virtual_pulse_interval_ms = s_tap_interval_ms / 24.0f;
        if (virtual_pulse_interval_ms < 1.0f) {
            virtual_pulse_interval_ms = 1.0f;
        }
        /* Guarded, not an unbounded while() -- handles more than one
         * virtual pulse having come due between scans (matches
         * op_mode.c's own "handle more than one step" pattern) without
         * any realistic path to actually spinning that high. */
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
