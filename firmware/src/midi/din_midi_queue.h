#pragma once

/* DIN MIDI byte queues: the hardware-free half of midi/din_midi.c (rings,
 * classification, coalescing), tested natively
 * (firmware/test/test_din_midi_queue.c). din_midi.c is the thin PIO/UART
 * layer that feeds and drains them.
 *
 * Why TX is more than a FIFO: DIN runs at 31,250 baud, ~1 ms per 3-byte
 * message, and MPE expression (per-note bend and pressure, pedal CCs on
 * several channels) easily outruns it. So:
 *
 *   - RELIABLE: Note On/Off, sustain and other CCs, Program Change...
 *     Queued in order, never merged, dropped only if the queue is truly
 *     full (counted). A lost Note-Off is a stuck note.
 *   - COALESCED: Pitch Bend, Channel Pressure and the continuous CCs (mod,
 *     expression, slide). Only the latest value per (channel, kind) is
 *     kept and sent when the wire has room. The final value (e.g. back to
 *     center) is never lost.
 *   - Order kept where it matters: a channel's pending continuous values
 *     are flushed before any reliable message on that channel, so "bend to
 *     center, then Note-Off" arrives in that order.
 *   - System Real-Time bytes jump the queue (a separate small queue drained
 *     first; they are legal between any two bytes).
 * SysEx and System Common are not sent to DIN (midi/midi_out.c).
 *
 * Threading: each ring has one producer and one consumer, so no locks.
 * TX: main context produces, the PIO TX-FIFO interrupt consumes. RX: the
 * UART RX interrupt produces, main (tiles_midi_in_scan()) consumes. */

#include <stdbool.h>
#include <stdint.h>

/* Both ring sizes must be powers of two. */
#define TILES_DIN_RX_RING_SIZE 256u
#define TILES_DIN_TX_RING_SIZE 512u
#define TILES_DIN_TX_RT_RING_SIZE 16u

/* Continuous values flush into the TX ring only while it holds at most
 * this many bytes (~7.7 ms of wire time), so expression never delays a
 * Note-On by much. */
#define TILES_DIN_TX_FLUSH_THRESHOLD_BYTES 24u

void tiles_din_queue_init(void);

/* ---- TX: producer side (main context) ---- */

/* Queues one channel-voice message (1-3 bytes) or one Real-Time byte.
 * False if not accepted (SysEx/System Common, or no room for a reliable
 * message, which is counted). Coalesced messages always return true. */
bool tiles_din_queue_push_message(const uint8_t *msg, uint8_t len);

/* Moves pending coalesced values into the TX ring while it's shallow (see
 * TILES_DIN_TX_FLUSH_THRESHOLD_BYTES). Call every main-loop pass. True if
 * anything was queued (so the caller starts the transmitter). */
bool tiles_din_queue_service(void);

/* ---- TX: consumer side (interrupt) ---- */

/* Next byte for the wire: Real-Time queue first, then the main ring. False
 * if both are empty. */
bool tiles_din_queue_pop_tx_byte(uint8_t *out);
bool tiles_din_queue_tx_has_data(void);

/* Diagnostics: bytes waiting in both TX rings; reliable messages dropped
 * since boot. */
uint32_t tiles_din_queue_tx_pending_bytes(void);
uint32_t tiles_din_queue_tx_dropped(void);

/* ---- RX ---- */

/* Interrupt side. False (byte discarded, overflow flagged) if full. */
bool tiles_din_queue_rx_push(uint8_t byte);

/* Main-context pop. Returns false if empty. */
bool tiles_din_queue_rx_pop(uint8_t *out);

/* Interrupt side: a byte was lost or corrupted (UART framing/break/
 * overrun) before reaching the ring. Same effect as an overflow. */
void tiles_din_queue_rx_flag_loss(void);

/* True once if bytes were lost since the last call; the parser should drop
 * any half-built message. */
bool tiles_din_queue_rx_take_overflow(void);
