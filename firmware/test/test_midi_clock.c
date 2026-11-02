/*
 * services/midi_clock.c (pico time stubbed): an external Start begins on
 * the next Clock, the downbeat, with the counter on a beat boundary, so
 * steps land on the DAW's grid. Run with test/run.sh.
 */

#include <assert.h>
#include <stdio.h>

#include "midi_clock.h"
#include "midi_in.h"

uint32_t g_now_ms;

static tiles_midi_in_realtime_callback_t s_rt;
bool tiles_midi_in_register_realtime_callback(tiles_midi_in_realtime_callback_t callback) {
    s_rt = callback;
    return true;
}

static tiles_midi_in_song_position_callback_t s_spp;
void tiles_midi_in_set_song_position_callback(tiles_midi_in_song_position_callback_t callback) {
    s_spp = callback;
}

static void byte(uint8_t b) {
    s_rt(b, g_now_ms);
}

/* One main-loop pass: the clock scan, then op_mode's single read. */
static tiles_midi_clock_state_t pass(void) {
    tiles_midi_clock_scan();
    return tiles_midi_clock_get_state();
}

int main(void) {
    g_now_ms = 1000u;
    tiles_midi_clock_init();
    assert(s_rt != NULL);

    /* 1. clock already running (no Start): it's picked up as a start */
    for (int i = 0; i < 5; i++) {
        byte(0xF8);
    }
    tiles_midi_clock_state_t st = pass();
    assert(st.running && st.start_edge && st.pulse_count == 5u);

    /* 2. Stop, then a Start alone in a pass: nothing starts yet */
    g_now_ms += 10u;
    byte(0xFC);
    st = pass();
    assert(!st.running && !st.start_edge);
    byte(0xFA);
    st = pass();
    assert(!st.running && !st.start_edge);

    /* 3. the next Clock is the downbeat: start there, on a beat boundary */
    g_now_ms += 10u;
    byte(0xF8);
    st = pass();
    assert(st.running && st.start_edge && st.pulse_count % 24u == 0u);
    uint32_t downbeat = st.pulse_count;
    /* six more Clocks: exactly one sixteenth after the downbeat */
    for (int i = 0; i < 6; i++) {
        byte(0xF8);
    }
    st = pass();
    assert(st.running && !st.start_edge && st.pulse_count == downbeat + 6u);

    /* 4. Start and its Clock in the same pass (the usual USB case): same result */
    for (int i = 0; i < 7; i++) {
        byte(0xF8); /* leave the counter off a beat */
    }
    byte(0xFC);
    pass();
    byte(0xFA);
    byte(0xF8);
    st = pass();
    assert(st.running && st.start_edge && st.pulse_count % 24u == 0u);

    /* 5. a Start already on a beat boundary's eve stays put (no extra beat) */
    byte(0xFC);
    pass();
    for (int i = 0; i < 23; i++) {
        byte(0xF8);
    }
    pass();
    uint32_t before = tiles_midi_clock_get_state().pulse_count;
    assert(before % 24u == 23u);
    byte(0xFA);
    byte(0xF8);
    st = pass();
    assert(st.start_edge && st.pulse_count == before + 1u);

    /* 6. Continue resumes without a start edge; Stop cancels a waiting Start */
    byte(0xFC);
    pass();
    byte(0xFB);
    st = pass();
    assert(st.running && !st.start_edge);
    byte(0xFA);
    byte(0xFC);
    byte(0xF8);
    st = pass();
    assert(!st.running && !st.start_edge);

    /* 7. a Start whose Clock never comes is forgotten once the clock is gone */
    byte(0xFA);
    pass();
    g_now_ms += 600u; /* past the external-clock timeout */
    st = pass();
    assert(!st.running && !st.start_edge);

    /* 8. the lead: at 120 BPM (20.833 ms a pulse), after a Start, each pulse
     * comes through 15 ms before its Clock, never two ahead, never back */
    tiles_midi_clock_set_lead_ms(15u);
    g_now_ms = 100000u;
    byte(0xFA);
    byte(0xF8); /* the downbeat */
    st = pass();
    assert(st.start_edge && st.pulse_count % 24u == 0u);
    downbeat = st.pulse_count;
    /* Clocks at 0, 21, 42, 63 ms (whole ms: 21 ~ 20.8) */
    for (int k = 1; k <= 3; k++) {
        g_now_ms = 100000u + (uint32_t)(21 * k);
        byte(0xF8);
        st = pass();
        assert(st.pulse_count == downbeat + (uint32_t)k);
    }
    /* 63 ms: pace known (3 intervals of 21). 5 ms after: not yet; 6 ms
     * after (= 21 - 15): the next pulse is through */
    g_now_ms = 100063u + 5u;
    assert(pass().pulse_count == downbeat + 3u);
    g_now_ms = 100063u + 6u;
    assert(pass().pulse_count == downbeat + 4u);
    /* still one ahead at most, however long the Clock takes */
    g_now_ms = 100063u + 40u;
    assert(pass().pulse_count == downbeat + 4u);
    /* the Clock arrives: no double count */
    byte(0xF8);
    assert(pass().pulse_count == downbeat + 4u);
    /* a Stop never takes a pulse back */
    byte(0xFC);
    assert(pass().pulse_count == downbeat + 4u);

    /* 9. lead 0 = off */
    tiles_midi_clock_set_lead_ms(0u);
    g_now_ms = 200000u;
    byte(0xFA);
    byte(0xF8);
    downbeat = pass().pulse_count;
    for (int k = 1; k <= 4; k++) {
        g_now_ms = 200000u + (uint32_t)(21 * k);
        byte(0xF8);
        pass();
    }
    g_now_ms += 20u;
    assert(pass().pulse_count == downbeat + 4u);
    byte(0xFC);
    pass();

    /* 10. the tap/restart generator keeps real time: 120 BPM is 480 pulses
     * in 10 s (whole-ms pulses made 476) */
    g_now_ms = 300000u; /* the external clock long gone */
    pass();
    assert(tiles_midi_clock_restart());
    uint32_t from = pass().pulse_count;
    for (uint32_t ms = 1u; ms <= 10000u; ms++) {
        g_now_ms = 300000u + ms;
        pass();
    }
    uint32_t pulses = pass().pulse_count - from;
    assert(pulses >= 479u && pulses <= 481u);

    /* 11. Song Position Pointer + Continue (a DAW playing from mid-song):
     * starts on the next Clock at that position, the beat phase matching */
    assert(s_spp != NULL);
    g_now_ms = 400000u;
    byte(0xFC);
    pass();
    s_spp(9u, g_now_ms); /* 9 sixteenths in: beat 3, second sixteenth */
    byte(0xFB);
    st = pass();
    assert(!st.running && !st.start_edge);
    byte(0xF8);
    st = pass();
    assert(st.running && st.start_edge && st.song_pulse == 54u && st.pulse_count % 24u == 54u % 24u);
    for (int i = 0; i < 6; i++) {
        byte(0xF8);
    }
    st = pass();
    assert(st.song_pulse == 60u && !st.start_edge);

    /* 12. a plain Continue (no position) resumes; a Start is the top again */
    byte(0xFC);
    pass();
    byte(0xFB);
    st = pass();
    assert(st.running && !st.start_edge);
    byte(0xFC);
    pass();
    s_spp(40u, g_now_ms);
    byte(0xFA); /* a Start ignores the position */
    byte(0xF8);
    st = pass();
    assert(st.start_edge && st.song_pulse == 0u && st.pulse_count % 24u == 0u);

    printf("midi_clock: all tests pass\n");
    return 0;
}
