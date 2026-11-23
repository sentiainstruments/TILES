#include "presets.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* ---- the record (README.md, "Preset record") ---- */

#define MARK0 'T'
#define MARK1 'P'
#define VERSION 1u
#define OFF_NAME 4u
#define OFF_BYTES 14u   /* bank, unison, filter, voice mode */
#define OFF_FLAGS 18u   /* phase reset, LFO synced, LFO retrigger */
#define OFF_SHAPE 19u
#define OFF_COMMON 20u  /* 42 floats */
#define OFF_GRAIN 188u  /* 4 bytes, 17 floats */
#define OFF_FM 260u     /* the DX7 voice, a pad byte, 2 floats */
#define RECORD_END 424u /* zeros after, for later versions */

/* A float field: where it sits, and the range a record may give it (out
 * of range: clamped, so a record can't make a sound that harms ears or
 * the engine). */
typedef struct {
    uint16_t at;
    float lo, hi;
} field_t;

#define P(f, lo, hi) {(uint16_t)offsetof(synth_patch_t, f), lo, hi}
static const field_t k_common[] = {
    P(wave, -1.0f, 2.0f),           P(wave_pressure, -2.0f, 2.0f), P(wave_slide, -2.0f, 2.0f),
    P(wave_env, -2.0f, 2.0f),       P(wave_lfo, -2.0f, 2.0f),      P(detune_cents, 0.0f, 100.0f),
    P(width, 0.0f, 1.0f),           P(drive, 0.0f, 8.0f),          P(fold, 0.0f, 1.0f),
    P(cutoff_hz, 0.0f, 20000.0f),  P(resonance, 0.0f, 1.0f),      P(key_track, -1.0f, 2.0f),
    P(env_oct, -8.0f, 8.0f),        P(velocity_oct, -8.0f, 8.0f),  P(pressure_oct, -8.0f, 8.0f),
    P(slide_oct, -8.0f, 8.0f),      P(lfo_oct, -8.0f, 8.0f),       P(amp.attack_ms, 0.0f, 60000.0f),
    P(amp.decay_ms, 0.0f, 60000.0f), P(amp.sustain, 0.0f, 1.0f),   P(amp.release_ms, 0.0f, 60000.0f),
    P(mod.attack_ms, 0.0f, 60000.0f), P(mod.decay_ms, 0.0f, 60000.0f), P(mod.sustain, 0.0f, 1.0f),
    P(mod.release_ms, 0.0f, 60000.0f), P(lfo.rate, 0.0f, 64.0f),   P(lfo.delay_ms, 0.0f, 10000.0f),
    P(vibrato_cents, 0.0f, 1200.0f), P(pitch_env_semis, -48.0f, 48.0f), P(pitch_env_ms, 0.0f, 10000.0f),
    P(glide_ms, 0.0f, 10000.0f),    P(level, 0.0f, 4.0f),          P(velocity_level, 0.0f, 1.0f),
    P(pressure_level, 0.0f, 2.0f),  P(drift_cents, 0.0f, 100.0f),  P(fx.reverb, 0.0f, 1.0f),
    P(fx.delay, 0.0f, 1.0f),        P(fx.chorus, 0.0f, 1.0f),      P(fx.drive, 0.0f, 1.0f),
    P(fx.dark, 0.0f, 1.0f),         P(fx.crush, 0.0f, 1.0f),       P(fx.pump, 0.0f, 1.0f),
};
#undef P
#define G(f, lo, hi) {(uint16_t)offsetof(tiles_grain_params_t, f), lo, hi}
static const field_t k_grain[] = {
    G(size_ms, 0.0f, 2000.0f),     G(density, 0.0f, 1000.0f),     G(jitter, 0.0f, 1.0f),
    G(spray, 0.0f, 1.0f),          G(scan, -8.0f, 8.0f),          G(scatter_cents, 0.0f, 2400.0f),
    G(octave_up, 0.0f, 1.0f),      G(octave_down, 0.0f, 1.0f),    G(fifth, 0.0f, 1.0f),
    G(reverse, 0.0f, 1.0f),        G(spread, 0.0f, 1.0f),         G(skew, 0.0f, 1.0f),
    G(size_pressure, -4.0f, 4.0f), G(density_pressure, -4.0f, 4.0f), G(spray_pressure, -1.0f, 1.0f),
    G(spray_slide, -1.0f, 1.0f),   G(scatter_slide, -2400.0f, 2400.0f),
};
#undef G
#define N_COMMON (sizeof(k_common) / sizeof(k_common[0]))
#define N_GRAIN (sizeof(k_grain) / sizeof(k_grain[0]))
_Static_assert(OFF_COMMON + 4u * N_COMMON == OFF_GRAIN, "the common floats end where the grain block starts");
_Static_assert(OFF_GRAIN + 4u + 4u * N_GRAIN == OFF_FM, "the grain block ends where the FM block starts");
_Static_assert(OFF_FM + TILES_DX7_VOICE_BYTES + 1u + 8u == RECORD_END, "the FM block's end");
_Static_assert(RECORD_END <= TILES_PRESET_RECORD_BYTES, "a record fits its slot");

