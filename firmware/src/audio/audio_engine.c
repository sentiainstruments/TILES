#include "audio_engine.h"

#include "midi_channels.h"
#include "sampler.h"

#include <math.h>
#include <string.h>

/* Sine table with one guard point for linear interpolation. */
#define TABLE_BITS 11u
#define TABLE_SIZE (1u << TABLE_BITS)
#define FRAC_BITS (32u - TABLE_BITS)
static float s_sine[TABLE_SIZE + 1u];

/* First guesses, to be tuned by ear. */
#define ATTACK_MS 4.0f
#define RELEASE_MS 300.0f
#define SHED_RELEASE_MS 10.0f /* a voice the load guard ends */
#define SILENT_ENV 0.0001f
/* Level and pitch move once per block, the level ramped across it. */
#define BLOCK_FRAMES 16u
/* Four loud voices reach the soft clip's knee. */
#define MIX_GAIN 0.22f
/* A full-scale drum sample alone stays below the knee (about -6 dBFS). */
#define DRUM_GAIN 0.5f
/* 2^32 / 48000: Hz to phase increment. */
#define HZ_TO_INC 89478.485f
/* A track's cost before it has been measured (cycles per voice-sample). */
#define COST_GUESS 150.0f
/* Measured costs follow the live timing over ~16 blocks. */
#define COST_SMOOTHING (1.0f / 16.0f)
/* Over the ceiling this many blocks running: end a voice. */
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
    float env;      /* attack/release envelope 0..1 */
    float level;    /* level reached at the end of the last block */
    uint32_t phase;
    uint32_t age;
} voice_t;

static voice_t s_voice[TILES_AUDIO_VOICES];
static float s_bend[16];     /* semitones */
static float s_pressure[16]; /* channel pressure 0..1 */
static bool s_sustain[16];
static float s_bend_range;
static float s_attack_step;
static float s_release_coef;
static float s_shed_coef;
static uint32_t s_age;

/* The budget, in cycles per output sample. */
static uint32_t (*s_now_us)(void);
static float s_clk_mhz;
static float s_core_cycles;  /* the whole of core 1 per sample */
static float s_budget;       /* what synth voices may take: target minus the reserve */
static float s_ceiling;      /* what the guard holds them to */
static float s_cost[TILES_AUDIO_TRACKS];
static uint32_t s_over_blocks;
static uint32_t s_shed;

void tiles_audio_engine_init(uint8_t bend_range_semitones, uint32_t clk_mhz, uint32_t (*now_us)(void)) {
    for (uint32_t i = 0; i <= TABLE_SIZE; i++) {
        s_sine[i] = sinf(6.2831853f * (float)i / (float)TABLE_SIZE);
    }
    memset(s_voice, 0, sizeof(s_voice));
    memset(s_bend, 0, sizeof(s_bend));
    memset(s_pressure, 0, sizeof(s_pressure));
    memset(s_sustain, 0, sizeof(s_sustain));
    s_bend_range = (float)bend_range_semitones;
    s_attack_step = 1000.0f / (ATTACK_MS * (float)TILES_AUDIO_RATE_HZ);
    /* Falls to SILENT_ENV over RELEASE_MS. */
    s_release_coef = expf(logf(SILENT_ENV) / (RELEASE_MS * 0.001f * (float)TILES_AUDIO_RATE_HZ));
    s_shed_coef = expf(logf(SILENT_ENV) / (SHED_RELEASE_MS * 0.001f * (float)TILES_AUDIO_RATE_HZ));
    s_age = 0;
    s_now_us = now_us;
    s_clk_mhz = (float)clk_mhz;
    s_core_cycles = (float)clk_mhz * 1000000.0f / (float)TILES_AUDIO_RATE_HZ;
    s_budget = s_core_cycles * (float)TILES_AUDIO_TARGET_PCT / 100.0f - (float)TILES_AUDIO_RESERVE_CYCLES;
    s_ceiling = s_core_cycles * (float)TILES_AUDIO_CEILING_PCT / 100.0f - (float)TILES_AUDIO_RESERVE_CYCLES;
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        s_cost[t] = COST_GUESS;
    }
    s_over_blocks = 0;
    s_shed = 0;
}

void tiles_audio_engine_set_track_cost(uint8_t track, uint32_t cycles) {
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

static float osc(uint32_t phase) {
    uint32_t i = phase >> FRAC_BITS;
    float frac = (float)(phase & ((1u << FRAC_BITS) - 1u)) * (1.0f / (float)(1u << FRAC_BITS));
    return s_sine[i] + frac * (s_sine[i + 1u] - s_sine[i]);
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
        if (v->state == VOICE_OFF || v->marked || (track >= 0 && v->track != (uint8_t)track)) {
            continue;
        }
        if (v->state == VOICE_RELEASE) {
            if (tail == NULL || v->env < tail->env) {
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
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->marked) {
            v->marked = false;
            if (fits && v != first) {
                v->state = VOICE_OFF; /* the real engines fade a taken voice over ~2 ms */
            }
        }
    }
    return fits ? first : NULL;
}

static void note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    bool lane;
    uint8_t track = tiles_audio_engine_track(channel, &lane);
    if (track == TILES_AUDIO_TRACK_DRUMS) {
        return; /* the drum kit comes with the drum engine */
    }
    voice_t *v = voice_for_note_on(channel, note, track, lane);
    if (v == NULL) {
        return;
    }
    if (v->state == VOICE_OFF) {
        v->phase = 0; /* a sine from zero starts without a click */
        v->env = 0.0f;
        v->level = 0.0f;
    }
    v->state = VOICE_ON;
    v->channel = channel;
    v->note = note;
    v->track = track;
    v->lane = lane;
    v->fast = false;
    v->velocity = (float)velocity / 127.0f;
    v->poly = 0.0f;
    v->age = ++s_age;
}

