#pragma once

/*
 * DIN MIDI (the 5-pin / TRS jacks) -- hardware layer.
 *
 * Real feedback: "are midi plugs working?" -> DIN MIDI was documented as
 * planned (midi/README.md, docs/hardware/) but never built: GP0/GP2 were held
 * high and GP1 was a plain input, nothing read or wrote either. "yes build
 * DIN MIDI" -> this file and its two neighbours:
 *
 *   midi/din_midi_queue.c   the hardware-free logic (rings, coalescing, Real-
 *                           Time priority) -- tested natively off-target
 *   midi/din_midi_tx.pio    the 31,250-baud PIO UART transmitter
 *   midi/din_midi.c         this layer: PIO + UART0 + interrupts
 *
 * IN  (GP1, optoisolated, input-polarity tolerant per the hardware handoff):
 * hardware UART0 RX at 31,250 8N1, drained by an interrupt into a ring, then
 * parsed by midi/midi_in.c with its OWN parser state (a second byte source
 * must never share one parser with USB -- their messages would interleave
 * mid-message). Everything the USB side reacts to -- clock/transport, notes
 * (the melodic echo), SysEx -- reacts to DIN the same way.
 *
 * OUT (GP0 = line A, GP2 = line B, a dual buffer): a PIO UART on ONE of the two
 * lines while the other is held high -- MIDI's current loop only conducts
 * when the two lines differ, and WHICH line carries the data is the TRS
 * polarity (Type A vs Type B). TILES_DIN_MIDI_OUT_DEFAULT_LINE below is that
 * choice. The hardware handoff doesn't say which physical TRS polarity each
 * line corresponds to, so the default is a first guess: if a receiver hears
 * nothing, flip it. Independent of USB: DIN works with no host at all
 * (external power only), which is the whole point of the jack.
 *
 * A failed init disables DIN and nothing else (hardware non-negotiable: "a
 * failed subsystem disables itself; it never blocks USB diagnostics").
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_DIN_MIDI_OUT_LINE_A = 0, /* GP0 carries the data, GP2 held high */
    TILES_DIN_MIDI_OUT_LINE_B = 1, /* GP2 carries the data, GP0 held high */
} tiles_din_midi_out_line_t;

/* Which line carries MIDI OUT data at boot. See the header comment. */
#define TILES_DIN_MIDI_OUT_DEFAULT_LINE TILES_DIN_MIDI_OUT_LINE_A

/* Claims a PIO state machine + UART0, starts both directions. Must run after
 * board_init() (which parks GP0/GP2 high and GP1 as an input). Returns false,
 * leaving DIN disabled and every other call here a no-op, if a resource is
 * unavailable. */
bool tiles_din_midi_init(void);

bool tiles_din_midi_is_ready(void);

/* Switches which line carries MIDI OUT data (the other is parked high).
 * Bytes already in the transmitter's FIFO are discarded. */
void tiles_din_midi_set_out_line(tiles_din_midi_out_line_t line);

/* Queues one channel-voice message (1-3 bytes) or one System Real-Time byte
 * for DIN OUT. No-op if DIN isn't ready. See din_midi_queue.h for what gets
 * coalesced versus queued reliably. */
void tiles_din_midi_send(const uint8_t *msg, uint8_t len);

/* Call every main-loop iteration: flushes coalesced values (bend/pressure/
 * expression) into the transmit queue and makes sure the transmitter is
 * running. */
void tiles_din_midi_service(void);

/* RX side, for midi/midi_in.c. Pops one received byte; false if none. */
bool tiles_din_midi_rx_read_byte(uint8_t *byte);

/* True once if RX bytes were lost/corrupted since the last call (ring
 * overflow, UART framing/break/overrun error). */
bool tiles_din_midi_rx_take_loss(void);
