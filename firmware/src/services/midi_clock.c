#include "midi_clock.h"

#include "tusb.h"

/* MIDI System Real-Time status bytes -- single-byte messages, no data
 * bytes ever follow. See this file's header for why a plain byte scan
 * (no running-status tracking) is sufficient and correct for finding
 * these. */
#define MIDI_REALTIME_CLOCK 0xF8u
#define MIDI_REALTIME_START 0xFAu
#define MIDI_REALTIME_CONTINUE 0xFBu
#define MIDI_REALTIME_STOP 0xFCu

static uint32_t s_pulse_count;
static bool s_running;
static bool s_start_edge;

void tiles_midi_clock_init(void) {
    s_pulse_count = 0u;
    s_running = false;
    s_start_edge = false;
}

void tiles_midi_clock_scan(void) {
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
                break;
            case MIDI_REALTIME_START:
                s_running = true;
                s_start_edge = true;
                break;
            case MIDI_REALTIME_CONTINUE:
                /* Resumes wherever playback already was -- deliberately
                 * does NOT set start_edge (that's reset-to-step-zero,
                 * Continue is the opposite of that). */
                s_running = true;
                break;
            case MIDI_REALTIME_STOP:
                s_running = false;
                break;
            default:
                /* Everything else (notes, CC, sysex bytes, etc.) is out
                 * of scope for this clock-only receiver -- see the
                 * header's own "Scope" section. */
                break;
            }
        }
    }
}

tiles_midi_clock_state_t tiles_midi_clock_get_state(void) {
    tiles_midi_clock_state_t state = {
        .pulse_count = s_pulse_count,
        .running = s_running,
        .start_edge = s_start_edge,
    };
    s_start_edge = false;
    return state;
}
