#include "audio_engine.h"

#include "audio_hot.h"
#include "fx.h"
#include "fx_memory.h"
#include "midi_channels.h"
#include "sampler.h"
#include "synth_voice.h"
#include "wavetable.h"

#include <math.h>
#include <string.h>

/* Frames per block: two control steps of the voices (SYNTH_CTRL). */
#define BLOCK_FRAMES 16u
/* Several loud voices reach the soft clip's knee. */
#define MIX_GAIN 0.3f
/* The note stack of a mono track (notes held, newest last). */
#define MONO_STACK 8u
/* A full-scale drum sample alone stays below the knee (about -6 dBFS). */
#define DRUM_GAIN 0.5f
/* Measured costs follow the live timing over ~16 blocks. */
#define COST_SMOOTHING (1.0f / 16.0f)
/* The load the guard compares, smoothed over ~8 blocks (2.7 ms): a USB
 * interrupt landing in one block isn't an overload. */
#define LOAD_SMOOTHING (1.0f / 8.0f)
/* The smoothed load over the ceiling this many blocks running: end a voice. */
#define GUARD_BLOCKS 3u
/* A block that took this many times its own length was paused (a flash
 * write), not loaded: not learned from. */
#define PAUSED_RATIO 1.5f

enum { VOICE_OFF = 0, VOICE_ON, VOICE_HELD, VOICE_RELEASE };

typedef struct {
    uint8_t state;
    uint8_t channel;
    uint8_t note;
    uint8_t track;
    bool lane;      /* a sequencer lane's note, not a played one */
    bool fast;      /* ended by the load guard: a short release */
    bool marked;    /* chosen to give way, while a note-on decides */
    float velocity; /* 0..1 */
    float poly;     /* poly pressure 0..1 */
    uint32_t age;
    const synth_patch_t *patch; /* the sound it started with (NULL: none yet) */
    union {
        synth_voice_t sv; /* SYNTH_ENGINE_WAVETABLE */
        tiles_fm_voice_t fm;
    } u;
} voice_t;

typedef struct {
    uint8_t channel, note, velocity;
} held_note_t;

static voice_t s_voice[TILES_AUDIO_VOICES];
static float s_bend[16];     /* semitones */
static float s_pressure[16]; /* channel pressure 0..1 */
static float s_slide[16];    /* CC74 0..1 */
static bool s_sustain[16];
static float s_bend_range;
static uint32_t s_age;
static uint8_t s_track_patch[TILES_AUDIO_TRACKS]; /* a factory sound, or TILES_AUDIO_PATCH_BANK */
static const synth_patch_t *s_track_sound[TILES_AUDIO_TRACKS];

/* Each track's voice from the DX7 bank: two slots, so voices still fading
 * from the last one keep theirs while core 0 fills the other
 * (tiles_audio_engine_fm_load()). */
typedef struct {
    tiles_fm_patch_t fm;
    synth_patch_t patch;
    char name[11];
} bank_sound_t;
static bank_sound_t s_bank_sound[TILES_AUDIO_TRACKS][2];
static volatile uint8_t s_bank_next[TILES_AUDIO_TRACKS];    /* the slot the next load fills */
static volatile bool s_bank_pending[TILES_AUDIO_TRACKS];    /* filled, not yet switched to */
static held_note_t s_mono_held[TILES_AUDIO_TRACKS][MONO_STACK];
static uint8_t s_mono_count[TILES_AUDIO_TRACKS];

/* The wavetables, built once at init (TILES_WT_BANK_*) with the effects'
 * memory as working space. */
static tiles_wt_bank_t s_banks[TILES_WT_BANK_COUNT];
static float *s_build_scratch;
static synth_context_t s_ctx;
_Static_assert(TILES_WT_SCRATCH_FLOATS * sizeof(float) + sizeof(tiles_wt_spectrum_t) <= TILES_FX_MEMORY_BYTES,
               "the wavetable build fits the effects' memory");

/* Each track's effects (its sound's, changed by a track setting), their
 * insert state, and its block before it is mixed. */
static tiles_fx_amounts_t s_track_fx[TILES_AUDIO_TRACKS];
static tiles_fx_insert_t s_insert[TILES_AUDIO_TRACKS];
static float s_track_l[TILES_AUDIO_TRACKS][BLOCK_FRAMES], s_track_r[TILES_AUDIO_TRACKS][BLOCK_FRAMES];
static float s_fx_cost;

/* The budget, in cycles per output sample. */
static uint32_t (*s_now_us)(void);
static float s_clk_mhz;
static float s_core_cycles;  /* the whole of core 1 per sample */
static float s_budget;       /* what synth voices may take: target minus the reserve */
static float s_ceiling;      /* what the guard holds them to */
static float s_cost[TILES_AUDIO_TRACKS];
static uint32_t s_over_blocks;
static float s_load; /* cycles a sample, fading voices left out, smoothed */
static uint32_t s_shed;

