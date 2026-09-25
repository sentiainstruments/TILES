#include "din_midi_queue.h"

#include <stddef.h>

/* Ring indices are volatile and only ever advanced by their one owner (see
 * the header's threading note). The barrier keeps the compiler from moving
 * the buffer write after the index publish (or the buffer read before the
 * index check) -- volatile alone only orders volatile accesses against each
 * other, and the buffers are plain arrays. Same core, so no hardware fence
 * is needed. */
#define DIN_COMPILER_BARRIER() __asm__ volatile("" ::: "memory")

#define TX_MASK (TILES_DIN_TX_RING_SIZE - 1u)
#define RT_MASK (TILES_DIN_TX_RT_RING_SIZE - 1u)
#define RX_MASK (TILES_DIN_RX_RING_SIZE - 1u)

static uint8_t s_tx_buf[TILES_DIN_TX_RING_SIZE];
static volatile uint16_t s_tx_head; /* written by main */
static volatile uint16_t s_tx_tail; /* written by ISR */

static uint8_t s_rt_buf[TILES_DIN_TX_RT_RING_SIZE];
static volatile uint16_t s_rt_head;
static volatile uint16_t s_rt_tail;

static uint8_t s_rx_buf[TILES_DIN_RX_RING_SIZE];
static volatile uint16_t s_rx_head; /* written by ISR */
static volatile uint16_t s_rx_tail; /* written by main */
static volatile bool s_rx_overflow;

static uint32_t s_tx_dropped;

/* ---- coalescing slots ----
 * One per (channel, continuous kind). `dirty` = a value is waiting to go out.
 * Only ever touched from main context (push_message / service), so no
 * cross-context sharing to protect. */
typedef enum {
    KIND_PITCH_BEND = 0,
    KIND_CHANNEL_PRESSURE,
    KIND_CC_MOD,        /* CC1  */
    KIND_CC_EXPRESSION, /* CC11 */
    KIND_CC_SLIDE,      /* CC74 -- MPE's Y dimension */
    KIND_COUNT
} continuous_kind_t;

#define NUM_CHANNELS 16u
#define NUM_SLOTS (NUM_CHANNELS * (uint8_t)KIND_COUNT)

typedef struct {
    uint8_t d1;
    uint8_t d2;
    bool dirty;
} slot_t;

static slot_t s_slots[NUM_CHANNELS][KIND_COUNT];
static uint8_t s_scan_pos;
/* How many slots are dirty right now -- lets service() (called every main-
 * loop iteration) return immediately in the overwhelmingly common case that
 * nothing is waiting, instead of scanning all 80 slots. */
static uint8_t s_dirty_count;

void tiles_din_queue_init(void) {
    s_tx_head = 0u;
    s_tx_tail = 0u;
    s_rt_head = 0u;
    s_rt_tail = 0u;
    s_rx_head = 0u;
    s_rx_tail = 0u;
    s_rx_overflow = false;
    s_tx_dropped = 0u;
    s_scan_pos = 0u;
    s_dirty_count = 0u;
    for (uint8_t c = 0u; c < NUM_CHANNELS; c++) {
        for (uint8_t k = 0u; k < (uint8_t)KIND_COUNT; k++) {
            s_slots[c][k].dirty = false;
        }
    }
}

/* ---- TX ring (producer: main) ---- */

static uint16_t tx_count(void) {
    return (uint16_t)((s_tx_head - s_tx_tail) & TX_MASK);
}

static uint16_t tx_free(void) {
    /* One slot stays empty so head == tail always means "empty". */
    return (uint16_t)(TILES_DIN_TX_RING_SIZE - 1u - tx_count());
}

/* Whole-message enqueue: either every byte goes in, or none do (and the
 * drop is counted). The head is published once, after the last byte, so the
 * consumer never sees half a message. */
