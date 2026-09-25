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
 * polarity. Fixed at TRS TYPE A (the MIDI Association's standard) by default,
 * per docs/architecture/defaults-and-safeguards.md: "Default: Type A TRS
 * polarity... Selectable via profile (GP0/GP2 role swap), not auto-detected --
 * there's no way to sense polarity from the jack side alone." So it is NEVER
 * switched automatically; only an explicit tiles_din_midi_set_trs_type() call
 * (a future profile setting) changes it. Which of GP0/GP2 is Type A follows
 * the handoff's "line A / line B" naming (Type A = line A = GP0); that mapping
 * is inferred, not stated in the docs, so it is the first thing to check if a
 * Type A receiver hears nothing. Independent of USB: DIN works with no host at
 * all (external power only), which is the whole point of the jack.
 *
 * A failed init disables DIN and nothing else (hardware non-negotiable: "a
 * failed subsystem disables itself; it never blocks USB diagnostics").
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_DIN_MIDI_TRS_TYPE_A = 0, /* GP0 carries the data, GP2 held high */
    TILES_DIN_MIDI_TRS_TYPE_B = 1, /* GP2 carries the data, GP0 held high */
} tiles_din_midi_trs_type_t;

/* TRS polarity at boot: Type A, "for now" (real feedback: "dont auto flip
 * select type A for now"). Never changed automatically. See the header
 * comment. */
#define TILES_DIN_MIDI_OUT_DEFAULT_TYPE TILES_DIN_MIDI_TRS_TYPE_A

/* Claims a PIO state machine + UART0, starts both directions. Must run after
 * board_init() (which parks GP0/GP2 high and GP1 as an input). Returns false,
 * leaving DIN disabled and every other call here a no-op, if a resource is
 * unavailable. */
bool tiles_din_midi_init(void);

bool tiles_din_midi_is_ready(void);

/* Explicitly switches the TRS polarity (the other line is parked high).
 * Nothing in the firmware calls this today -- it exists for the future
 * profile setting. Bytes already in the transmitter's FIFO are discarded. */
void tiles_din_midi_set_trs_type(tiles_din_midi_trs_type_t type);
tiles_din_midi_trs_type_t tiles_din_midi_get_trs_type(void);

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
