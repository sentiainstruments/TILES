#include "sample_slot.h"

#include "kv_store.h"

#include <string.h>

_Static_assert(TILES_SAMPLE_HEADER_BYTES + TILES_SAMPLE_MAX_FRAMES_MONO * 2u <= TILES_SAMPLE_SLOT_BYTES,
               "the mono cap fits a slot");
_Static_assert(TILES_SAMPLE_HEADER_BYTES + TILES_SAMPLE_MAX_FRAMES_STEREO * 4u <= TILES_SAMPLE_SLOT_BYTES,
               "the stereo cap fits a slot");

#define HDR_CRC_AT 44u

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t tiles_sample_max_frames(uint8_t channels) {
    return channels == 1u ? TILES_SAMPLE_MAX_FRAMES_MONO
                          : channels == 2u ? TILES_SAMPLE_MAX_FRAMES_STEREO : 0u;
}

uint32_t tiles_sample_audio_bytes(const tiles_sample_info_t *info) {
    return info->frames * info->channels * 2u;
}

bool tiles_sample_info_valid(const tiles_sample_info_t *info) {
    uint32_t cap = tiles_sample_max_frames(info->channels);
    if (cap == 0u || info->frames == 0u || info->frames > cap) {
        return false;
    }
    size_t n = strnlen(info->name, sizeof(info->name));
    if (n == 0u || n > TILES_SAMPLE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (info->name[i] <= ' ' || info->name[i] > '~') {
            return false;
        }
    }
    return true;
}

bool tiles_sample_header_encode(const tiles_sample_info_t *info, uint8_t out[TILES_SAMPLE_HEADER_BYTES]) {
    if (!tiles_sample_info_valid(info)) {
        return false;
    }
    memset(out, 0xFF, TILES_SAMPLE_HEADER_BYTES);
    put_u32(&out[0], TILES_SAMPLE_MAGIC);
    put_u16(&out[4], TILES_SAMPLE_LAYOUT);
    out[6] = info->channels;
    out[7] = 16u;
    put_u32(&out[8], TILES_SAMPLE_RATE_HZ);
    put_u32(&out[12], info->frames);
    put_u32(&out[16], info->audio_crc);
    memset(&out[20], 0, TILES_SAMPLE_NAME_MAX + 1u);
    memcpy(&out[20], info->name, strnlen(info->name, TILES_SAMPLE_NAME_MAX));
    put_u32(&out[HDR_CRC_AT], tiles_kv_crc32(0u, out, HDR_CRC_AT));
    return true;
}

bool tiles_sample_header_decode(const uint8_t *in, tiles_sample_info_t *info) {
    if (get_u32(&in[0]) != TILES_SAMPLE_MAGIC || get_u16(&in[4]) != TILES_SAMPLE_LAYOUT || in[7] != 16u ||
        get_u32(&in[8]) != TILES_SAMPLE_RATE_HZ || get_u32(&in[HDR_CRC_AT]) != tiles_kv_crc32(0u, in, HDR_CRC_AT)) {
        return false;
    }
    tiles_sample_info_t got;
    memset(&got, 0, sizeof(got));
    got.channels = in[6];
    got.frames = get_u32(&in[12]);
    got.audio_crc = get_u32(&in[16]);
    memcpy(got.name, &in[20], TILES_SAMPLE_NAME_MAX);
    got.name[TILES_SAMPLE_NAME_MAX] = '\0';
    if (!tiles_sample_info_valid(&got)) {
        return false;
    }
    *info = got;
    return true;
}
