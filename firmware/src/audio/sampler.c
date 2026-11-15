#include "sampler.h"

#include "audio_hot.h"

#include <string.h>

_Static_assert(TILES_SAMPLER_RING_BYTES % TILES_SAMPLER_CHUNK_BYTES == 0u, "chunks never straddle the ring's end");
_Static_assert(TILES_SAMPLER_HEAD_BYTES % TILES_SAMPLER_CHUNK_BYTES == 0u, "streaming starts on a chunk");
_Static_assert(TILES_SAMPLER_HEAD_BYTES < TILES_SAMPLER_RING_BYTES, "the head fits the ring with room to stream");
_Static_assert((TILES_SAMPLER_RING_BYTES & (TILES_SAMPLER_RING_BYTES - 1u)) == 0u, "ring is a power of two");

/* Written by core 0 only while `ready` is false; core 1 reads it after
 * seeing ready (acquire). gen changes with every change of contents. */
typedef struct {
    bool ready;
    uint32_t gen;
    uint8_t channels;
    uint32_t bytes;
    uint32_t flash_offset;
} slot_t;

typedef struct {
    bool active;
    uint8_t slot;
    uint8_t channels;
    uint32_t gen;
    uint32_t serial; /* a restart's number: a chunk started before it is dropped */
    uint32_t bytes;
    uint32_t flash_offset;
    uint32_t fetched;  /* audio bytes in the ring, counted from the start */
    uint32_t consumed; /* audio bytes played */
    float gl, gr;
    uint32_t age;
    uint32_t ring[TILES_SAMPLER_RING_BYTES / 4u];
} voice_t;

static slot_t s_slot[TILES_SAMPLE_SLOT_COUNT];
static uint32_t s_head[TILES_SAMPLE_SLOT_COUNT][TILES_SAMPLER_HEAD_BYTES / 4u];
static voice_t s_voice[TILES_SAMPLER_VOICES];
static const tiles_sampler_fetch_t *s_fetch;
static uint32_t s_age;
static uint32_t s_underruns;

static struct {
    bool active;
    uint8_t voice;
    uint32_t serial;
    uint32_t bytes;
} s_inflight;

void tiles_sampler_init(const tiles_sampler_fetch_t *fetch) {
    memset(s_slot, 0, sizeof(s_slot));
    memset(s_voice, 0, sizeof(s_voice));
    memset(&s_inflight, 0, sizeof(s_inflight));
    s_fetch = fetch;
    s_age = 0;
    s_underruns = 0;
}

void tiles_sampler_slot_clear(uint8_t slot) {
    if (slot >= TILES_SAMPLE_SLOT_COUNT) {
        return;
    }
    __atomic_store_n(&s_slot[slot].ready, false, __ATOMIC_RELEASE);
    __atomic_add_fetch(&s_slot[slot].gen, 1u, __ATOMIC_RELEASE);
}

void tiles_sampler_slot_set(uint8_t slot, uint8_t channels, uint32_t frames, uint32_t flash_offset,
                            const uint8_t *head) {
    if (slot >= TILES_SAMPLE_SLOT_COUNT || (channels != 1u && channels != 2u)) {
        return;
    }
    tiles_sampler_slot_clear(slot);
    slot_t *s = &s_slot[slot];
    s->channels = channels;
    s->bytes = frames * channels * 2u;
    s->flash_offset = flash_offset;
    uint32_t n = s->bytes < TILES_SAMPLER_HEAD_BYTES ? s->bytes : TILES_SAMPLER_HEAD_BYTES;
    memset(s_head[slot], 0, sizeof(s_head[slot]));
    memcpy(s_head[slot], head, n);
    __atomic_store_n(&s->ready, true, __ATOMIC_RELEASE);
}

/* Lands the chunk in flight, then starts one for the voice with the least
 * audio waiting that has room for a chunk. */
