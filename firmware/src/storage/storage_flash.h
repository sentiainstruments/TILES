#pragma once

/*
 * The real flash operations under storage/kv_store.h: reading through the XIP
 * window, erasing and programming a store's two sectors (flash_map.h) --
 * one set of ops for the settings, one for the content store.
 */

#include "kv_store.h"

#include <stdbool.h>
#include <stdint.h>

/* Brackets every flash erase or program in the firmware. XIP is off while
 * they run, so the audio core (audio/usb_audio.c) is paused and interrupts
 * are masked here. False if that core didn't pause within 10 ms: nothing
 * was done, skip the write (counted in INFO flash.lockout_fails). */
bool tiles_flash_write_begin(uint32_t *irq_state);
void tiles_flash_write_end(uint32_t irq_state);
uint32_t tiles_flash_lockout_fails(void);

/* Run inside every write, with core 1 paused: `before` stops the drum
 * sampler's flash DMA (audio/sample_stream.c), `after` lets it go on. */
void tiles_flash_set_write_hooks(void (*before)(void), void (*after)(void));

/* The drum sample slots (flash_map.h): false if the image grew into them.
 * Erase one 4 KB sector, or program one 256-byte page, at an offset from
 * the region's start. */
bool tiles_storage_sample_region_safe(void);
bool tiles_storage_sample_erase(uint32_t offset);
bool tiles_storage_sample_program(uint32_t offset, const uint8_t *page);

/* False if the application image has grown into the settings region -- then
 * nothing here may write, or it would corrupt the running program. */
bool tiles_storage_settings_region_safe(void);

/* Flash ops for the settings store. Only use when the check above is true. */
const tiles_kv_ops_t *tiles_storage_settings_ops(void);

/* The same pair for the content store (custom scales), just below settings. */
bool tiles_storage_content_region_safe(void);
const tiles_kv_ops_t *tiles_storage_content_ops(void);

/* And for drum mode's pattern, below the content store. */
bool tiles_storage_drum_region_safe(void);
const tiles_kv_ops_t *tiles_storage_drum_ops(void);

/* And for the mode, scale, key and octave (services/play_state.c). */
bool tiles_storage_state_region_safe(void);
const tiles_kv_ops_t *tiles_storage_state_ops(void);

/* And for the sequencer's autosave (services/op_mode.c). */
bool tiles_storage_seq_region_safe(void);
const tiles_kv_ops_t *tiles_storage_seq_ops(void);

/* And for the last caught crash (diagnostics/fault.c). */
bool tiles_storage_fault_region_safe(void);
const tiles_kv_ops_t *tiles_storage_fault_ops(void);

/* And for the DX7 bank (audio/fm_bank.c), lowest: its two sectors as the
 * two slots. */
bool tiles_storage_fm_bank_region_safe(void);
const tiles_kv_ops_t *tiles_storage_fm_bank_ops(void);

/* And for the presets (audio/presets.c), below it: group 0-11's two
 * sectors as the two slots. */
bool tiles_storage_preset_region_safe(void);
const tiles_kv_ops_t *tiles_storage_preset_ops(uint8_t group);
