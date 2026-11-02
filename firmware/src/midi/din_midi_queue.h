#pragma once

/* DIN byte queues, the hardware-free half of din_midi.c (tested natively).
 * At 31,250 baud MPE outruns the wire, so:
 *   - RELIABLE (notes, sustain, other CCs): in order, never merged; a lost
 *     Note-Off is a stuck note.
 *   - COALESCED (bend, pressure, continuous CCs): latest per channel and
 *     kind, flushed before any reliable message on that channel.
 *   - Real-Time jumps the queue. No SysEx or System Common.
 * One producer and one consumer per ring, so no locks. */

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