/* A voice of either engine. */
static inline bool is_fm(const voice_t *v) {
    return v->patch != NULL && v->patch->engine == SYNTH_ENGINE_FM;
}

static void voice_release(voice_t *v) {
    if (is_fm(v)) {
        tiles_fm_voice_release(&v->u.fm);
    } else {
        synth_voice_release(&v->u.sv);
    }
}

static void voice_kill(voice_t *v) {
    if (is_fm(v)) {
        tiles_fm_voice_kill(&v->u.fm);
    } else {
        synth_voice_kill(&v->u.sv);
    }
}

/* 0..~1 in both engines (an FM carrier's gain goes to 2). */
static float voice_level(const voice_t *v) {
    return is_fm(v) ? 0.5f * tiles_fm_voice_level(&v->u.fm) : synth_voice_level(&v->u.sv);
}

/* Silent now: its slot is taken. */
static void voice_cut(voice_t *v) {
    if (is_fm(v)) {
        v->u.fm.sounding = false;
    } else {
        v->u.sv.amp.stage = SYNTH_ENV_IDLE;
    }
}

/* An FM sound's level, velocity and pressure shares, as the wavetable
 * voice takes them. */
static float fm_gain(const voice_t *v, const synth_input_t *in) {
    const synth_patch_t *p = v->patch;
    float velocity = 1.0f - p->velocity_level + p->velocity_level * in->velocity * in->velocity;
    return p->level * velocity * (1.0f + p->pressure_level * v->u.fm.pressure_s);
}

static tiles_fm_input_t fm_input(const synth_input_t *in) {
    tiles_fm_input_t f = {in->note, in->velocity, in->pressure, in->slide};
    return f;
}

typedef struct {
    uint32_t bank;
} design_ctx_t;

TILES_AUDIO_COLD static void design(void *ctx, uint32_t frame, tiles_wt_spectrum_t *spectrum) {
    tiles_wt_design(((const design_ctx_t *)ctx)->bank, frame, spectrum, s_build_scratch);
}

/* A patch's cost before it has been measured, cycles per voice-sample,
 * from SYNTH BENCH on unit 2 (2026-11-14), then learned live. */
TILES_AUDIO_COLD static float patch_cost(const synth_patch_t *p) {
    if (p->engine == SYNTH_ENGINE_FM) {
        /* Operators at output level 0 are skipped. */
        uint32_t ops = 0;
        for (uint32_t k = 0; k < 6u; k++) {
            ops += p->fm->dx7[k * TILES_DX7_OP_BYTES + 16u] > 0u ? 1u : 0u;
        }
        return 30.0f + 22.0f * (float)ops;
    }
    float copies = (float)(p->unison < 1u ? 1u : p->unison);
    bool stereo = p->unison > 1u && p->width > 0.0f;
    float channels = stereo ? 2.0f : 1.0f;
    float filter = p->filter == SYNTH_FILTER_LADDER24 ? (stereo ? 100.0f : 80.0f) : (stereo ? 45.0f : 30.0f);
    float shaped = (p->drive > 0.0f || p->fold > 0.0f ? 15.0f : 0.0f) + (p->fold > 0.0f ? 40.0f : 0.0f);
    return 45.0f + 38.0f * copies + (stereo ? 10.0f : 0.0f) + filter + shaped * channels;
}

TILES_AUDIO_COLD void tiles_audio_engine_init(uint8_t bend_range_semitones, uint32_t clk_mhz, uint32_t (*now_us)(void)) {
    static bool built;
    if (!built) {
        s_build_scratch = (float *)tiles_fx_memory();
        tiles_wt_spectrum_t *spectrum = (tiles_wt_spectrum_t *)(s_build_scratch + TILES_WT_SCRATCH_FLOATS);
        for (uint32_t b = 0; b < TILES_WT_BANK_COUNT; b++) {
            design_ctx_t ctx = {b};
            tiles_wt_build(&s_banks[b], design, &ctx, spectrum, s_build_scratch);
            s_ctx.banks[b] = &s_banks[b];
        }
        tiles_fm_init();
        built = true;
    }
    tiles_fx_init(tiles_fx_memory());
    s_fx_cost = 0.0f;
    s_ctx.beats_per_second = 2.0f; /* 120 BPM until told */
    memset(s_voice, 0, sizeof(s_voice));
    memset(s_bend, 0, sizeof(s_bend));
    memset(s_pressure, 0, sizeof(s_pressure));
    memset(s_slide, 0, sizeof(s_slide));
    memset(s_sustain, 0, sizeof(s_sustain));
    memset(s_mono_count, 0, sizeof(s_mono_count));
    s_bend_range = (float)bend_range_semitones;
    s_age = 0;
    s_now_us = now_us;
    s_clk_mhz = (float)clk_mhz;
    s_core_cycles = (float)clk_mhz * 1000000.0f / (float)TILES_AUDIO_RATE_HZ;
    s_budget = s_core_cycles * (float)TILES_AUDIO_TARGET_PCT / 100.0f - (float)TILES_AUDIO_RESERVE_CYCLES;
    s_ceiling = s_core_cycles * (float)TILES_AUDIO_CEILING_PCT / 100.0f - (float)TILES_AUDIO_RESERVE_CYCLES;
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        s_track_patch[t] = g_synth_track_default[t];
        s_track_sound[t] = &g_synth_patches[s_track_patch[t]];
        s_cost[t] = patch_cost(s_track_sound[t]);
        s_track_fx[t] = s_track_sound[t]->fx;
        tiles_fx_insert_reset(&s_insert[t]);
        s_bank_pending[t] = false;
    }
    s_over_blocks = 0;
    s_load = 0.0f;
    s_shed = 0;
}

