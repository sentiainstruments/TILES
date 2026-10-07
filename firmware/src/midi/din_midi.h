#pragma once

/* DIN MIDI (the TRS MIDI jacks), hardware layer:
 *
 *   midi/din_midi_queue.c   hardware-free logic (rings, coalescing,
 *                           Real-Time priority), tested natively
 *   midi/din_midi_tx.pio    31,250-baud PIO UART transmitter
 *   midi/din_midi.c         this layer: PIO + UART0 + interrupts
 *
 * IN (GP1, optoisolated): UART0 RX at 31,250 8N1, interrupt-fed ring,
 * parsed by midi/midi_in.c with its own parser state. Everything that
 * reacts to USB MIDI (clock, transport, echo notes, SysEx) reacts to DIN
 * the same way.
 *
 * OUT (GP0 = line A, GP2 = line B): a PIO UART on one line while the other
 * is held high; the current loop only conducts when they differ, and which
 * line carries data is the TRS polarity. Default Type A (the MIDI
 * Association standard), changed only by the `midi.din_trs_type` setting,
 * never auto-detected (the jack can't sense polarity;
 * docs/architecture/defaults-and-safeguards.md). Type A = line A = GP0 is
 * inferred from the handoff's naming, so check it first if a Type A
 * receiver hears nothing. Works without a USB host (external power only).
 *
 * A failed init disables DIN and nothing else. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_DIN_MIDI_TRS_TYPE_A = 0, /* GP0 carries data, GP2 held high */
    TILES_DIN_MIDI_TRS_TYPE_B = 1, /* GP2 carries data, GP0 held high */
} tiles_din_midi_trs_type_t;

/* TRS polarity at boot. Never changed automatically. */
#define TILES_DIN_MIDI_OUT_DEFAULT_TYPE TILES_DIN_MIDI_TRS_TYPE_A

/* Claims a PIO state machine and UART0 and starts both directions. Call
 * after board_init() (GP0/GP2 parked high, GP1 input). On failure DIN
 * stays disabled and every call here is a no-op. */
bool tiles_din_midi_init(void);

bool tiles_din_midi_is_ready(void);

/* Switches the TRS polarity (the other line is parked high). Driven by the
 * `midi.din_trs_type` setting. Bytes in the transmitter's FIFO are
 * discarded. */
void tiles_din_midi_set_trs_type(tiles_din_midi_trs_type_t type);
tiles_din_midi_trs_type_t tiles_din_midi_get_trs_type(void);

/* Queues one channel-voice message (1-3 bytes) or one Real-Time byte for
 * DIN OUT. No-op if DIN isn't ready. See din_midi_queue.h for what gets
 * coalesced. */
void tiles_din_midi_send(const uint8_t *msg, uint8_t len);

/* Call every main-loop pass: flushes coalesced values, keeps the
 * transmitter running, and sends Active Sensing (0xFE) after ~250 ms of
 * silence (DIN only; see din_midi.c). */
void tiles_din_midi_service(void);

/* RX side, for midi/midi_in.c: pops one byte; false if none. */
bool tiles_din_midi_rx_read_byte(uint8_t *byte);

/* True once if RX bytes were lost since the last call (ring overflow,
 * UART framing/break/overrun). */
bool tiles_din_midi_rx_take_loss(void);