static bool tx_push(const uint8_t *msg, uint8_t len) {
    if (tx_free() < len) {
        s_tx_dropped++;
        return false;
    }
    uint16_t head = s_tx_head;
    for (uint8_t i = 0u; i < len; i++) {
        s_tx_buf[(head + i) & TX_MASK] = msg[i];
    }
    DIN_COMPILER_BARRIER();
    s_tx_head = (uint16_t)((head + len) & TX_MASK);
    return true;
}

static bool rt_push(uint8_t byte) {
    uint16_t head = s_rt_head;
    uint16_t next = (uint16_t)((head + 1u) & RT_MASK);
    if (next == s_rt_tail) {
        return false; /* full; a Real-Time byte is dropped, never blocks */
    }
    s_rt_buf[head] = byte;
    DIN_COMPILER_BARRIER();
    s_rt_head = next;
    return true;
}

/* ---- message classification ---- */

static bool classify_continuous(const uint8_t *msg, uint8_t len, continuous_kind_t *kind) {
    uint8_t type = (uint8_t)(msg[0] & 0xF0u);
    if (type == 0xE0u && len == 3u) {
        *kind = KIND_PITCH_BEND;
        return true;
    }
    if (type == 0xD0u && len == 2u) {
        *kind = KIND_CHANNEL_PRESSURE;
        return true;
    }
    if (type == 0xB0u && len == 3u) {
        switch (msg[1]) {
        case 1u:
            *kind = KIND_CC_MOD;
            return true;
        case 11u:
            *kind = KIND_CC_EXPRESSION;
            return true;
        case 74u:
            *kind = KIND_CC_SLIDE;
            return true;
        default:
            break;
        }
    }
    return false;
}

/* Builds the wire message for one slot into out[]; returns its length. */
static uint8_t slot_to_message(uint8_t channel, continuous_kind_t kind, const slot_t *s, uint8_t out[3]) {
    switch (kind) {
    case KIND_PITCH_BEND:
        out[0] = (uint8_t)(0xE0u | channel);
        out[1] = s->d1;
        out[2] = s->d2;
        return 3u;
    case KIND_CHANNEL_PRESSURE:
        out[0] = (uint8_t)(0xD0u | channel);
        out[1] = s->d1;
        return 2u;
    case KIND_CC_MOD:
        out[0] = (uint8_t)(0xB0u | channel);
        out[1] = 1u;
        out[2] = s->d1;
        return 3u;
    case KIND_CC_EXPRESSION:
        out[0] = (uint8_t)(0xB0u | channel);
        out[1] = 11u;
        out[2] = s->d1;
        return 3u;
    case KIND_CC_SLIDE:
    default:
        out[0] = (uint8_t)(0xB0u | channel);
        out[1] = 74u;
        out[2] = s->d1;
        return 3u;
    }
}

/* Pushes one slot's pending value if the ring has room for it. Leaves it
 * dirty (to try again later) if not -- a coalesced value is never counted as
 * dropped, it just waits. */
static bool flush_slot(uint8_t channel, uint8_t kind) {
    slot_t *s = &s_slots[channel][kind];
    if (!s->dirty) {
        return false;
    }
    uint8_t msg[3];
    uint8_t len = slot_to_message(channel, (continuous_kind_t)kind, s, msg);
    if (tx_free() < len) {
        return false;
    }
    (void)tx_push(msg, len);
    s->dirty = false;
    s_dirty_count--;
    return true;
}

static void flush_channel(uint8_t channel) {
    for (uint8_t k = 0u; k < (uint8_t)KIND_COUNT; k++) {
        (void)flush_slot(channel, k);
    }
}