TILES_AUDIO_COLD void tiles_audio_engine_set_track_cost(uint8_t track, uint32_t cycles) {
    if (track < TILES_AUDIO_TRACKS) {
        s_cost[track] = (float)cycles;
    }
}

uint32_t tiles_audio_engine_track_cost(uint8_t track) {
    return track < TILES_AUDIO_TRACKS ? (uint32_t)(s_cost[track] + 0.5f) : 0u;
}

uint32_t tiles_audio_engine_shed_count(void) {
    return s_shed;
}

TILES_AUDIO_COLD static void assign_sound(uint8_t track, const synth_patch_t *p, uint8_t patch) {
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        if (s_voice[i].state != VOICE_OFF && s_voice[i].track == track) {
            s_voice[i].state = VOICE_RELEASE;
            voice_kill(&s_voice[i]); /* the old sound ends in 5 ms */
        }
    }
    s_track_patch[track] = patch;
    s_track_sound[track] = p;
    s_mono_count[track] = 0u;
    s_cost[track] = patch_cost(p);
    s_track_fx[track] = p->fx;
}

TILES_AUDIO_COLD void tiles_audio_engine_set_patch(uint8_t track, uint8_t patch) {
    if (track >= TILES_AUDIO_TRACKS || patch >= g_synth_patch_count || s_track_patch[track] == patch) {
        return;
    }
    assign_sound(track, &g_synth_patches[patch], patch);
}

TILES_AUDIO_COLD bool tiles_audio_engine_fm_load(uint8_t track, const uint8_t voice[TILES_DX7_VOICE_BYTES],
                                                 const char *name) {
    if (track >= TILES_AUDIO_TRACKS || __atomic_load_n(&s_bank_pending[track], __ATOMIC_ACQUIRE)) {
        return false;
    }
    bank_sound_t *b = &s_bank_sound[track][s_bank_next[track]];
    memcpy(b->fm.dx7, voice, TILES_DX7_VOICE_BYTES);
    b->fm.pressure_db = 9.0f; /* as the factory FM sounds: brighter with pressure and slide */
    b->fm.slide_db = 6.0f;
    strncpy(b->name, name, sizeof(b->name) - 1u);
    b->name[sizeof(b->name) - 1u] = '\0';
    memset(&b->patch, 0, sizeof(b->patch));
    b->patch.name = b->name;
    b->patch.engine = SYNTH_ENGINE_FM;
    b->patch.fm = &b->fm;
    b->patch.voice_mode = SYNTH_POLY;
    /* Carriers add up: level by their count, about the factory sounds'
     * loudness (first guess). */
    uint8_t carriers = tiles_fm_carriers(voice);
    b->patch.level = 0.6f / sqrtf((float)(carriers < 1u ? 1u : carriers));
    b->patch.velocity_level = 0.2f;
    b->patch.pressure_level = 0.15f;
    b->patch.fx.reverb = 0.2f;
    b->patch.fx.delay = 0.1f;
    __atomic_store_n(&s_bank_pending[track], true, __ATOMIC_RELEASE);
    return true;
}

TILES_AUDIO_COLD void tiles_audio_engine_fm_use(uint8_t track) {
    if (track >= TILES_AUDIO_TRACKS || !__atomic_load_n(&s_bank_pending[track], __ATOMIC_ACQUIRE)) {
        return;
    }
    uint8_t slot = s_bank_next[track];
    assign_sound(track, &s_bank_sound[track][slot].patch, TILES_AUDIO_PATCH_BANK);
    s_bank_next[track] = (uint8_t)(1u - slot);
    __atomic_store_n(&s_bank_pending[track], false, __ATOMIC_RELEASE);
}

const char *tiles_audio_engine_track_name(uint8_t track) {
    return track < TILES_AUDIO_TRACKS && s_track_sound[track] != NULL ? s_track_sound[track]->name : "";
}