static void TILES_AUDIO_HOT(service_fetch)(void) {
    if (s_fetch == NULL) {
        return;
    }
    if (s_inflight.active) {
        if (s_fetch->busy()) {
            return;
        }
        voice_t *v = &s_voice[s_inflight.voice];
        if (v->active && v->serial == s_inflight.serial) {
            v->fetched += s_inflight.bytes;
        }
        s_inflight.active = false;
    }
    voice_t *best = NULL;
    uint32_t best_waiting = UINT32_MAX;
    for (uint32_t i = 0; i < TILES_SAMPLER_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (!v->active || v->fetched >= v->bytes) {
            continue;
        }
        uint32_t waiting = v->fetched - v->consumed;
        if (TILES_SAMPLER_RING_BYTES - waiting >= TILES_SAMPLER_CHUNK_BYTES && waiting < best_waiting) {
            best = v;
            best_waiting = waiting;
        }
    }
    if (best == NULL) {
        return;
    }
    uint32_t bytes = best->bytes - best->fetched;
    bytes = bytes < TILES_SAMPLER_CHUNK_BYTES ? (bytes + 3u) & ~3u : TILES_SAMPLER_CHUNK_BYTES;
    uint32_t *dst = &best->ring[(best->fetched % TILES_SAMPLER_RING_BYTES) / 4u];
    if (s_fetch->start(best->flash_offset + best->fetched, dst, bytes / 4u)) {
        s_inflight.active = true;
        s_inflight.voice = (uint8_t)(best - s_voice);
        s_inflight.serial = best->serial;
        s_inflight.bytes = bytes;
    }
}

void tiles_sampler_hit(uint8_t slot, uint8_t velocity) {
    if (slot >= TILES_SAMPLE_SLOT_COUNT || velocity == 0u) {
        return;
    }
    slot_t *s = &s_slot[slot];
    if (!__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE)) {
        return;
    }
    voice_t *v = NULL, *free_voice = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < TILES_SAMPLER_VOICES; i++) {
        voice_t *c = &s_voice[i];
        if (!c->active) {
            if (free_voice == NULL) {
                free_voice = c;
            }
        } else if (c->slot == slot) {
            v = c;
        } else if (oldest == NULL || c->age < oldest->age) {
            oldest = c;
        }
    }
    if (v == NULL) {
        v = free_voice != NULL ? free_voice : oldest;
    }
    /* A chunk still landing in this voice's ring would overwrite the head. */
    if (s_inflight.active && &s_voice[s_inflight.voice] == v) {
        while (s_fetch->busy()) {
        }
        s_inflight.active = false;
    }
    v->slot = slot;
    v->channels = s->channels;
    v->gen = __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE);
    v->serial++;
    v->bytes = s->bytes;
    v->flash_offset = s->flash_offset;
    v->fetched = v->bytes < TILES_SAMPLER_HEAD_BYTES ? v->bytes : TILES_SAMPLER_HEAD_BYTES;
    v->consumed = 0;
    memcpy(v->ring, s_head[slot], TILES_SAMPLER_HEAD_BYTES);
    float g = (float)velocity / 127.0f;
    g *= g;                                      /* a soft hit is clearly quieter */
    float pan = v->channels == 1u ? 0.70710678f : 1.0f; /* mono: centred, equal power */
    v->gl = g * pan * (1.0f / 32768.0f);
    v->gr = v->gl;
    v->age = ++s_age;
    v->active = true;
    service_fetch();
}

void TILES_AUDIO_HOT(tiles_sampler_render)(float *l, float *r, uint32_t n) {
    service_fetch();
    for (uint32_t vi = 0; vi < TILES_SAMPLER_VOICES; vi++) {
        voice_t *v = &s_voice[vi];
        if (!v->active) {
            continue;
        }
        slot_t *s = &s_slot[v->slot];
        if (!__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != v->gen) {
            v->active = false; /* reloaded or deleted under it */
            continue;
        }
        uint32_t frame_bytes = v->channels * 2u;
        uint32_t left = (v->bytes - v->consumed) / frame_bytes;
        uint32_t ready = (v->fetched - v->consumed) / frame_bytes;
        uint32_t want = n < left ? n : left;
        uint32_t k = want < ready ? want : ready;
        if (k < want) {
            s_underruns++;
        }
        const int16_t *ring = (const int16_t *)v->ring;
        const uint32_t mask = TILES_SAMPLER_RING_BYTES / 2u - 1u;
        uint32_t idx = v->consumed / 2u;
        if (v->channels == 2u) {
            for (uint32_t i = 0; i < k; i++) {
                l[i] += (float)ring[idx & mask] * v->gl;
                r[i] += (float)ring[(idx + 1u) & mask] * v->gr;
                idx += 2u;
            }
        } else {
            for (uint32_t i = 0; i < k; i++) {
                float x = (float)ring[idx & mask];
                l[i] += x * v->gl;
                r[i] += x * v->gr;
                idx++;
            }
        }
        v->consumed += k * frame_bytes;
        if (v->consumed >= v->bytes) {
            v->active = false;
        }
    }
    service_fetch();
}

uint8_t tiles_sampler_voices(void) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < TILES_SAMPLER_VOICES; i++) {
        count += s_voice[i].active ? 1u : 0u;
    }
    return count;
}

uint32_t tiles_sampler_underruns(void) {
    return s_underruns;
}