bool tiles_din_queue_push_message(const uint8_t *msg, uint8_t len) {
    if (msg == NULL || len == 0u) {
        return false;
    }
    uint8_t status = msg[0];

    if (status >= 0xF8u) {
        return len == 1u && rt_push(status);
    }
    if (status < 0x80u || status >= 0xF0u || len > 3u) {
        return false; /* SysEx / System Common / malformed: not handled here */
    }

    uint8_t channel = (uint8_t)(status & 0x0Fu);
    continuous_kind_t kind;
    if (classify_continuous(msg, len, &kind)) {
        slot_t *s = &s_slots[channel][kind];
        /* CCs carry their value in data2; everything else in d1/d2 as sent. */
        if ((status & 0xF0u) == 0xB0u) {
            s->d1 = msg[2];
            s->d2 = 0u;
        } else {
            s->d1 = msg[1];
            s->d2 = (len == 3u) ? msg[2] : 0u;
        }
        if (!s->dirty) {
            s->dirty = true;
            s_dirty_count++;
        }
        return true;
    }

    /* Reliable: anything already pending on this channel goes first. */
    flush_channel(channel);
    return tx_push(msg, len);
}

bool tiles_din_queue_service(void) {
    if (s_dirty_count == 0u || tx_count() > TILES_DIN_TX_FLUSH_THRESHOLD_BYTES) {
        return false;
    }
    bool queued = false;
    /* Scan every slot at most once per call, resuming where the last call
     * stopped so a busy channel can't starve the others. Stop as soon as the
     * ring is no longer shallow. */
    uint8_t start = s_scan_pos;
    for (uint8_t n = 0u; n < NUM_SLOTS; n++) {
        if (tx_count() > TILES_DIN_TX_FLUSH_THRESHOLD_BYTES) {
            break;
        }
        uint8_t pos = (uint8_t)((start + n) % NUM_SLOTS);
        uint8_t channel = (uint8_t)(pos / (uint8_t)KIND_COUNT);
        uint8_t kind = (uint8_t)(pos % (uint8_t)KIND_COUNT);
        if (flush_slot(channel, kind)) {
            queued = true;
            s_scan_pos = (uint8_t)((pos + 1u) % NUM_SLOTS);
        }
    }
    return queued;
}

/* ---- TX consumer (interrupt) ---- */

bool tiles_din_queue_pop_tx_byte(uint8_t *out) {
    uint16_t rt_tail = s_rt_tail;
    if (rt_tail != s_rt_head) {
        DIN_COMPILER_BARRIER();
        *out = s_rt_buf[rt_tail];
        DIN_COMPILER_BARRIER();
        s_rt_tail = (uint16_t)((rt_tail + 1u) & RT_MASK);
        return true;
    }
    uint16_t tail = s_tx_tail;
    if (tail != s_tx_head) {
        DIN_COMPILER_BARRIER();
        *out = s_tx_buf[tail];
        DIN_COMPILER_BARRIER();
        s_tx_tail = (uint16_t)((tail + 1u) & TX_MASK);
        return true;
    }
    return false;
}

bool tiles_din_queue_tx_has_data(void) {
    return s_rt_tail != s_rt_head || s_tx_tail != s_tx_head;
}

uint32_t tiles_din_queue_tx_pending_bytes(void) {
    return (uint32_t)tx_count() + (uint32_t)((s_rt_head - s_rt_tail) & RT_MASK);
}

uint32_t tiles_din_queue_tx_dropped(void) {
    return s_tx_dropped;
}

/* ---- RX ---- */

bool tiles_din_queue_rx_push(uint8_t byte) {
    uint16_t head = s_rx_head;
    uint16_t next = (uint16_t)((head + 1u) & RX_MASK);
    if (next == s_rx_tail) {
        s_rx_overflow = true;
        return false;
    }
    s_rx_buf[head] = byte;
    DIN_COMPILER_BARRIER();
    s_rx_head = next;
    return true;
}

bool tiles_din_queue_rx_pop(uint8_t *out) {
    uint16_t tail = s_rx_tail;
    if (tail == s_rx_head) {
        return false;
    }
    DIN_COMPILER_BARRIER();
    *out = s_rx_buf[tail];
    DIN_COMPILER_BARRIER();
    s_rx_tail = (uint16_t)((tail + 1u) & RX_MASK);
    return true;
}

void tiles_din_queue_rx_flag_loss(void) {
    s_rx_overflow = true;
}

bool tiles_din_queue_rx_take_overflow(void) {
    bool was = s_rx_overflow;
    s_rx_overflow = false;
    return was;
}