TILES_AUDIO_COLD static float *track_fx_field(tiles_fx_amounts_t *fx, uint8_t param) {
    switch (param) {
    case TILES_FX_TRACK_REVERB:
        return &fx->reverb;
    case TILES_FX_TRACK_DELAY:
        return &fx->delay;
    case TILES_FX_TRACK_CHORUS:
        return &fx->chorus;
    case TILES_FX_TRACK_DRIVE:
        return &fx->drive;
    case TILES_FX_TRACK_DARK:
        return &fx->dark;
    case TILES_FX_TRACK_CRUSH:
        return &fx->crush;
    case TILES_FX_TRACK_PUMP:
        return &fx->pump;
    default:
        return NULL;
    }
}

TILES_AUDIO_COLD static float *bus_field(tiles_fx_bus_t *b, uint8_t param) {
    switch (param) {
    case TILES_FX_BUS_DECAY:
        return &b->reverb_decay;
    case TILES_FX_BUS_DAMPING:
        return &b->reverb_damping;
    case TILES_FX_BUS_PREDELAY_MS:
        return &b->reverb_predelay_ms;
    case TILES_FX_BUS_MOD:
        return &b->reverb_mod;
    case TILES_FX_BUS_SIXTEENTHS:
        return &b->delay_sixteenths;
    case TILES_FX_BUS_FEEDBACK:
        return &b->delay_feedback;
    case TILES_FX_BUS_TONE:
        return &b->delay_tone;
    case TILES_FX_BUS_TO_REVERB:
        return &b->delay_to_reverb;
    case TILES_FX_BUS_CHORUS_RATE:
        return &b->chorus_rate;
    case TILES_FX_BUS_CHORUS_DEPTH:
        return &b->chorus_depth;
    case TILES_FX_BUS_PUMP_RELEASE:
        return &b->pump_release;
    default:
        return NULL;
    }
}

TILES_AUDIO_COLD void tiles_audio_engine_set_fx(uint8_t target, uint8_t param, float value) {
    if (target < TILES_AUDIO_TRACKS) {
        float *f = track_fx_field(&s_track_fx[target], param);
        if (f != NULL) {
            *f = synth_clamp(value, 0.0f, 1.0f);
        }
    } else if (target == TILES_FX_TARGET_BUS) {
        tiles_fx_bus_t bus = *tiles_fx_bus();
        float *f = bus_field(&bus, param);
        if (f != NULL) {
            *f = value;
            tiles_fx_set_bus(&bus);
        }
    }
}

TILES_AUDIO_COLD float tiles_audio_engine_fx(uint8_t target, uint8_t param) {
    if (target < TILES_AUDIO_TRACKS) {
        float *f = track_fx_field(&s_track_fx[target], param);
        return f != NULL ? *f : 0.0f;
    }
    if (target == TILES_FX_TARGET_BUS) {
        tiles_fx_bus_t bus = *tiles_fx_bus();
        float *f = bus_field(&bus, param);
        return f != NULL ? *f : 0.0f;
    }
    return 0.0f;
}

uint32_t tiles_audio_engine_fx_cost(void) {
    return (uint32_t)(s_fx_cost + 0.5f);
}

uint8_t tiles_audio_engine_patch(uint8_t track) {
    return track < TILES_AUDIO_TRACKS ? s_track_patch[track] : 0u;
}

TILES_AUDIO_COLD const char *tiles_audio_engine_patch_name(uint8_t patch) {
    return patch < g_synth_patch_count ? g_synth_patches[patch].name : "";
}

uint8_t tiles_audio_engine_patch_count(void) {
    return (uint8_t)g_synth_patch_count;
}

void tiles_audio_engine_set_tempo(float beats_per_minute) {
    if (beats_per_minute >= 20.0f && beats_per_minute <= 400.0f) {
        s_ctx.beats_per_second = beats_per_minute / 60.0f;
    }
}

TILES_AUDIO_COLD uint32_t tiles_audio_engine_bench(uint8_t patch, uint32_t frames, uint32_t (*cycles)(void)) {
    if (patch >= g_synth_patch_count || frames == 0u) {
        return 0u;
    }
    const synth_patch_t *p = &g_synth_patches[patch];
    voice_t v;
    memset(&v, 0, sizeof(v));
    v.patch = p;
    synth_input_t in = {48.0f, 0.8f, 0.5f, 0.5f};
    tiles_fm_input_t fin = fm_input(&in);
    if (is_fm(&v)) {
        tiles_fm_voice_start(&v.u.fm, p->fm, &fin, 0.0f, false);
    } else {
        synth_voice_start(&v.u.sv, p, &in, 12345u, false);
    }
    float l[BLOCK_FRAMES], r[BLOCK_FRAMES];
    uint64_t total = 0;
    uint32_t done = 0;
    while (done < frames) {
        memset(l, 0, sizeof(l));
        memset(r, 0, sizeof(r));
        uint32_t t0 = cycles();
        if (is_fm(&v)) {
            tiles_fm_voice_render(&v.u.fm, &fin, fm_gain(&v, &in), l, r, BLOCK_FRAMES);
        } else {
            synth_voice_render(&v.u.sv, &s_ctx, &in, l, r, BLOCK_FRAMES);
        }
        total += cycles() - t0;
        done += BLOCK_FRAMES;
    }
    return (uint32_t)(total / done);
}