static void note_off(uint8_t channel, uint8_t note) {
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->state == VOICE_ON && v->channel == channel && v->note == note) {
            v->state = s_sustain[channel] ? VOICE_HELD : VOICE_RELEASE;
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
            } else if (data1 == 120u || data1 == 123u) {
                release_channel(channel, false);
            }
            break;
        default:
            break;
    }
}

/* Padé tanh: gentle above the knee, flat at +-1. */
static float soft_clip(float x) {
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static void render_voice(voice_t *v, float *mix, uint32_t n) {
    float pressure = s_pressure[v->channel] > v->poly ? s_pressure[v->channel] : v->poly;
    float target = (0.25f + 0.75f * v->velocity) * (0.55f + 0.45f * pressure);
    float bright = 0.1f + 0.5f * pressure;
    float norm = 1.0f / (1.0f + 0.5f * bright);
    float hz = 440.0f * exp2f(((float)v->note - 69.0f + s_bend[v->channel]) / 12.0f);
    uint32_t inc = (uint32_t)(hz * HZ_TO_INC);
    float level = v->level;
    float step = (target - level) / (float)n;
    float release = v->fast ? s_shed_coef : s_release_coef;
    for (uint32_t i = 0; i < n; i++) {
        if (v->state == VOICE_RELEASE) {
            v->env *= release;
        } else if (v->env < 1.0f) {
            v->env += s_attack_step;
            if (v->env > 1.0f) {
                v->env = 1.0f;
            }
        }
        float s = osc(v->phase) + bright * (0.35f * osc(v->phase * 2u) + 0.15f * osc(v->phase * 3u));
        mix[i] += s * norm * level * v->env;
        level += step;
        v->phase += inc;
    }
    v->level = target;
    if (v->state == VOICE_RELEASE && v->env < SILENT_ENV) {
        v->state = VOICE_OFF;
    }
}

/* Learns each track's cost per voice from the block's timing, and holds
 * the voices under the ceiling: several blocks over it in a row end the
 * quietest voice quickly (a tail, then a sequencer note, then the oldest
 * played note). */
static void guard(const float *track_cycles, const uint32_t *track_voices, uint32_t n) {
    float total = 0.0f;
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        total += track_cycles[t];
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
    if (total <= s_ceiling * (float)n) {
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
        s_shed++;
    }
}

static void render_block(int32_t *out_lr, uint32_t n) {
    float mix[BLOCK_FRAMES];
    memset(mix, 0, sizeof(mix));
    float track_cycles[TILES_AUDIO_TRACKS] = {0};
    uint32_t track_voices[TILES_AUDIO_TRACKS] = {0};
    for (uint32_t t = 0; t < TILES_AUDIO_TRACKS; t++) {
        uint32_t t0 = s_now_us != NULL ? s_now_us() : 0u;
        for (uint32_t vi = 0; vi < TILES_AUDIO_VOICES; vi++) {
            voice_t *v = &s_voice[vi];
            if (v->state != VOICE_OFF && v->track == t) {
                render_voice(v, mix, n);
                track_voices[t]++;
            }
        }
        if (s_now_us != NULL) {
            track_cycles[t] = (float)(s_now_us() - t0) * s_clk_mhz;
        }
    }
    if (s_now_us != NULL) {
        guard(track_cycles, track_voices, n);
    }
    float drums_l[BLOCK_FRAMES], drums_r[BLOCK_FRAMES];
    memset(drums_l, 0, sizeof(drums_l));
    memset(drums_r, 0, sizeof(drums_r));
    tiles_sampler_render(drums_l, drums_r, n);
    for (uint32_t i = 0; i < n; i++) {
        float yl = soft_clip(mix[i] * MIX_GAIN + drums_l[i] * DRUM_GAIN);
        float yr = soft_clip(mix[i] * MIX_GAIN + drums_r[i] * DRUM_GAIN);
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

uint8_t tiles_audio_engine_active_voices(void) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        if (s_voice[i].state != VOICE_OFF) {
            count++;
        }
    }
    return count;
}

uint8_t tiles_audio_engine_track_voices(uint8_t track) {
    uint8_t count = 0;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        if (s_voice[i].state != VOICE_OFF && s_voice[i].track == track) {
            count++;
        }
    }
    return count;
}
