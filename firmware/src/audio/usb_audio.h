#pragma once

/* USB audio: TILES as a stereo 48 kHz / 24-bit input on the computer, the
 * test voice (audio_engine.h) rendered on core 1. README.md has the design. */

#include <stdbool.h>
#include <stdint.h>

/* Shared with the descriptor (midi/usb_descriptors.c). */
#define TILES_AUDIO_ENTITY_INPUT 0x01u  /* the test voice, a "synthesizer" terminal */
#define TILES_AUDIO_ENTITY_OUTPUT 0x03u /* USB streaming */
#define TILES_AUDIO_ENTITY_CLOCK 0x04u  /* fixed 48 kHz */
#define TILES_AUDIO_EP_IN 0x85u        /* the isochronous stream */
#define TILES_AUDIO_CHANNELS 2u
#define TILES_AUDIO_BYTES_PER_SAMPLE 4u /* 24 bits in a 4-byte subslot */
#define TILES_AUDIO_BITS_PER_SAMPLE 24u
/* Packets are 48 frames; the endpoint allows the 49 an asynchronous
 * 48 kHz endpoint may send. */
#define TILES_AUDIO_MAX_PACKET_FRAMES 49u
#define TILES_AUDIO_EP_SIZE (TILES_AUDIO_MAX_PACKET_FRAMES * TILES_AUDIO_CHANNELS * TILES_AUDIO_BYTES_PER_SAMPLE)

/* Standalone mode, first thing at boot (services/device_mode.h): starts core
 * 1, which brings up USB with the audio input and runs tud_task() every
 * pass (~0.2 ms) next to the engine. Returns once USB is up. */
void tiles_usb_audio_start(void);

/* A channel message TILES sends (midi_out.c), queued for core 1. Never
 * waits: a full queue drops the message and counts it. */
void tiles_usb_audio_midi(uint8_t status, uint8_t data1, uint8_t data2);

/* A drum hit for the sampler: slot 0-7 (drum 1-8 of the first bank),
 * velocity 1-127. Queued for core 1 like the messages above. */
void tiles_usb_audio_drum_hit(uint8_t slot, uint8_t velocity);

/* A track's sound (0-3, a factory patch), in order with the notes around
 * it; false outside standalone mode. */
bool tiles_usb_audio_set_patch(uint8_t track, uint8_t patch);

/* The tempo the synced LFOs follow; the main loop passes it on. */
void tiles_usb_audio_set_tempo(float beats_per_minute);

/* True once core 1 runs the engine (standalone mode). */
bool tiles_usb_audio_started(void);

/* An effect setting (audio_engine.h's TILES_FX_*) to core 1: false if the
 * queue is full or the sound isn't running. */
bool tiles_usb_audio_set_fx(uint8_t target, uint8_t param, float value);

/* Every main-loop pass with the MIDI clock: each new beat while it runs
 * ducks the pump (unless a kick just did). */
void tiles_usb_audio_follow_clock(bool running, uint32_t song_pulse);

typedef struct {
    bool streaming;       /* a host has the input open */
    uint32_t packets;     /* sent since boot */
    uint32_t late_frames; /* USB frames with no packet (their audio is lost) */
    uint32_t underruns;   /* packets padded with silence: core 1 behind */
    uint32_t midi_drops;  /* messages lost to a full queue */
    uint8_t voices;       /* sounding now */
    uint8_t load_pct;     /* core 1 time spent rendering, last second */
    uint32_t shed;        /* voices the load guard ended */
    uint16_t cost[4];     /* each track's measured cycles per voice-sample */
    uint8_t drum_voices;  /* drum samples ringing now */
    uint32_t drum_underruns; /* blocks a drum voice ran out of streamed data */
    uint32_t usb_gap_us;  /* core 1's longest tud_task() to tud_task(), streaming */
    uint32_t fx_cost;     /* the effects' cycles per sample */
    bool fx_lent;         /* their memory lent to the pad capture: quiet */
    uint8_t load_peak_pct; /* the busiest second since boot */
    uint32_t stack_free;  /* core 1's stack never reached, bytes (of 4,096) */
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