uint8_t tiles_audio_engine_track(uint8_t channel, bool *from_lane) {
    *from_lane = true;
    switch (channel) {
    case TILES_MIDI_CH_SEQ_LANE_0:
        return 0u;
    case TILES_MIDI_CH_SEQ_LANE_1:
        return 1u;
    case TILES_MIDI_CH_SEQ_LANE_2:
        return 2u;
    case TILES_MIDI_CH_SEQ_LANE_3:
        return 3u;
    default:
        break;
    }
    *from_lane = false;
    switch (channel) {
    case TILES_MIDI_CH_CHORD: /* Prism, Duet's chords, the chord strip */
        return 2u;
    case TILES_MIDI_CH_GAME: /* the physics modes, bouncing notes, games */
        return 3u;
    case TILES_MIDI_CH_DRUMS:
        return TILES_AUDIO_TRACK_DRUMS;
    default: /* the MPE master and pool: melodic, Duet's melody, bass, Song */
        return 1u;
    }
}

/* The voice to give up on `track` (-1: any): the quietest releasing tail,
 * else (unless tails_only) the oldest sequencer note, else, for a played
 * note only, the oldest played note. */
static voice_t *victim(int32_t track, bool tails_only, bool take_played) {
    voice_t *tail = NULL, *lane = NULL, *played = NULL;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->state == VOICE_OFF || v->marked || v->fast || (track >= 0 && v->track != (uint8_t)track)) {
            continue; /* a voice being cut has given way already */
        }
        if (v->state == VOICE_RELEASE) {
            if (tail == NULL || voice_level(v) < voice_level(tail)) {
                tail = v;
            }
        } else if (v->lane) {
            if (lane == NULL || v->age < lane->age) {
                lane = v;
            }
        } else if (played == NULL || v->age < played->age) {
            played = v;
        }
    }
    if (tail != NULL || tails_only) {
        return tail;
    }
    if (lane != NULL) {
        return lane;
    }
    return take_played ? played : NULL;
}

/* The next voice to give way for a note on `track`. A track at its 8 gives
 * from itself first (a played note takes its lane's voice). Otherwise: a
 * releasing tail anywhere, this track's oldest sequencer note, the oldest
 * sequencer note anywhere, then (for a played note) the oldest played note
 * on the busiest track. A sequencer note never takes a played one. */
static voice_t *next_victim(uint8_t track, bool played, bool from_track) {
    if (from_track) {
        return victim((int32_t)track, false, played);
    }
    voice_t *v = victim(-1, true, false);
    if (v == NULL) {
        v = victim((int32_t)track, false, false);
    }
    if (v == NULL) {
        v = victim(-1, false, false);
    }
    if (v == NULL && played) {
        uint32_t count[TILES_AUDIO_TRACKS] = {0};
        for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
            if (s_voice[i].state != VOICE_OFF && !s_voice[i].marked) {
                count[s_voice[i].track]++;
            }
        }
        uint8_t busiest = 0u;
        for (uint8_t t = 1u; t < TILES_AUDIO_TRACKS; t++) {
            if (count[t] > count[busiest]) {
                busiest = t;
            }
        }
        v = victim((int32_t)busiest, false, true);
    }
    return v;
}

/* Every voice has a cost (its track's, measured live); the voices together
 * stay inside the budget. A note that doesn't fit takes room in the order
 * above, as many voices as it needs; if they aren't enough it is skipped
 * and nothing is taken (NULL). */
