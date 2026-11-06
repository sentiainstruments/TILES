#include "audio_engine.h"

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
#define SILENT_ENV 0.0001f
/* Level and pitch move once per block, the level ramped across it. */
#define BLOCK_FRAMES 16u
/* Four loud voices reach the soft clip's knee. */
#define MIX_GAIN 0.22f
/* 2^32 / 48000: Hz to phase increment. */
#define HZ_TO_INC 89478.485f

enum { VOICE_OFF = 0, VOICE_ON, VOICE_HELD, VOICE_RELEASE };

typedef struct {
    uint8_t state;
    uint8_t channel;
    uint8_t note;
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
static uint32_t s_age;

void tiles_audio_engine_init(uint8_t bend_range_semitones) {
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
    s_age = 0;
}

static float osc(uint32_t phase) {
    uint32_t i = phase >> FRAC_BITS;
    float frac = (float)(phase & ((1u << FRAC_BITS) - 1u)) * (1.0f / (float)(1u << FRAC_BITS));
    return s_sine[i] + frac * (s_sine[i + 1u] - s_sine[i]);
}

static voice_t *voice_for_note_on(uint8_t channel, uint8_t note) {
    voice_t *free_voice = NULL, *quietest = NULL, *oldest = NULL;
    for (uint32_t i = 0; i < TILES_AUDIO_VOICES; i++) {
        voice_t *v = &s_voice[i];
        if (v->state != VOICE_OFF && v->channel == channel && v->note == note) {
            return v; /* the same note again: retrigger, never two */
        }
        if (v->state == VOICE_OFF) {
            if (free_voice == NULL) {
                free_voice = v;
            }
        } else if (v->state == VOICE_RELEASE) {
            if (quietest == NULL || v->env < quietest->env) {
                quietest = v;
            }
        } else if (oldest == NULL || v->age < oldest->age) {
            oldest = v;
        }
    }
    if (free_voice != NULL) {
        return free_voice;
    }
    return quietest != NULL ? quietest : oldest;
}

static void note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    voice_t *v = voice_for_note_on(channel, note);
    if (v->state == VOICE_OFF) {
        v->phase = 0; /* a sine from zero starts without a click */
        v->env = 0.0f;
        v->level = 0.0f;
    }
    v->state = VOICE_ON;
    v->channel = channel;
    v->note = note;
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

static void render_block(int32_t *out_lr, uint32_t n) {
    float mix[BLOCK_FRAMES];
    memset(mix, 0, sizeof(mix));
    for (uint32_t vi = 0; vi < TILES_AUDIO_VOICES; vi++) {
        voice_t *v = &s_voice[vi];
        if (v->state == VOICE_OFF) {
            continue;
        }
        float pressure = s_pressure[v->channel] > v->poly ? s_pressure[v->channel] : v->poly;
        float target = (0.25f + 0.75f * v->velocity) * (0.55f + 0.45f * pressure);
        float bright = 0.1f + 0.5f * pressure;
        float norm = 1.0f / (1.0f + 0.5f * bright);
        float hz = 440.0f * exp2f(((float)v->note - 69.0f + s_bend[v->channel]) / 12.0f);
        uint32_t inc = (uint32_t)(hz * HZ_TO_INC);
        float level = v->level;
        float step = (target - level) / (float)n;
        for (uint32_t i = 0; i < n; i++) {
            if (v->state == VOICE_RELEASE) {
                v->env *= s_release_coef;
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
    for (uint32_t i = 0; i < n; i++) {
        float y = soft_clip(mix[i] * MIX_GAIN);
        int32_t sample = (int32_t)(y * 8388607.0f) * 256; /* 24 bits in the top of the subslot */
        out_lr[2u * i] = sample;
        out_lr[2u * i + 1u] = sample;
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
