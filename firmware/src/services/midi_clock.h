#pragma once

/*
 * MIDI clock -- the timing source services/op_mode.h's sequencer (and,
 * eventually, arp) mode runs from. Two sources feed the SAME shared
 * pulse counter transparently, so op_mode.h's own consumer code never
 * needs to know or care which one is currently driving it:
 *
 *   1. RECEIVE: real System Real-Time bytes (0xF8 Clock, 0xFA Start,
 *      0xFB Continue, 0xFC Stop) parsed from USB MIDI IN -- see the
 *      "Scope" section below. Always takes priority the instant it's
 *      present.
 *   2. TAP TEMPO: real feedback: "lets make the midi clock work in a way
 *      where we can do master tap tempo on the instrument with shift
 *      round button when not derecting midi clock from a daw. the tapp
 *      tempo is only active in sequencer and arp mode and requiere 4
 *      taps to calcualte minimum." services/op_mode.h feeds circle
 *      (SW6, "shift") button-press timestamps to
 *      tiles_midi_clock_register_tap() -- ONLY while sequencer/arp mode
 *      is active AND tiles_midi_clock_external_active() is false, gating
 *      logic that lives in op_mode.c, not here (this file only knows
 *      about clock bytes/taps, not which operation mode is active).
 *      Once 4 taps land within TAP_TEMPO_TIMEOUT_MS of each other, the
 *      averaged interval becomes this board's own internal master
 *      tempo -- tiles_midi_clock_scan() then synthesizes virtual clock
 *      pulses at that rate (24 per quarter note, the same MIDI-spec
 *      resolution real clock bytes use) into the exact same
 *      `pulse_count` field, and the FIRST time a tempo is established
 *      this session, `start_edge` fires exactly the way a real 0xFA
 *      Start would (op_mode.c's sequencer reset-to-step-0 logic needs no
 *      changes at all to support this -- it already only cares about
 *      the shared state contract, not which source produced it). Every
 *      tap after that first one re-syncs the internal generator's phase
 *      to the moment of the tap (so the beat visibly/audibly snaps to
 *      your tapping, not just the tempo) and refines the averaged
 *      interval from up to the last 8 taps. A gap longer than
 *      TAP_TEMPO_TIMEOUT_MS between taps starts a fresh tap session
 *      (the next tap begins accumulating toward a new 4-tap minimum
 *      again) WITHOUT interrupting whatever tempo was already
 *      established and running -- pausing to think doesn't stop the
 *      clock.
 *      Real external clock reappearing always wins immediately: the
 *      internal generator simply stops advancing `pulse_count` the
 *      moment tiles_midi_clock_external_active() goes true again (real
 *      bytes take over incrementing the same counter), no explicit
 *      hand-off code needed.
 *      "and then flash that light as the tempo even when midi sync
 *      flash the tempo there" -- op_mode.c reads `pulse_count % 24 == 0`
 *      itself each scan to flash circle's own LED on every beat,
 *      regardless of which of the two sources above is actually driving
 *      the counter at that moment -- see op_mode.c's own file header for
 *      that half.
 *
 * USB MIDI IN only for now: midi/midi_out.h's own header notes DIN MIDI
 * IN isn't built yet, so RX only reads tud_midi_stream_read() (the
 * composite device's descriptor already includes a full IN+OUT endpoint
 * pair, see midi/usb_descriptors.c's TUD_MIDI_DESCRIPTOR call, so no
 * descriptor change was needed, just actually calling the read side).
 * Once DIN MIDI IN exists, its own byte stream would feed the exact same
 * parser this file already has.
 *
 * Scope: ONLY the four System Real-Time bytes that matter for
 * transport/tempo are parsed from incoming MIDI; everything else read
 * from the USB MIDI IN stream is silently discarded. Real-time bytes are
 * always single, complete messages that can legally appear anywhere in a
 * MIDI byte stream (never as a data byte of another message, per the
 * MIDI spec's real-time priority rule), so no running-status/data-byte
 * state machine is needed to find them safely -- a plain byte-by-byte
 * scan is correct. Full MIDI IN (note events, CC, etc. -- e.g. for a
 * future live-input-driven arp) is a separate, not-yet-built feature;
 * this file is intentionally narrow.
 *
 * Pulse counting, not a callback: `pulse_count` increments regardless of
 * transport (`running`) state -- matching real MIDI clock behavior,
 * where a master keeps sending clock continuously and it's the SLAVE's
 * own running flag (set by Start/Continue, cleared by Stop) that decides
 * whether to act on new pulses; the internal tap-tempo generator mirrors
 * that same convention. tiles_op_mode_scan() diffs this counter against
 * its own last-seen value each scan (robust to more than one pulse
 * arriving between scans, and to the main loop's own variable iteration
 * rate) rather than this file pushing a per-pulse event.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Total clock pulses ever seen, counted regardless of `running`
     * below -- diff this against your own last-read value to find out
     * how many pulses occurred since you last checked. Advances from
     * EITHER real 0xF8 bytes or, while no external clock is present, the
     * internal tap-tempo generator -- see this file's own header. */
    uint32_t pulse_count;
    /* Current transport state: true from the scan a Start/Continue (or
     * the first tap-tempo establishment) is seen until the scan a Stop
     * is seen (or until boot, if none of those have ever happened). */
    bool running;
    /* True only on the exact tiles_midi_clock_get_state() call that
     * first observes a Start (0xFA, NOT Continue) OR a first-ever
     * tap-tempo establishment since the previous call -- consumed
     * (cleared) by that same read, like a hardware status register's
     * edge flag. Continue (0xFB) sets `running` true without setting
     * this -- it resumes from wherever playback already was, it does
     * not reset position. */
    bool start_edge;
    /* True while `pulse_count` is currently being advanced by the
     * internal tap-tempo generator rather than real incoming MIDI clock
     * bytes -- for diagnostics/future UI, not needed by op_mode.c's own
     * sequencer logic (which treats pulse_count identically either way). */
    bool source_is_tap_tempo;
} tiles_midi_clock_state_t;

