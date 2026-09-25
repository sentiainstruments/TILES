#pragma once

/*
 * DIN MIDI byte queues -- the hardware-free half of midi/din_midi.c.
 *
 * Kept free of any Pico SDK include on purpose: everything in here is pure
 * logic (ring buffers, message classification, coalescing), which is exactly
 * the part that's easy to get subtly wrong and impossible to eyeball on a
 * scope, so it's compiled and exercised natively off-target. midi/din_midi.c
 * is the thin hardware layer (PIO transmitter, UART receiver, IRQs) that
 * feeds and drains these queues.
 *
 * Why the TX side is more than a FIFO. DIN MIDI is 31,250 baud: 3,125
 * bytes/s, about 1 ms per 3-byte message, while USB MIDI is effectively
 * unlimited. This controller streams MPE expression (per-note pitch bend
 * and channel pressure, plus the expression pedal broadcast to 16 channels
 * -- 48 bytes per pedal step), which can easily out-run that wire. So:
 *
 *   - Note On/Off, sustain and every other CC, Program Change... are
 *     RELIABLE: queued in order, never merged, never dropped unless the
 *     queue is genuinely full (counted, see tiles_din_queue_tx_dropped()).
 *     A lost Note-Off is a stuck note, so these get the queue's whole depth.
 *   - Pitch Bend, Channel Pressure and the "continuous" CCs (mod wheel,
 *     expression, slide) are COALESCED: only the latest value per
 *     (channel, kind) is kept, and it goes out when the wire has room. An
 *     intermediate bend value nobody could hear is worth dropping; the FINAL
 *     value (the return to center) is not, and coalescing keeps it.
 *   - Ordering is preserved where it matters: before any reliable message on
 *     a channel is queued, that channel's pending continuous values are
 *     flushed ahead of it, so "bend to center, then Note-Off" and "bend to
 *     center, then Note-On" reach the receiver in that order.
 *   - System Real-Time bytes (Start/Stop/Clock...) jump the queue: a
 *     separate small queue is drained first, and Real-Time bytes are legal
 *     between any two bytes of another message.
 * SysEx and System Common messages are not handled here (the callers don't
 * mirror them to DIN -- see midi/midi_out.c).
 *
 * Threading: each ring has exactly one producer and one consumer, so no
 * locks are needed. TX rings: producer = main context (push/service),
 * consumer = the PIO TX-FIFO interrupt (pop). RX ring: producer = the UART
 * RX interrupt, consumer = main context (tiles_midi_in_scan()).
 */

#include <stdbool.h>
#include <stdint.h>

/* Both ring sizes must be powers of two. */
#define TILES_DIN_RX_RING_SIZE 256u
#define TILES_DIN_TX_RING_SIZE 512u
#define TILES_DIN_TX_RT_RING_SIZE 16u

/* Continuous values only flush into the TX ring while it holds at most this
 * many bytes (~7.7 ms of wire time), so expression never queues in front of
 * a Note-On and the worst-case latency it adds stays small. */
#define TILES_DIN_TX_FLUSH_THRESHOLD_BYTES 24u

void tiles_din_queue_init(void);

/* ---- TX: producer side (main context) ---- */

/* Queues one complete channel-voice message (1-3 bytes: status + data) or a
 * single System Real-Time byte (0xF8-0xFF). Returns false if the message was
 * not accepted (unsupported status such as SysEx/System Common, or the
 * queue had no room for a reliable message -- the latter is counted). A
 * coalesced message always returns true: it replaces the previous value. */
bool tiles_din_queue_push_message(const uint8_t *msg, uint8_t len);

/* Moves pending coalesced values into the TX ring, oldest-scan-position
 * first, only while the ring is shallow (see TILES_DIN_TX_FLUSH_THRESHOLD_
 * BYTES). Call every main-loop iteration. Returns true if it queued
 * anything (so the caller knows to make sure the transmitter is running). */
bool tiles_din_queue_service(void);

/* ---- TX: consumer side (interrupt) ---- */

/* Pops the next byte to put on the wire: Real-Time queue first, then the
 * main ring. Returns false if both are empty. */
bool tiles_din_queue_pop_tx_byte(uint8_t *out);
bool tiles_din_queue_tx_has_data(void);

/* Bytes currently waiting in the TX rings (both), and reliable messages
 * dropped for lack of room since boot. Diagnostics only. */
uint32_t tiles_din_queue_tx_pending_bytes(void);
uint32_t tiles_din_queue_tx_dropped(void);

/* ---- RX ---- */

/* Interrupt-side push of one received byte. Returns false (byte discarded,
 * overflow flag set) if the ring is full. */
bool tiles_din_queue_rx_push(uint8_t byte);

/* Main-context pop. Returns false if empty. */
bool tiles_din_queue_rx_pop(uint8_t *out);

/* Interrupt-side: records that a byte was lost or corrupted (UART framing/
 * break/overrun error) without it ever reaching the ring. Same effect on the
 * parser as a ring overflow -- see tiles_din_queue_rx_take_overflow(). */
void tiles_din_queue_rx_flag_loss(void);

/* True (once) if bytes were lost since the last call -- the parser fed by
 * this ring should treat that as a stream discontinuity and drop any
 * half-assembled message. */
bool tiles_din_queue_rx_take_overflow(void);
