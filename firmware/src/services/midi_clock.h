#pragma once

/*
 * MIDI clock RECEIVE (System Real-Time messages only) -- the timing
 * source services/op_mode.h's sequencer mode runs from. Real feedback:
 * "we pull midi clock from midi or usb from software or hardware and
 * thats how the clock works" -- deliberately no internal free-running
 * fallback tempo; the sequencer simply doesn't advance until a real
 * external clock is sending, exactly the way a hardware sequencer
 * chained off a DAW/master clock behaves.
 *
 * USB only for now: midi/midi_out.h's own header notes DIN MIDI IN isn't
 * built yet, so this only reads tud_midi_stream_read() (USB MIDI IN --
 * the composite device's descriptor already includes a full IN+OUT
 * endpoint pair, see midi/usb_descriptors.c's TUD_MIDI_DESCRIPTOR call,
 * so no descriptor change was needed, just actually calling the read
 * side). Once DIN MIDI IN exists, its own byte stream would feed the
 * exact same tiles_midi_clock_feed_byte() parser this file already has
 * -- "from midi or usb... hardware" in the real feedback above is this
 * module's own forward-looking design, not yet wired to a second
 * physical source.
 *
 * Scope: ONLY the four System Real-Time bytes that matter for
 * transport/tempo (0xF8 Clock, 0xFA Start, 0xFB Continue, 0xFC Stop)
 * are parsed; everything else read from the USB MIDI IN stream is
 * silently discarded. Real-time bytes are always single, complete
 * messages that can legally appear anywhere in a MIDI byte stream (never
 * as a data byte of another message, per the MIDI spec's real-time
 * priority rule), so no running-status/data-byte state machine is
 * needed to find them safely -- a plain byte-by-byte scan is correct.
 * Full MIDI IN (note events, CC, etc. -- e.g. for a future live-input-
 * driven arp) is a separate, not-yet-built feature; this file is
 * intentionally narrow.
 *
 * Pulse counting, not a callback: 0xF8 increments a monotonic counter
 * regardless of transport (running) state -- matching real MIDI clock
 * behavior, where a master keeps sending clock continuously and it's the
 * SLAVE's own running flag (set by Start/Continue, cleared by Stop) that
 * decides whether to act on new pulses. tiles_op_mode_scan() diffs this
 * counter against its own last-seen value each scan (robust to more than
 * one pulse arriving between scans, and to the main loop's own variable
 * iteration rate) rather than this file pushing a per-pulse event.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Total 0xF8 Timing Clock bytes ever seen, counted regardless of
     * `running` below -- diff this against your own last-read value to
     * find out how many pulses occurred since you last checked. */
    uint32_t pulse_count;
    /* Current transport state: true from the scan a Start/Continue is
     * seen until the scan a Stop is seen (or until boot, if neither has
     * ever arrived). */
    bool running;
    /* True only on the exact tiles_midi_clock_get_state() call that
     * first observes a Start (0xFA, NOT Continue) since the previous
     * call -- consumed (cleared) by that same read, like a hardware
     * status register's edge flag. Continue (0xFB) sets `running` true
     * without setting this -- it resumes from wherever playback already
     * was, it does not reset position. */
    bool start_edge;
} tiles_midi_clock_state_t;

void tiles_midi_clock_init(void);

/* Drains and parses every byte currently available from USB MIDI IN.
 * Call every main-loop iteration, after tud_task() (so this iteration's
 * USB RX FIFO is current) and before anything that reads
 * tiles_midi_clock_get_state(). */
void tiles_midi_clock_scan(void);

/* Snapshot of current clock/transport state -- see the struct's own
 * field comments. Consumes (clears) start_edge as a side effect, so call
 * this at most once per scan per consumer; with a single consumer
 * (services/op_mode.h) today, that's automatically satisfied. */
tiles_midi_clock_state_t tiles_midi_clock_get_state(void);