void tiles_midi_clock_init(void);

/* Drains and parses every byte currently available from USB MIDI IN, and
 * advances the internal tap-tempo generator (if it's the currently
 * active source -- see this file's own header) by whatever real time has
 * elapsed since the last call. Call every main-loop iteration, after
 * tud_task() (so this iteration's USB RX FIFO is current) and before
 * anything that reads tiles_midi_clock_get_state() or
 * tiles_midi_clock_external_active(). */
void tiles_midi_clock_scan(void);

/* Snapshot of current clock/transport state -- see the struct's own
 * field comments. Consumes (clears) start_edge as a side effect, so call
 * this at most once per scan per consumer; with a single consumer
 * (services/op_mode.h) today, that's automatically satisfied. */
tiles_midi_clock_state_t tiles_midi_clock_get_state(void);

/* True if a real 0xF8 byte has been seen within the last
 * EXTERNAL_CLOCK_TIMEOUT_MS (midi_clock.c) -- "is a DAW/hardware clock
 * source currently actually sending," not just "has one ever been seen."
 * services/op_mode.h checks this before treating a circle-button press
 * as a tap-tempo tap at all (real feedback: "when not derecting midi
 * clock from a daw"), and tiles_midi_clock_register_tap() below also
 * defensively no-ops if this is true, so a stray call from anywhere
 * else can't accidentally fight a real external clock. */
bool tiles_midi_clock_external_active(uint32_t now_ms);

/* Registers one tap-tempo tap at `now_ms` -- call once per qualifying
 * circle-button press edge (services/op_mode.h owns deciding what
 * "qualifying" means: sequencer/arp mode active, no real external clock
 * detected, and not part of game_mode.h's reserved 4-button combo). A
 * no-op if external clock is currently active (see
 * tiles_midi_clock_external_active() above) -- real MIDI clock always
 * wins. See this file's own header for the full tap-tempo behavior
 * (4-tap minimum, phase-resync per tap, session-timeout-without-
 * interrupting-playback). */
void tiles_midi_clock_register_tap(uint32_t now_ms);

/* True once at least 4 taps have landed within the current tap session
 * (see tiles_midi_clock_register_tap()) and a tempo has actually been
 * established -- real feedback: "requiere 4 taps to calcualte minimum."
 * Not needed for op_mode.c's own clock consumption (pulse_count/running
 * already reflect this), but useful for any future "still waiting for
 * enough taps" UI. */
bool tiles_midi_clock_tap_tempo_established(void);

/* Manually sets the transport's running state -- services/op_mode.h's
 * sequencer start/stop control (real feedback: "we need a button that
 * starts and stops sequencer"). A no-op while
 * tiles_midi_clock_external_active() is true, mirroring
 * tiles_midi_clock_register_tap()'s own "external always wins" guard --
 * the DAW's own Start/Stop bytes are the only valid transport control
 * whenever a real clock is present. Setting `true` does NOT set
 * start_edge (this resumes wherever pulse_count already is, Continue-
 * style, not a reset to step zero) -- op_mode.h only calls this to
 * resume playback that tap-tempo's own first establishment already
 * started once (that path already sets start_edge itself, see
 * tiles_midi_clock_register_tap() above). */
void tiles_midi_clock_set_running(bool running);
