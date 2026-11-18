#pragma once

/* The sound TILES makes itself: the wavetable engine (synth_voice.h)
 * playing the MIDI TILES sends, on four tracks plus drums, a sound
 * (patch) per track (docs/architecture/audio-engine.md, "Tracks";
 * synth-engines.md). Plain C with no Pico dependencies, so
 * test/test_audio_engine.c runs it on the computer. Runs on core 1
 * (audio/usb_audio.c). */

#include <stdbool.h>
#include <stdint.h>

#define TILES_AUDIO_RATE_HZ 48000u
#define TILES_AUDIO_TRACKS 4u
#define TILES_AUDIO_TRACK_DRUMS TILES_AUDIO_TRACKS /* the kit: no voices here yet */
#define TILES_AUDIO_VOICES 20u      /* slots; the budget lets ~15 of the cheapest sound, the rest fade */
#define TILES_AUDIO_TRACK_VOICES 8u /* one track's most */
#define TILES_AUDIO_DRUM_VOICES 5u  /* drum samples ringing at once (the sampler) */

/* The guardrails (docs/architecture/audio-engine.md, "Guardrails"): every
 * voice together stays under TARGET % of core 1, after the reserve for the
 * effects and drums (measured on unit 2, cycles per sample: plate 473,
 * delay 118, ensemble 103, overdrive 122 a driven track, drum samples ~65
 * each); the live guard ends voices above CEILING %. */
#define TILES_AUDIO_TARGET_PCT 85u
#define TILES_AUDIO_CEILING_PCT 90u
#define TILES_AUDIO_RESERVE_CYCLES 1200u

/* Builds the wavetables (~0.15 s, the first time, in the effects' memory:
 * fx_memory.h) and clears the effects; call once before anything else. bend_range is the
 * semitones a full pitch bend means (what TILES announces to receivers).
 * clk_mhz sets the budget; now_us (NULL: none) times each track's voices
 * live, to learn their cost and run the load guard. */
void tiles_audio_engine_init(uint8_t bend_range_semitones, uint32_t clk_mhz, uint32_t (*now_us)(void));

/* One channel message (status 0x80-0xEF). Note on/off, poly and channel
 * pressure, pitch bend, CC 64 (sustain), CC 74 (slide), CC 120/123 (all
 * off). */
void tiles_audio_engine_midi(uint8_t status, uint8_t data1, uint8_t data2);

/* Renders frames (a multiple of 8) of interleaved stereo, signed 24-bit
 * audio in the top 24 bits of each int32 (USB audio's 4-byte subslot
 * layout). */
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

/* Each track's sound: one of the factory patches (synth_patches.c). A
 * change ends the track's notes. */
void tiles_audio_engine_set_patch(uint8_t track, uint8_t patch);
/* tiles_audio_engine_patch() for a track playing a bank voice. */
#define TILES_AUDIO_PATCH_BANK 0xFFu
uint8_t tiles_audio_engine_patch(uint8_t track);
/* The track's sound's name (a factory sound's, or the bank voice's). */
const char *tiles_audio_engine_track_name(uint8_t track);
/* A DX7 voice (unpacked, fm_voice.h) for a track, in two steps: core 0
 * fills the track's free slot (false while the last one is still to be
 * switched to), then core 1 switches the track to it, ending the old
 * sound's voices in 5 ms (tiles_audio_engine_fm_use(), via usb_audio). */
bool tiles_audio_engine_fm_load(uint8_t track, const uint8_t voice[155], const char *name);
void tiles_audio_engine_fm_use(uint8_t track);
const char *tiles_audio_engine_patch_name(uint8_t patch);
uint8_t tiles_audio_engine_patch_count(void);

/* The tempo synced LFOs, the delay and the pump follow (120 until set). */
void tiles_audio_engine_set_tempo(float beats_per_minute);

/* The effects (fx.h). A track's own (TILES_FX_TRACK_*: its sound's until
 * changed, and again when the sound changes) or the shared ones'
 * (TILES_FX_BUS_*), by number so the settings link and the menus share
 * one list. */
enum {
    TILES_FX_TRACK_REVERB = 0,
    TILES_FX_TRACK_DELAY,
    TILES_FX_TRACK_CHORUS,
    TILES_FX_TRACK_DRIVE,
    TILES_FX_TRACK_DARK,
    TILES_FX_TRACK_CRUSH,
    TILES_FX_TRACK_PUMP,
    TILES_FX_TRACK_PARAMS,
};
enum {
    TILES_FX_BUS_DECAY = 0,
    TILES_FX_BUS_DAMPING,
    TILES_FX_BUS_PREDELAY_MS,
    TILES_FX_BUS_MOD,
    TILES_FX_BUS_SIXTEENTHS,
    TILES_FX_BUS_FEEDBACK,
    TILES_FX_BUS_TONE,
    TILES_FX_BUS_TO_REVERB,
    TILES_FX_BUS_CHORUS_RATE,
    TILES_FX_BUS_CHORUS_DEPTH,
    TILES_FX_BUS_PUMP_RELEASE,
    TILES_FX_BUS_PARAMS,
};
#define TILES_FX_TARGET_BUS TILES_AUDIO_TRACKS /* the target for the shared ones */
void tiles_audio_engine_set_fx(uint8_t target, uint8_t param, float value);
float tiles_audio_engine_fx(uint8_t target, uint8_t param);

/* The effects' cycles per sample, followed live (0 before any). */
uint32_t tiles_audio_engine_fx_cost(void);

/* A test: cycles per sample of one voice of `patch` held alone for
 * `frames`, away from the playing voices; `cycles` reads a cycle counter. */
uint32_t tiles_audio_engine_bench(uint8_t patch, uint32_t frames, uint32_t (*cycles)(void));
