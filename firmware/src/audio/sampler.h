#pragma once

/* The drum sampler: 5 voices playing the 8 sample slots (sample_slot.h)
 * on core 1. A slot's first 1 KB waits in RAM, so a hit starts at once;
 * the rest streams from flash into a 2 KB ring per voice, one 512-byte
 * chunk in flight at a time. Plain C: the fetch is passed in (DMA from
 * flash on the board, sample_stream.c; a copy in test/test_sampler.c). */

#include <stdbool.h>
#include <stdint.h>

#include "audio_engine.h"
#include "sample_slot.h"

#define TILES_SAMPLER_VOICES TILES_AUDIO_DRUM_VOICES
#define TILES_SAMPLER_RING_BYTES 2048u
#define TILES_SAMPLER_CHUNK_BYTES 512u
#define TILES_SAMPLER_HEAD_BYTES 1024u

typedef struct {
    /* The chunk last started hasn't landed yet. */
    bool (*busy)(void);
    /* Starts copying `words` 32-bit words from flash (an offset from its
     * start) to dst. False: not now (flash is being written). */
    bool (*start)(uint32_t flash_offset, uint32_t *dst, uint32_t words);
} tiles_sampler_fetch_t;

void tiles_sampler_init(const tiles_sampler_fetch_t *fetch);

/* Core 0, when a slot's contents change: empty it (its voices stop), or
 * fill it: the audio at flash_offset, `head` its first bytes (up to
 * TILES_SAMPLER_HEAD_BYTES; copied). */
void tiles_sampler_slot_clear(uint8_t slot);
void tiles_sampler_slot_set(uint8_t slot, uint8_t channels, uint32_t frames, uint32_t flash_offset,
                            const uint8_t *head);

/* Core 1: a hit on slot 0-7. The slot's own voice restarts if it rings;
 * otherwise a free voice, or the oldest. An empty slot plays nothing. */
void tiles_sampler_hit(uint8_t slot, uint8_t velocity);

/* Core 1: adds n frames of the ringing voices to l and r (full scale 1.0),
 * and keeps the streaming going. */
void tiles_sampler_render(float *l, float *r, uint32_t n);

uint8_t tiles_sampler_voices(void);

/* Core 1, the granular tape: copies a slot's audio, mixed to mono, into
 * dst (room for `room` samples, at least TILES_SAMPLER_CHUNK_BYTES / 2 of
 * them spare for a stereo slot), the head at once and the rest a chunk at
 * a time when no voice needs one. A new copy replaces the last. */
void tiles_sampler_copy_start(uint8_t slot, int16_t *dst, uint32_t room);
void tiles_sampler_copy_stop(void);
/* Samples copied so far (0: none, or the slot changed under it). */
uint32_t tiles_sampler_copy_frames(void);
/* `slot` if it holds audio, else the last slot that does; -1: none. */
int32_t tiles_sampler_loaded_slot(uint8_t slot);
/* Changes with every change of a slot's contents. */
uint32_t tiles_sampler_slot_gen(uint8_t slot);
/* Blocks where a voice ran out of streamed data (it waits, then goes on). */
uint32_t tiles_sampler_underruns(void);
