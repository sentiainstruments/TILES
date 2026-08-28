#include "midi_clock.h"

#include "pico/time.h"

#include "tusb.h"

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

static uint32_t s_pulse_count;
static bool s_running;
static bool s_start_edge;
static bool s_source_is_tap_tempo;

static uint32_t s_last_external_pulse_ms;
static bool s_ever_seen_external_pulse;

static uint32_t s_tap_timestamps[TAP_TEMPO_MAX_HISTORY];
static uint8_t s_tap_count; /* taps in the CURRENT session, caps at TAP_TEMPO_MAX_HISTORY (oldest drops off) */
static uint32_t s_last_tap_ms;
static bool s_tap_tempo_established;
static float s_tap_interval_ms; /* averaged ms per quarter-note tap, valid only once established */
static uint32_t s_next_virtual_pulse_due_ms;

void tiles_midi_clock_init(void) {
    s_pulse_count = 0u;
    s_running = false;
    s_start_edge = false;
    s_source_is_tap_tempo = false;
    s_last_external_pulse_ms = 0u;
    s_ever_seen_external_pulse = false;
    s_tap_count = 0u;
    s_last_tap_ms = 0u;
    s_tap_tempo_established = false;
    s_tap_interval_ms = 0.0f;
    s_next_virtual_pulse_due_ms = 0u;
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

    bool first_establishment = !s_tap_tempo_established;
    s_tap_tempo_established = true;

    /* Resync the internal generator's phase to THIS tap -- the next
     * virtual pulse starts counting fresh from right now, so the beat
     * visibly/audibly snaps to your tapping on every tap, not just the
     * tempo. */
    float virtual_pulse_interval_ms = s_tap_interval_ms / 24.0f; /* 24 clocks/quarter note, per the MIDI spec */
    s_next_virtual_pulse_due_ms = now_ms + (uint32_t)(virtual_pulse_interval_ms + 0.5f);

    if (first_establishment) {
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

void tiles_midi_clock_scan(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    uint8_t buf[16];
    uint32_t read;
    /* Loop, not a single read -- tud_midi_stream_read() only fills up to
     * sizeof(buf) per call; draining the whole RX FIFO this tick (rather
     * than leaving bytes queued for next scan) keeps clock latency down
     * to whatever the main loop's own iteration time is. */
    while ((read = tud_midi_stream_read(buf, sizeof(buf))) > 0u) {
        for (uint32_t i = 0; i < read; i++) {
            switch (buf[i]) {
            case MIDI_REALTIME_CLOCK:
                s_pulse_count++;
                s_last_external_pulse_ms = now_ms;
                s_ever_seen_external_pulse = true;
                break;
            case MIDI_REALTIME_START:
                s_running = true;
                s_start_edge = true;
                s_last_external_pulse_ms = now_ms;
                s_ever_seen_external_pulse = true;
                break;
            case MIDI_REALTIME_CONTINUE:
                /* Resumes wherever playback already was -- deliberately
                 * does NOT set start_edge (that's reset-to-step-zero,
                 * Continue is the opposite of that). */
                s_running = true;
                s_last_external_pulse_ms = now_ms;
                s_ever_seen_external_pulse = true;
                break;
            case MIDI_REALTIME_STOP:
                s_running = false;
                s_last_external_pulse_ms = now_ms;
                s_ever_seen_external_pulse = true;
                break;
            default:
                /* Everything else (notes, CC, sysex bytes, etc.) is out
                 * of scope for this clock-only receiver -- see the
                 * header's own "Scope" section. */
                break;
            }
        }
    }

    /* Internal tap-tempo generator -- only ever advances pulse_count
     * while no real external clock is currently active; real bytes
     * above always take priority (the loop above already updated
     * s_last_external_pulse_ms if any arrived THIS scan, so this check
     * is already current). See this file's own header for the full
     * reasoning. */
    if (!tiles_midi_clock_external_active(now_ms) && s_tap_tempo_established) {
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