static void put_f32(uint8_t *p, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    p[0] = (uint8_t)u;
    p[1] = (uint8_t)(u >> 8);
    p[2] = (uint8_t)(u >> 16);
    p[3] = (uint8_t)(u >> 24);
}

static float get_f32(const uint8_t *p) {
    uint32_t u = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    float v;
    memcpy(&v, &u, 4);
    return v;
}

void tiles_sound_fix(tiles_sound_t *s) {
    s->patch.name = s->name;
    s->patch.fm = s->patch.engine == SYNTH_ENGINE_FM ? &s->fm : NULL;
    s->patch.grain = s->patch.engine == SYNTH_ENGINE_GRAIN ? &s->grain : NULL;
}

void tiles_sound_from_patch(tiles_sound_t *s, const synth_patch_t *p) {
    memset(s, 0, sizeof(*s));
    s->patch = *p;
    if (p->fm != NULL) {
        s->fm = *p->fm;
    }
    if (p->grain != NULL) {
        s->grain = *p->grain;
    }
    if (p->name != NULL) {
        strncpy(s->name, p->name, TILES_PRESET_NAME_MAX);
    }
    tiles_sound_fix(s);
}

void tiles_preset_encode(const tiles_sound_t *s, uint8_t out[TILES_PRESET_RECORD_BYTES]) {
    const synth_patch_t *p = &s->patch;
    memset(out, 0, TILES_PRESET_RECORD_BYTES);
    out[0] = MARK0;
    out[1] = MARK1;
    out[2] = VERSION;
    out[3] = p->engine;
    memcpy(&out[OFF_NAME], s->name, strnlen(s->name, TILES_PRESET_NAME_MAX));
    out[OFF_BYTES + 0u] = p->bank;
    out[OFF_BYTES + 1u] = p->unison;
    out[OFF_BYTES + 2u] = p->filter;
    out[OFF_BYTES + 3u] = p->voice_mode;
    out[OFF_FLAGS] = (uint8_t)((p->phase_reset ? 1u : 0u) | (p->lfo.synced ? 2u : 0u) | (p->lfo.retrigger ? 4u : 0u));
    out[OFF_SHAPE] = p->lfo.shape;
    for (uint32_t i = 0; i < N_COMMON; i++) {
        put_f32(&out[OFF_COMMON + 4u * i], *(const float *)((const uint8_t *)p + k_common[i].at));
    }
    out[OFF_GRAIN + 0u] = s->grain.source;
    out[OFF_GRAIN + 1u] = s->grain.slot;
    out[OFF_GRAIN + 2u] = s->grain.root;
    for (uint32_t i = 0; i < N_GRAIN; i++) {
        put_f32(&out[OFF_GRAIN + 4u + 4u * i], *(const float *)((const uint8_t *)&s->grain + k_grain[i].at));
    }
    memcpy(&out[OFF_FM], s->fm.dx7, TILES_DX7_VOICE_BYTES);
    put_f32(&out[OFF_FM + TILES_DX7_VOICE_BYTES + 1u], s->fm.pressure_db);
    put_f32(&out[OFF_FM + TILES_DX7_VOICE_BYTES + 5u], s->fm.slide_db);
}

/* A float from the record into `dst` (NULL: only checked), clamped. */
static bool take_f32(const uint8_t *p, const field_t *f, uint8_t *base) {
    float v = get_f32(p);
    if (!isfinite(v)) {
        return false;
    }
    if (base != NULL) {
        *(float *)(base + f->at) = v < f->lo ? f->lo : v > f->hi ? f->hi : v;
    }
    return true;
}

