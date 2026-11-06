#pragma once

/* The sound TILES makes itself: a placeholder MPE test voice (sine with a
 * little harmonic colour) that plays the MIDI TILES sends. Plain C with no
 * Pico dependencies, so test/test_audio_engine.c runs it on the computer.
 * Runs on core 1 (audio/usb_audio.c); the real engines replace the voice
 * code, not this interface. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_AUDIO_RATE_HZ 48000u
#define TILES_AUDIO_VOICES 16u

/* Builds the sine table; call once before anything else. bend_range is the
 * semitones a full pitch bend means (what TILES announces to receivers). */
void tiles_audio_engine_init(uint8_t bend_range_semitones);

/* One channel message (status 0x80-0xEF). Note on/off, poly and channel
 * pressure, pitch bend, CC 64 (sustain), CC 120/123 (all off). */
void tiles_audio_engine_midi(uint8_t status, uint8_t data1, uint8_t data2);

/* Renders frames of interleaved stereo, signed 24-bit audio in the top 24
 * bits of each int32 (USB audio's 4-byte subslot layout). */
void tiles_audio_engine_render(int32_t *out_lr, uint32_t frames);

/* Voices still sounding (releases included). */
uint8_t tiles_audio_engine_active_voices(void);
