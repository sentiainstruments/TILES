#pragma once

/* The sound TILES makes itself: the wavetable, FM and granular engines
 * (synth_voice.h, fm_voice.h, grain_voice.h) playing the MIDI TILES
 * sends, on four tracks plus drums, a sound (patch) per track
 * (docs/architecture/audio-engine.md, "Tracks"; synth-engines.md). Plain C with no Pico dependencies, so
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
 * effects and drums (measured on unit 2, cycles per sample: plate 242 and
 * delay 71 at 24 kHz, ensemble 102, overdrive 122 a driven track, drum
 * samples ~65 each; all the effects live at most 644 in a soak with four
 * driven tracks); the live guard ends voices above CEILING %. */
#define TILES_AUDIO_TARGET_PCT 85u
#define TILES_AUDIO_CEILING_PCT 90u
#define TILES_AUDIO_RESERVE_CYCLES 1050u

/* Wavetable banks in RAM at once (38 KB each): slot 1 always the classic
 * bank, the others any of the library's (wavetable.h), built on core 0 when
 * a sound needs one (tiles_audio_engine_service()). Every track's sound
 * fits while the four want at most this many - 1 banks besides classic. */
#define TILES_AUDIO_WT_SLOTS 4u

/* Builds the wavetables the tracks' sounds need and fills the other slots
 * (~50 ms a bank, the first time) and clears the effects; call once before anything else. bend_range is the
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

/* The granular tape's samples loaded (0: no tape sound, or its slot is
 * empty). */
uint32_t tiles_audio_engine_tape_frames(void);

/* Voices still sounding (releases included). */
uint8_t tiles_audio_engine_active_voices(void);

/* The track a channel plays on (0-3, or TILES_AUDIO_TRACK_DRUMS) and
 * whether it is a sequencer lane's: lane N is track N; melodic, Duet's
 * melody and bass guitar share lane 2's track, chords lane 3's, the
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

/* The master since boot, per channel: the mix's loudest sample before its
 * soft clip (1.0 = full scale), samples over 1.0 (squeezed by the clip,
 * ~1.6 dB down there already) and samples at its ceiling (flattened). */
typedef struct {
    float peak;
    uint32_t knee, clips;
} tiles_audio_master_t;
tiles_audio_master_t tiles_audio_engine_master(void);

/* Each track's sound, in two steps: core 0 copies preset `slot` (presets.h)
 * into the track's free sound, then core 1 switches the track to it
 * (tiles_audio_engine_sound_use(), via usb_audio), the old sound's voices
 * ending in 5 ms. A sound whose wavetable bank isn't in RAM waits for it
 * (BUILDING: the track plays its old sound meanwhile, and
 * tiles_audio_engine_service() says when to switch), or, when the only
 * room is the old sound's own bank, switches at once and stays silent
 * until its bank is built (~0.3 s). fm_load() fills it with a DX7 voice
 * (unpacked, fm_voice.h) instead. set_patch() does it all at once: on the
 * computer, and before core 1 runs. */
typedef enum {
    TILES_SOUND_REFUSED = 0, /* an empty slot, the last one still to be switched to, or no room for its bank */
    TILES_SOUND_READY,       /* switch to it now */
    TILES_SOUND_BUILDING,    /* its bank is being built: switch when service() says */
    TILES_SOUND_CURRENT,     /* the track plays it already (a waiting one is dropped) */
} tiles_sound_load_t;
tiles_sound_load_t tiles_audio_engine_sound_load(uint8_t track, uint8_t slot);
bool tiles_audio_engine_fm_load(uint8_t track, const uint8_t voice[155], const char *name);
/* Core 0, every pass: builds the banks sounds want, a step at a time for
 * about `budget_us` (at least one step); a bit set for each track whose
 * waiting sound is now ready to switch to. */
uint8_t tiles_audio_engine_service(uint32_t budget_us);
/* The slots' banks ("classic,glass,-,+talk": - empty, + being built); the
 * last bank built: its time, and its longest single step. */
void tiles_audio_engine_wt_slots(char *out, uint32_t size);
uint32_t tiles_audio_engine_wt_build_ms(void);
uint32_t tiles_audio_engine_wt_step_us(void);
void tiles_audio_engine_sound_use(uint8_t track);
void tiles_audio_engine_set_patch(uint8_t track, uint8_t slot);
/* tiles_audio_engine_patch() for a track playing a bank voice. */
#define TILES_AUDIO_PATCH_BANK 0xFFu
/* The track's preset slot. */
uint8_t tiles_audio_engine_patch(uint8_t track);
/* The track's sound's name (its preset's, or the bank voice's). */
const char *tiles_audio_engine_track_name(uint8_t track);

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

/* A test: cycles per sample of one voice of preset `slot` held alone for
 * `frames` (a granular one after 0.5 s untimed, its cloud full), away from
 * the playing voices; `cycles` reads a cycle counter. */
uint32_t tiles_audio_engine_bench(uint8_t slot, uint32_t frames, uint32_t (*cycles)(void));