bool tiles_preset_decode(const uint8_t in[TILES_PRESET_RECORD_BYTES], tiles_sound_t *out) {
    if (in[0] != MARK0 || in[1] != MARK1 || in[2] != VERSION || in[3] > SYNTH_ENGINE_GRAIN) {
        return false;
    }
    uint32_t name_len = 0;
    while (name_len < TILES_PRESET_NAME_MAX && in[OFF_NAME + name_len] != 0u) {
        uint8_t c = in[OFF_NAME + name_len];
        if (c < 0x20u || c > 0x7Eu) {
            return false;
        }
        name_len++;
    }
    const uint8_t *b = &in[OFF_BYTES];
    if (name_len == 0u || b[0] >= TILES_WT_BANK_COUNT || b[1] > SYNTH_UNISON_MAX || b[2] > SYNTH_FILTER_LADDER24 ||
        b[3] > SYNTH_LEGATO || in[OFF_SHAPE] > SYNTH_LFO_RANDOM || in[OFF_GRAIN] > TILES_GRAIN_SRC_TAPE ||
        in[OFF_GRAIN + 1u] > 7u || in[OFF_GRAIN + 2u] > 127u) {
        return false;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->patch.engine = in[3];
        memcpy(out->name, &in[OFF_NAME], name_len);
        out->patch.bank = b[0];
        out->patch.unison = b[1]; /* 0 plays as 1 */
        out->patch.filter = b[2];
        out->patch.voice_mode = b[3];
        out->patch.phase_reset = (in[OFF_FLAGS] & 1u) != 0u;
        out->patch.lfo.synced = (in[OFF_FLAGS] & 2u) != 0u;
        out->patch.lfo.retrigger = (in[OFF_FLAGS] & 4u) != 0u;
        out->patch.lfo.shape = in[OFF_SHAPE];
        out->grain.source = in[OFF_GRAIN];
        out->grain.slot = in[OFF_GRAIN + 1u];
        out->grain.root = in[OFF_GRAIN + 2u];
        memcpy(out->fm.dx7, &in[OFF_FM], TILES_DX7_VOICE_BYTES);
    }
    uint8_t *patch = out != NULL ? (uint8_t *)&out->patch : NULL;
    uint8_t *grain = out != NULL ? (uint8_t *)&out->grain : NULL;
    for (uint32_t i = 0; i < N_COMMON; i++) {
        if (!take_f32(&in[OFF_COMMON + 4u * i], &k_common[i], patch)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < N_GRAIN; i++) {
        if (!take_f32(&in[OFF_GRAIN + 4u + 4u * i], &k_grain[i], grain)) {
            return false;
        }
    }
    static const field_t k_fm_db = {0u, 0.0f, 24.0f};
    float db[2] = {get_f32(&in[OFF_FM + TILES_DX7_VOICE_BYTES + 1u]), get_f32(&in[OFF_FM + TILES_DX7_VOICE_BYTES + 5u])};
    if (!isfinite(db[0]) || !isfinite(db[1])) {
        return false;
    }
    if (out != NULL) {
        out->fm.pressure_db = db[0] < k_fm_db.lo ? k_fm_db.lo : db[0] > k_fm_db.hi ? k_fm_db.hi : db[0];
        out->fm.slide_db = db[1] < k_fm_db.lo ? k_fm_db.lo : db[1] > k_fm_db.hi ? k_fm_db.hi : db[1];
        tiles_sound_fix(out);
    }
    return true;
}

/* ---- the store ---- */

static tiles_kv_t s_kv[TILES_PRESET_GROUPS];
static uint8_t s_stored[TILES_PRESET_SLOTS / 8u]; /* a bit a slot holding a record */
static uint8_t s_record[TILES_PRESET_RECORD_BYTES];

static bool stored(uint8_t slot) {
    return (s_stored[slot / 8u] >> (slot % 8u) & 1u) != 0u;
}

static void mark(uint8_t slot, bool on) {
    s_stored[slot / 8u] = (uint8_t)(on ? s_stored[slot / 8u] | 1u << (slot % 8u) : s_stored[slot / 8u] & ~(1u << (slot % 8u)));
}

/* Slot's record from flash into s_record. */
static bool read_record(uint8_t slot) {
    tiles_kv_t *kv = &s_kv[slot / TILES_PRESET_GROUP_SLOTS];
    tiles_kv_info_t info = tiles_kv_get_info(kv);
    if (kv->ops == NULL || info.newest < 0) {
        return false;
    }
    uint32_t at = TILES_KV_HEADER_SIZE + (uint32_t)(slot % TILES_PRESET_GROUP_SLOTS) * TILES_PRESET_RECORD_BYTES;
    return kv->ops->read((uint8_t)info.newest, at, s_record, TILES_PRESET_RECORD_BYTES);
}

void tiles_presets_init(const tiles_kv_ops_t *const ops[TILES_PRESET_GROUPS]) {
    memset(s_stored, 0, sizeof(s_stored));
    for (uint32_t g = 0; g < TILES_PRESET_GROUPS; g++) {
        tiles_kv_init(&s_kv[g], ops != NULL ? ops[g] : NULL);
        for (uint32_t k = 0; k < TILES_PRESET_GROUP_SLOTS; k++) {
            uint8_t slot = (uint8_t)(g * TILES_PRESET_GROUP_SLOTS + k);
            mark(slot, read_record(slot) && tiles_preset_decode(s_record, NULL));
        }
    }
}

bool tiles_preset_stored(uint8_t slot) {
    return slot < TILES_PRESET_SLOTS && stored(slot);
}

bool tiles_preset_get(uint8_t slot, tiles_sound_t *out) {
    if (slot >= TILES_PRESET_SLOTS) {
        return false;
    }
    if (stored(slot) && read_record(slot) && tiles_preset_decode(s_record, out)) {
        return true;
    }
    if (slot < TILES_PRESET_FACTORY_SLOTS && g_factory_presets[slot].name != NULL) {
        tiles_sound_from_patch(out, &g_factory_presets[slot]);
        return true;
    }
    return false;
}

void tiles_preset_name(uint8_t slot, char name[TILES_PRESET_NAME_MAX + 1u]) {
    memset(name, 0, TILES_PRESET_NAME_MAX + 1u);
    if (slot >= TILES_PRESET_SLOTS) {
        return;
    }
    if (stored(slot) && read_record(slot) && tiles_preset_decode(s_record, NULL)) {
        memcpy(name, &s_record[OFF_NAME], TILES_PRESET_NAME_MAX);
    } else if (slot < TILES_PRESET_FACTORY_SLOTS && g_factory_presets[slot].name != NULL) {
        strncpy(name, g_factory_presets[slot].name, TILES_PRESET_NAME_MAX);
    }
}

/* The slot's group rebuilt in `staging` with `record` (NULL: cleared) in
 * its place, written to the group's other sector. */
static tiles_preset_result_t write_slot(uint8_t slot, const uint8_t *record, uint8_t *staging) {
    if (slot >= TILES_PRESET_SLOTS) {
        return TILES_PRESET_ERR_SLOT;
    }
    tiles_kv_t *kv = &s_kv[slot / TILES_PRESET_GROUP_SLOTS];
    if (kv->ops == NULL) {
        return TILES_PRESET_ERR_NO_STORAGE;
    }
    const uint16_t len = (uint16_t)(TILES_PRESET_GROUP_SLOTS * TILES_PRESET_RECORD_BYTES);
    uint16_t got = 0, version = 0;
    if (!tiles_kv_read(kv, staging, len, &got, &version) || got != len) {
        memset(staging, 0, len);
    }
    uint8_t *at = &staging[(slot % TILES_PRESET_GROUP_SLOTS) * TILES_PRESET_RECORD_BYTES];
    if (record != NULL) {
        memcpy(at, record, TILES_PRESET_RECORD_BYTES);
    } else {
        memset(at, 0, TILES_PRESET_RECORD_BYTES);
    }
    if (tiles_kv_write(kv, staging, len, 1u) != TILES_KV_OK) {
        return TILES_PRESET_ERR_FLASH;
    }
    mark(slot, record != NULL);
    return TILES_PRESET_OK;
}

tiles_preset_result_t tiles_preset_put(uint8_t slot, const uint8_t record[TILES_PRESET_RECORD_BYTES],
                                       uint8_t *staging) {
    if (!tiles_preset_decode(record, NULL)) {
        return TILES_PRESET_ERR_RECORD;
    }
    return write_slot(slot, record, staging);
}

tiles_preset_result_t tiles_preset_clear(uint8_t slot, uint8_t *staging) {
    return write_slot(slot, NULL, staging);
}

const char *tiles_preset_result_name(tiles_preset_result_t r) {
    switch (r) {
    case TILES_PRESET_OK:
        return "ok";
    case TILES_PRESET_ERR_NO_STORAGE:
        return "no-storage";
    case TILES_PRESET_ERR_SLOT:
        return "slot";
    case TILES_PRESET_ERR_RECORD:
        return "record";
    default:
        return "flash";
    }
}