static voice_t *voice_for_note_on(uint8_t channel, uint8_t note, uint8_t track, bool lane) {
    voice_t *free_voice = NULL;
    uint32_t on_track = 0;
    float load = 0.0f;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->state == VOICE_OFF) {
            if (free_voice == NULL) {
                free_voice = v;
            }
            continue;
        }
        if (v->fast) {
            continue; /* fading out: neither the track's nor the budget's */
        }
        if (v->channel == channel && v->note == note) {
            return v; /* the same note again: retrigger, never two */
        }
        load += s_cost[v->track];
        on_track += v->track == track ? 1u : 0u;
    }
    bool played = !lane;
    bool track_full = on_track >= TILES_AUDIO_TRACK_VOICES;
    float need = s_cost[track];
    if (!track_full && free_voice != NULL && load + need <= s_budget) {
        return free_voice;
    }
    voice_t *first = NULL;
    bool fits = false;
    for (;;) {
        voice_t *v = next_victim(track, played, track_full && first == NULL);
        if (v == NULL) {
            break;
        }
        v->marked = true;
        load -= s_cost[v->track];
        if (first == NULL) {
            first = v;
        }
        if (load + need <= s_budget) {
            fits = true;
            break;
        }
    }
    /* The voices given up fade out over 5 ms; the note takes a free slot
     * (there are more slots than the budget lets sound), else the first. */
    voice_t *slot = NULL;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->marked) {
            v->marked = false;
            if (fits) {
                v->state = VOICE_RELEASE;
                v->fast = true;
                voice_kill(v);
            }
        } else if (v->state == VOICE_OFF && slot == NULL) {
            slot = v;
        }
    }
    if (!fits) {
        return NULL;
    }
    if (slot == NULL) {
        slot = first;
        voice_cut(slot); /* no slot left: cut */
    }
    return slot;
}

static synth_input_t voice_input(const voice_t *v) {
    float pressure = s_pressure[v->channel] > v->poly ? s_pressure[v->channel] : v->poly;
    synth_input_t in = {(float)v->note + s_bend[v->channel], v->velocity, pressure, s_slide[v->channel]};
    return in;
}

static void start_voice(voice_t *v, uint8_t channel, uint8_t note, uint8_t velocity, uint8_t track, bool lane,
                        bool legato) {
    v->state = VOICE_ON;
    v->channel = channel;
    v->note = note;
    v->track = track;
    v->lane = lane;
    v->fast = false;
    v->velocity = (float)velocity / 127.0f;
    v->poly = 0.0f;
    v->age = ++s_age;
    synth_input_t in = voice_input(v);
    const synth_patch_t *patch = s_track_sound[track];
    if (v->patch == NULL || v->patch->engine != patch->engine) {
        memset(&v->u, 0, sizeof(v->u)); /* the slot last played the other engine */
    }
    v->patch = patch;
    if (patch->engine == SYNTH_ENGINE_FM) {
        tiles_fm_input_t fin = fm_input(&in);
        bool glide = legato || patch->voice_mode == SYNTH_MONO; /* as the wavetable voice */
        tiles_fm_voice_start(&v->u.fm, patch->fm, &fin, glide ? patch->glide_ms : 0.0f, legato);
    } else {
        synth_voice_start(&v->u.sv, patch, &in, s_age * 2654435761u, legato);
    }
}

/* A mono track's one voice (sounding, or its tail). */
static voice_t *mono_voice(uint8_t track) {
    voice_t *found = NULL;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->state != VOICE_OFF && v->track == track && !v->fast && (found == NULL || v->age > found->age)) {
            found = v;
        }
    }
    return found;
}

static void mono_forget(uint8_t track, uint8_t channel, uint8_t note) {
    uint8_t n = s_mono_count[track], k = 0;
    for (uint8_t i = 0; i < n; i++) {
        held_note_t h = s_mono_held[track][i];
        if (!(h.channel == channel && h.note == note)) {
            s_mono_held[track][k++] = h;
        }
    }
    s_mono_count[track] = k;
}

static void note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    bool lane;
    uint8_t track = tiles_audio_engine_track(channel, &lane);
    if (track == TILES_AUDIO_TRACK_DRUMS) {
        return; /* drums are the sampler's (usb_audio.c) */
    }
    const synth_patch_t *patch = s_track_sound[track];
    if (patch->voice_mode != SYNTH_POLY) {
        /* Mono: the newest held note sounds; legato glides into it while
         * another is still held. */
        mono_forget(track, channel, note);
        if (s_mono_count[track] == MONO_STACK) {
            mono_forget(track, s_mono_held[track][0].channel, s_mono_held[track][0].note);
        }
        bool held_before = s_mono_count[track] > 0u;
        s_mono_held[track][s_mono_count[track]++] = (held_note_t){channel, note, velocity};
        voice_t *v = mono_voice(track);
        if (v != NULL) {
            bool legato = patch->voice_mode == SYNTH_LEGATO && held_before && v->state != VOICE_RELEASE;
            start_voice(v, channel, note, velocity, track, lane, legato);
            return;
        }
    }
    voice_t *v = voice_for_note_on(channel, note, track, lane);
    if (v != NULL) {
        start_voice(v, channel, note, velocity, track, lane, false);
    }
}

