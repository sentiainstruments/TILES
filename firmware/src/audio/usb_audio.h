#pragma once

/* USB audio: TILES as a stereo 48 kHz / 24-bit input on the computer, the
 * test voice (audio_engine.h) rendered on core 1. README.md has the design. */

#include <stdbool.h>
#include <stdint.h>

/* Shared with the descriptor (midi/usb_descriptors.c). */
#define TILES_AUDIO_ENTITY_INPUT 0x01u  /* the test voice, a "synthesizer" terminal */
#define TILES_AUDIO_ENTITY_OUTPUT 0x03u /* USB streaming */
#define TILES_AUDIO_ENTITY_CLOCK 0x04u  /* fixed 48 kHz */
#define TILES_AUDIO_CHANNELS 2u
#define TILES_AUDIO_BYTES_PER_SAMPLE 4u /* 24 bits in a 4-byte subslot */
#define TILES_AUDIO_BITS_PER_SAMPLE 24u
/* A packet carries up to two frames' audio, so one missed frame is made
 * up in the next instead of dropped. */
#define TILES_AUDIO_MAX_PACKET_FRAMES 96u
#define TILES_AUDIO_EP_SIZE (TILES_AUDIO_MAX_PACKET_FRAMES * TILES_AUDIO_CHANNELS * TILES_AUDIO_BYTES_PER_SAMPLE)

/* Starts core 1 (the engine) after USB is up; call once, before the main
 * loop. */
void tiles_usb_audio_init(void);

/* A channel message TILES sends (midi_out.c), queued for core 1. Never
 * waits: a full queue drops the message and counts it. */
void tiles_usb_audio_midi(uint8_t status, uint8_t data1, uint8_t data2);

typedef struct {
    bool streaming;       /* a host has the input open */
    uint32_t packets;     /* sent since boot */
    uint32_t late_frames; /* USB frames with no packet, made up in the next */
    uint32_t underruns;   /* packets padded with silence: core 1 behind */
    uint32_t midi_drops;  /* messages lost to a full queue */
    uint8_t voices;       /* sounding now */
    uint8_t load_pct;     /* core 1 time spent rendering, last second */
} tiles_usb_audio_info_t;

tiles_usb_audio_info_t tiles_usb_audio_get_info(void);
