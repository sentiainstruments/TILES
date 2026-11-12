#pragma once

/* One drum sample slot in flash: a 256-byte header page, then the audio
 * (16-bit little-endian at 48 kHz, stereo interleaved L/R). The header is
 * programmed last, after the audio's CRC checked out, so a slot cut off
 * mid-load reads as empty. Plain C, tested in test/test_sampler.c.
 * Header, little-endian: 0 u32 magic, 4 u16 layout version, 6 u8
 * channels, 7 u8 bits, 8 u32 rate, 12 u32 frames, 16 u32 audio CRC-32,
 * 20 name (24 bytes, NUL-padded), 44 u32 CRC-32 of bytes 0-43. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_SAMPLE_SLOT_COUNT 8u
#define TILES_SAMPLE_SLOT_BYTES (192u * 1024u)
#define TILES_SAMPLE_HEADER_BYTES 256u
#define TILES_SAMPLE_RATE_HZ 48000u
#define TILES_SAMPLE_NAME_MAX 23u
/* The length caps: 2 s mono, 1 s stereo (both fit the slot). */
#define TILES_SAMPLE_MAX_FRAMES_MONO 96000u
#define TILES_SAMPLE_MAX_FRAMES_STEREO 48000u

#define TILES_SAMPLE_MAGIC 0x314D5354u /* "TSM1" */
#define TILES_SAMPLE_LAYOUT 1u

typedef struct {
    uint8_t channels; /* 1 or 2 */
    uint32_t frames;
    uint32_t audio_crc;
    char name[TILES_SAMPLE_NAME_MAX + 1u];
} tiles_sample_info_t;

/* Frames a slot takes for `channels` (0 if channels isn't 1 or 2). */
uint32_t tiles_sample_max_frames(uint8_t channels);

/* Audio bytes after the header. */
uint32_t tiles_sample_audio_bytes(const tiles_sample_info_t *info);

/* True if `info` can be stored: 1-2 channels, 1..cap frames, a name of
 * printable ASCII without spaces. */
bool tiles_sample_info_valid(const tiles_sample_info_t *info);

/* Writes the header page (unused bytes 0xFF). False if info isn't valid. */
bool tiles_sample_header_encode(const tiles_sample_info_t *info, uint8_t out[TILES_SAMPLE_HEADER_BYTES]);

/* Reads a header page; false for an empty, cut-off or foreign slot. */
bool tiles_sample_header_decode(const uint8_t *in, tiles_sample_info_t *info);