static void note_off(uint8_t channel, uint8_t note) {
    bool lane;
    uint8_t track = tiles_audio_engine_track(channel, &lane);
    if (track < TILES_AUDIO_TRACKS && s_track_sound[track]->voice_mode != SYNTH_POLY) {
        mono_forget(track, channel, note);
        voice_t *v = mono_voice(track);
        if (v != NULL && v->channel == channel && v->note == note && v->state == VOICE_ON && s_mono_count[track] > 0u) {
            /* Back to the newest note still held, gliding. */
            held_note_t h = s_mono_held[track][s_mono_count[track] - 1u];
            start_voice(v, h.channel, h.note, h.velocity, track, lane, true);
            return;
        }
    }
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->state == VOICE_ON && v->channel == channel && v->note == note) {
            v->state = s_sustain[channel] ? VOICE_HELD : VOICE_RELEASE;
            if (v->state == VOICE_RELEASE) {
                voice_release(v);
            }
        }
    }
}

static void release_channel(uint8_t channel, bool held_only) {
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->channel != channel) {
            continue;
        }
        if (v->state == VOICE_HELD || (!held_only && v->state == VOICE_ON)) {
            v->state = VOICE_RELEASE;
            voice_release(v);
        }
    }
}

void tiles_audio_engine_midi(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t channel = status & 0x0Fu;
    switch (status & 0xF0u) {
        case 0x90u:
            if (data2 > 0u) {
                note_on(channel, data1 & 0x7Fu, data2);
            } else {
                note_off(channel, data1 & 0x7Fu);
            }
            break;
        case 0x80u:
            note_off(channel, data1 & 0x7Fu);
            break;
        case 0xA0u:
            for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
                voice_t *v = &s_voice[i];
                if (v->state != VOICE_OFF && v->channel == channel && v->note == data1) {
                    v->poly = (float)data2 / 127.0f;
                }
            }
            break;
        case 0xD0u:
            s_pressure[channel] = (float)data1 / 127.0f;
            break;
        case 0xE0u: {
            int32_t bend = (int32_t)(((uint32_t)data2 << 7) | data1) - 8192;
            s_bend[channel] = (float)bend / 8192.0f * s_bend_range;
            break;
        }
        case 0xB0u:
            if (data1 == 64u) {
                s_sustain[channel] = data2 >= 64u;
                if (!s_sustain[channel]) {
                    release_channel(channel, true);
                }
            } else if (data1 == 74u) {
                s_slide[channel] = (float)data2 / 127.0f;
            } else if (data1 == 120u || data1 == 123u) {
                release_channel(channel, false);
                bool lane;
                uint8_t track = tiles_audio_engine_track(channel, &lane);
                if (track < TILES_AUDIO_TRACKS) {
                    s_mono_count[track] = 0u;
                }
            }
            break;
        default:
            break;
    }
}

static void TILES_AUDIO_HOT(render_voice)(voice_t *v, float *l, float *r, uint32_t n) {
    synth_input_t in = voice_input(v);
    if (is_fm(v)) {
        tiles_fm_input_t fin = fm_input(&in);
        tiles_fm_voice_render(&v->u.fm, &fin, fm_gain(v, &in), l, r, n);
        if (!tiles_fm_voice_active(&v->u.fm)) {
            v->state = VOICE_OFF;
        }
        return;
    }
    synth_voice_render(&v->u.sv, &s_ctx, &in, l, r, n);
    if (!synth_voice_active(&v->u.sv)) {
        v->state = VOICE_OFF;
    }
}

/* Learns each track's cost per voice from the block's timing, and holds
 * the voices under the ceiling: the smoothed load over it several blocks
 * running ends the quietest voice quickly (a tail, then a sequencer note,
 * then the oldest played note). A voice fading out (5 ms) is left out of
 * the load: counted, the guard would end another while it fades, then
 * another (a 7-voice chord once lost 6). */
static void TILES_AUDIO_HOT(guard)(const float *track_cycles, const uint32_t *track_voices,
                                   const uint32_t *track_fading, uint32_t n) {
    float total = 0.0f, fading = 0.0f;
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        total += track_cycles[t];
        fading += (float)track_fading[t] * s_cost[t];
    }
    if (total > s_core_cycles * (float)n * PAUSED_RATIO) {
        return; /* paused (a flash write): neither learned from nor counted */
    }
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        if (track_voices[t] > 0u) {
            float per_voice = track_cycles[t] / ((float)track_voices[t] * (float)n);
            s_cost[t] += (per_voice - s_cost[t]) * COST_SMOOTHING;
        }
    }
    s_load += (total / (float)n - fading - s_load) * LOAD_SMOOTHING;
    if (s_load <= s_ceiling) {
        s_over_blocks = 0;
        return;
    }
    if (++s_over_blocks < GUARD_BLOCKS) {
        return;
    }
    s_over_blocks = 0;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        s_voice[i].marked = s_voice[i].fast; /* already ending */
    }
    voice_t *v = victim(-1, false, true);
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        s_voice[i].marked = false;
    }
    if (v != NULL) {
        v->state = VOICE_RELEASE;
        v->fast = true;
        voice_kill(v);
        s_load -= s_cost[v->track]; /* its fade isn't load from here */
        s_shed++;
    }
}

