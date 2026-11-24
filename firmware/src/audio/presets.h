#pragma once

/* The 96 preset slots (docs/architecture/sound-roadmap.md, "Presets"): a
 * slot written from the computer holds its record in flash; one never
 * written plays its factory sound (factory_presets.c, in flash too); the
 * rest are empty. A track plays a RAM copy of its sound (tiles_sound_t,
 * audio_engine.c), so core 1 never reads flash. Records live 8 to a
 * kv_store group (two sectors, power-cut safe), 12 groups. Plain C over
 * the flash ops, tested on the computer (test/test_presets.c). The
 * record's byte layout is in README.md ("Preset record"). */

#include "fm_voice.h"
#include "grain_voice.h"
#include "kv_store.h"
#include "synth_voice.h"

#include <stdbool.h>
#include <stdint.h>

#define TILES_PRESET_SLOTS 96u
#define TILES_PRESET_GROUP_SLOTS 8u
#define TILES_PRESET_GROUPS (TILES_PRESET_SLOTS / TILES_PRESET_GROUP_SLOTS)
#define TILES_PRESET_RECORD_BYTES 508u
#define TILES_PRESET_NAME_MAX 10u
/* The factory sounds' slots: pages 1 and 2, laid out as the sound menu
 * shows them (24 a page, a row a role). */
#define TILES_PRESET_FACTORY_SLOTS 48u

_Static_assert(TILES_PRESET_GROUP_SLOTS * TILES_PRESET_RECORD_BYTES <= TILES_KV_MAX_PAYLOAD,
               "a group's records fit one kv_store payload");

/* A sound with everything it points at, so it can be copied whole: patch's
 * name, fm and grain point at the members here (tiles_sound_fix()). */
typedef struct {
    synth_patch_t patch;
    tiles_grain_params_t grain;
    tiles_fm_patch_t fm;
    char name[TILES_PRESET_NAME_MAX + 1u];
} tiles_sound_t;

/* After a copy: the patch's pointers at this sound's own members. */
void tiles_sound_fix(tiles_sound_t *s);

/* A copy of `p` and what it points at. */
void tiles_sound_from_patch(tiles_sound_t *s, const synth_patch_t *p);

/* The factory sounds by slot (an empty slot: name NULL), and the four
 * tracks' sounds at power-up. */
extern const synth_patch_t g_factory_presets[TILES_PRESET_FACTORY_SLOTS];
extern const uint8_t g_factory_track_default[4];

/* Core 0 at boot: the groups' flash ops (ops NULL, or any group's NULL:
 * the factory sounds only). */
void tiles_presets_init(const tiles_kv_ops_t *const ops[TILES_PRESET_GROUPS]);

/* Slot 0-95 into `out`: its stored record, else its factory sound; false
 * if it is empty. */
bool tiles_preset_get(uint8_t slot, tiles_sound_t *out);
/* Its record (as a stored one, or its factory sound encoded); false if
 * empty. */
bool tiles_preset_record(uint8_t slot, uint8_t out[TILES_PRESET_RECORD_BYTES]);
/* Its name ("" if empty). */
void tiles_preset_name(uint8_t slot, char name[TILES_PRESET_NAME_MAX + 1u]);
/* A slot holding a record from the computer (not its factory sound). */
bool tiles_preset_stored(uint8_t slot);

/* The record: little-endian, fixed offsets, versioned; zeros past what a
 * version defines. decode() checks the marker, the version, the engine
 * and every value (finite, enums in range). */
void tiles_preset_encode(const tiles_sound_t *s, uint8_t out[TILES_PRESET_RECORD_BYTES]);
bool tiles_preset_decode(const uint8_t in[TILES_PRESET_RECORD_BYTES], tiles_sound_t *out);

typedef enum {
    TILES_PRESET_OK = 0,
    TILES_PRESET_ERR_NO_STORAGE,
    TILES_PRESET_ERR_SLOT,
    TILES_PRESET_ERR_RECORD, /* doesn't decode */
    TILES_PRESET_ERR_FLASH,
} tiles_preset_result_t;

/* Core 0, while the sound is quiet (an erase is tens of ms): writes a
 * record into a slot, or clears it back to its factory sound. `staging`
 * is a 4 KB buffer (flash_staging.h) the group is rebuilt in. */
tiles_preset_result_t tiles_preset_put(uint8_t slot, const uint8_t record[TILES_PRESET_RECORD_BYTES],
                                       uint8_t *staging);
tiles_preset_result_t tiles_preset_clear(uint8_t slot, uint8_t *staging);

const char *tiles_preset_result_name(tiles_preset_result_t r);
