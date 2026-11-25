#pragma once

/* The drum sampler on the board: its slots in flash (storage/flash_map.h)
 * and its streaming, by DMA from the XIP stream FIFO (it reads flash in
 * idle cycles, around core 0's own code fetches, and leaves the cache
 * alone). Every flash write stops the stream first (storage_flash.h). */

/* Claims a DMA channel, finds the slots, readies the sampler. Before
 * core 1 starts. */
void tiles_sample_stream_init(void);
