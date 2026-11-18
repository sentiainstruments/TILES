#pragma once

/* Single-producer, single-consumer ring of 32-bit words between the two
 * cores: core 0 -> core 1 for MIDI, core 1 -> core 0 for audio frames.
 * Each index is written by one side only; acquire/release ordering makes
 * the data visible before the index that publishes it. Header-only so the
 * host tests use the same code. */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *buf;
    uint32_t mask; /* capacity - 1; capacity in words, a power of two */
    uint32_t head; /* producer only */
    uint32_t tail; /* consumer only */
} tiles_spsc_t;

static inline void tiles_spsc_init(tiles_spsc_t *r, uint32_t *buf, uint32_t capacity_words) {
    r->buf = buf;
    r->mask = capacity_words - 1u;
    r->head = 0;
    r->tail = 0;
}

/* Words waiting (consumer side). */
static inline uint32_t tiles_spsc_count(const tiles_spsc_t *r) {
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    return head - r->tail;
}

/* Words free (producer side). */
static inline uint32_t tiles_spsc_space(const tiles_spsc_t *r) {
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return r->mask + 1u - (r->head - tail);
}

/* All n words or none. */
static inline bool tiles_spsc_push(tiles_spsc_t *r, const uint32_t *words, uint32_t n) {
    if (tiles_spsc_space(r) < n) {
        return false;
    }
    uint32_t head = r->head;
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(head + i) & r->mask] = words[i];
    }
    __atomic_store_n(&r->head, head + n, __ATOMIC_RELEASE);
    return true;
}

/* Up to n words; returns how many. */
static inline uint32_t tiles_spsc_pop(tiles_spsc_t *r, uint32_t *words, uint32_t n) {
    uint32_t available = tiles_spsc_count(r);
    if (n > available) {
        n = available;
    }
    uint32_t tail = r->tail;
    for (uint32_t i = 0; i < n; i++) {
        words[i] = r->buf[(tail + i) & r->mask];
    }
    __atomic_store_n(&r->tail, tail + n, __ATOMIC_RELEASE);
    return n;
}