/* Voices into each track's block; then per track (at the mix level) its
 * inserts and pump, into the mix and the sends; the shared effects; the
 * drums; the master's soft clip. */
static void TILES_AUDIO_HOT(render_block)(int32_t *out_lr, uint32_t n) {
    float track_cycles[TILES_AUDIO_TRACKS] = {0};
    uint32_t track_voices[TILES_AUDIO_TRACKS] = {0};
    uint32_t track_fading[TILES_AUDIO_TRACKS] = {0};
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        float *tl = s_track_l[t], *tr = s_track_r[t];
        memset(tl, 0, n * sizeof(float));
        memset(tr, 0, n * sizeof(float));
        uint32_t t0 = s_now_us != NULL ? s_now_us() : 0u;
        for (uint32_t vi = 0; vi < TILES_AUDIO_VOICES; vi++) {
            voice_t *v = &s_voice[vi];
            if (v->state != VOICE_OFF && v->track == t) {
                track_fading[t] += v->fast ? 1u : 0u;
                render_voice(v, tl, tr, n);
                track_voices[t]++;
            }
        }
        if (s_now_us != NULL) {
            track_cycles[t] = (float)(s_now_us() - t0) * s_clk_mhz;
        }
    }
    if (s_now_us != NULL) {
        guard(track_cycles, track_voices, track_fading, n);
    }

    uint32_t fx_t0 = s_now_us != NULL ? s_now_us() : 0u;
    float mix_l[BLOCK_FRAMES], mix_r[BLOCK_FRAMES];
    float chorus_in[BLOCK_FRAMES], delay_in[BLOCK_FRAMES], reverb_in[BLOCK_FRAMES];
    memset(mix_l, 0, sizeof(mix_l));
    memset(mix_r, 0, sizeof(mix_r));
    memset(chorus_in, 0, sizeof(chorus_in));
    memset(delay_in, 0, sizeof(delay_in));
    memset(reverb_in, 0, sizeof(reverb_in));
    tiles_fx_block_begin(n, s_ctx.beats_per_second);
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        if (track_voices[t] == 0u) {
            continue;
        }
        float *tl = s_track_l[t], *tr = s_track_r[t];
        for (uint32_t i = 0; i < n; i++) {
            tl[i] *= MIX_GAIN;
            tr[i] *= MIX_GAIN;
        }
        const tiles_fx_amounts_t *fx = &s_track_fx[t];
        tiles_fx_insert(&s_insert[t], fx, tl, tr, n);
        for (uint32_t i = 0; i < n; i++) {
            mix_l[i] += tl[i];
            mix_r[i] += tr[i];
            float m = 0.5f * (tl[i] + tr[i]);
            chorus_in[i] += m * fx->chorus;
            delay_in[i] += m * fx->delay;
            reverb_in[i] += m * fx->reverb;
        }
    }
    tiles_fx_busses(chorus_in, delay_in, reverb_in, mix_l, mix_r, n, s_ctx.beats_per_second);
    if (s_now_us != NULL) {
        float per_sample = (float)(s_now_us() - fx_t0) * s_clk_mhz / (float)n;
        s_fx_cost += (per_sample - s_fx_cost) * COST_SMOOTHING;
    }

    float drums_l[BLOCK_FRAMES], drums_r[BLOCK_FRAMES];
    memset(drums_l, 0, sizeof(drums_l));
    memset(drums_r, 0, sizeof(drums_r));
    tiles_sampler_render(drums_l, drums_r, n);
    for (uint32_t i = 0; i < n; i++) {
        float yl = synth_sat(mix_l[i] + drums_l[i] * DRUM_GAIN); /* flat at +-1 */
        float yr = synth_sat(mix_r[i] + drums_r[i] * DRUM_GAIN);
        out_lr[2u * i] = (int32_t)(yl * 8388607.0f) * 256; /* 24 bits in the top of the subslot */
        out_lr[2u * i + 1u] = (int32_t)(yr * 8388607.0f) * 256;
    }
}

void tiles_audio_engine_render(int32_t *out_lr, uint32_t frames) {
    while (frames > 0u) {
        uint32_t n = frames < BLOCK_FRAMES ? frames : BLOCK_FRAMES;
        render_block(out_lr, n);
        out_lr += 2u * n;
        frames -= n;
    }
}

/* Counts leave out voices being cut (a 5 ms fade): they gave way already. */
uint8_t tiles_audio_engine_active_voices(void) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        if (s_voice[i].state != VOICE_OFF && !s_voice[i].fast) {
            count++;
        }
    }
    return count;
}

uint8_t tiles_audio_engine_track_voices(uint8_t track) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        if (s_voice[i].state != VOICE_OFF && !s_voice[i].fast && s_voice[i].track == track) {
            count++;
        }
    }
    return count;
}
