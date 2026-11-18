#pragma once

/* The DX7 bank in flash: 32 voices, loaded by SYNTH BANK PUT as the
 * 4,104-byte sysex streams in (F0 43 0n 09 20 00, the packed voices, the
 * DX7 checksum, F7), page by page into its region's first sector; the
 * header (CRC-32 of the voices) goes in the second sector last, so a load
 * cut short leaves no bank. Core 0 only; tested on the computer
 * (test/test_fm_bank.c). */

#include "fm_voice.h"
#include "kv_store.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TILES_FM_BANK_OK = 0,
    TILES_FM_BANK_ERR_NO_STORAGE,
    TILES_FM_BANK_ERR_FLASH,
    TILES_FM_BANK_ERR_NOT_DX7,  /* not a 32-voice bank dump */
    TILES_FM_BANK_ERR_CHECKSUM, /* the DX7's own checksum */
    TILES_FM_BANK_ERR_CRC,      /* the bytes arrived changed */
    TILES_FM_BANK_ERR_STATE,    /* no load under way, or one already is */
} tiles_fm_bank_result_t;

/* The region's two sectors (slot 0: the voices, slot 1: the header);
 * ops NULL: no storage. */
void tiles_fm_bank_init(const tiles_kv_ops_t *ops);
bool tiles_fm_bank_present(void);

/* Erases the old bank (header first) and expects the sysex next. */
tiles_fm_bank_result_t tiles_fm_bank_begin(uint32_t sysex_crc32);
tiles_fm_bank_result_t tiles_fm_bank_write(const uint8_t *data, uint32_t len);
uint32_t tiles_fm_bank_remaining(void);
/* Checks the whole and writes the header. */
tiles_fm_bank_result_t tiles_fm_bank_finish(void);

/* Voice 0-31 from the bank in flash (false: none there). */
bool tiles_fm_bank_name(uint8_t voice, char name[11]);
bool tiles_fm_bank_voice(uint8_t voice, uint8_t out[TILES_DX7_VOICE_BYTES]);

const char *tiles_fm_bank_result_name(tiles_fm_bank_result_t r);
