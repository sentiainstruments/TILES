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

/* A drum hit for the sampler: slot 0-7 (drum 1-8 of the first bank),
 * velocity 1-127. Queued for core 1 like the messages above. */
void tiles_usb_audio_drum_hit(uint8_t slot, uint8_t velocity);

typedef struct {
    bool streaming;       /* a host has the input open */
    uint32_t packets;     /* sent since boot */
    uint32_t late_frames; /* USB frames with no packet, made up in the next */
    uint32_t underruns;   /* packets padded with silence: core 1 behind */
    uint32_t midi_drops;  /* messages lost to a full queue */
    uint8_t voices;       /* sounding now */
    uint8_t load_pct;     /* core 1 time spent rendering, last second */
    uint32_t shed;        /* voices the load guard ended */
    uint16_t cost[4];     /* each track's measured cycles per voice-sample */
    uint8_t drum_voices;  /* drum samples ringing now */
    uint32_t drum_underruns; /* blocks a drum voice ran out of streamed data */
} tiles_usb_audio_info_t;

tiles_usb_audio_info_t tiles_usb_audio_get_info(void);

/* True while nothing TILES plays itself can be heard: no host has the
 * input open, or no voice sounds. Flash erases (tens of ms, core 1
 * paused) wait for this. */
bool tiles_usb_audio_is_quiet(void);

#if TILES_DSP_BENCH
#include "dsp_bench.h"
/* Bench builds: core 1 runs the DSP stress test (a few seconds). */
void tiles_usb_audio_bench_start(void);
/* NULL until the last run finished. */
const tiles_dsp_bench_t *tiles_usb_audio_bench_result(void);
/* Core 1 streams the 8 sample slots from flash for `ms` (dsp_bench.h). */
void tiles_usb_audio_bench_soak(uint32_t ms, bool cached);
#endif
