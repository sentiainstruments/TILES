#pragma once

/* The sound TILES makes itself: a placeholder MPE test voice (sine with a
 * little harmonic colour) that plays the MIDI TILES sends, on four tracks
 * plus drums (docs/architecture/audio-engine.md, "Tracks"). Plain C with
 * no Pico dependencies, so test/test_audio_engine.c runs it on the
 * computer. Runs on core 1 (audio/usb_audio.c); the real engines replace
 * the voice code, not this interface. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_AUDIO_RATE_HZ 48000u
#define TILES_AUDIO_TRACKS 4u
#define TILES_AUDIO_TRACK_DRUMS TILES_AUDIO_TRACKS /* the kit: no voices here yet */
#define TILES_AUDIO_VOICES 32u      /* slots; the budget decides how many sound */
#define TILES_AUDIO_TRACK_VOICES 8u /* one track's most */
#define TILES_AUDIO_DRUM_VOICES 5u  /* drum hits ringing at once (the drum engine) */

/* The guardrails (docs/architecture/audio-engine.md, "Guardrails"): every
 * voice together stays under TARGET % of core 1, after the reserve for the
 * effects bus and 5 drums (measured on unit 2: 5 synthesized drums with
 * reverb, delay, chorus and master, ~1,200 cycles per sample); the live
 * guard ends voices above CEILING %. */
#define TILES_AUDIO_TARGET_PCT 85u
#define TILES_AUDIO_CEILING_PCT 90u
#define TILES_AUDIO_RESERVE_CYCLES 1200u

/* Builds the sine table; call once before anything else. bend_range is the
 * semitones a full pitch bend means (what TILES announces to receivers).
 * clk_mhz sets the budget; now_us (NULL: none) times each track's voices
 * live, to learn their cost and run the load guard. */
void tiles_audio_engine_init(uint8_t bend_range_semitones, uint32_t clk_mhz, uint32_t (*now_us)(void));

/* One channel message (status 0x80-0xEF). Note on/off, poly and channel
 * pressure, pitch bend, CC 64 (sustain), CC 120/123 (all off). */
void tiles_audio_engine_midi(uint8_t status, uint8_t data1, uint8_t data2);

/* Renders frames of interleaved stereo, signed 24-bit audio in the top 24
 * bits of each int32 (USB audio's 4-byte subslot layout). */
void tiles_audio_engine_render(int32_t *out_lr, uint32_t frames);

/* Voices still sounding (releases included). */
uint8_t tiles_audio_engine_active_voices(void);

/* The track a channel plays on (0-3, or TILES_AUDIO_TRACK_DRUMS) and
 * whether it is a sequencer lane's: lane N is track N; melodic, Duet's
 * melody, bass guitar and Song share lane 2's track, chords lane 3's, the
 * physics modes lane 4's. */
uint8_t tiles_audio_engine_track(uint8_t channel, bool *from_lane);

/* Voices sounding on one track (releases included). */
uint8_t tiles_audio_engine_track_voices(uint8_t track);

/* A track's cost per voice in cycles per sample: set when an engine loads
 * (its measured figure), then followed live. */
void tiles_audio_engine_set_track_cost(uint8_t track, uint32_t cycles);
uint32_t tiles_audio_engine_track_cost(uint8_t track);

/* Voices the load guard has ended since boot. */
uint32_t tiles_audio_engine_shed_count(void);
