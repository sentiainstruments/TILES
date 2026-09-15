#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Sequencer pattern persistence across power cycles -- real feedback:
 * "add memory of last session when powered off for the sequencers."
 * Saves into the LAST TWO 4KB sectors of Pico flash (PICO_FLASH_SIZE_
 * BYTES - 2*FLASH_SECTOR_SIZE and - 1*FLASH_SECTOR_SIZE), alternating
 * between them on every save (see tiles_seq_store_save()'s own comment
 * in seq_store.c) so an interrupted/failed write can never corrupt the
 * ONLY copy -- the previous, still-valid slot stays loadable regardless.
 * Comfortably far from the firmware image itself (currently a few
 * hundred KB, out of 4MB total) -- normal `picotool load` reflashing
 * during development only writes the sectors the .uf2 file actually
 * covers, so it never touches this region either.
 *
 * NOT a true "save at the instant power is cut" -- this board has no
 * brownout/VSYS-loss detection (see services/power.h's own header: it
 * only ever reports WHICH source is currently feeding it, never an
 * early warning that all of them are about to disappear) and no energy
 * reserve (supercap/battery) to keep the MCU alive long enough to
 * finish a flash write after power actually goes away, so a literal
 * power-loss-triggered save isn't reliably achievable on this hardware
 * at all -- attempting one risks starting a write that then gets cut
 * mid-erase, which is a real corruption risk this design specifically
 * avoids by never writing outside a deliberate, unhurried moment.
 * Instead, services/op_mode.c auto-saves a few seconds after the last
 * sequencer edit goes quiet (see that file's own seq_store_mark_dirty()
 * and its debounce timer) -- by the time real power loss happens, flash
 * already reflects whatever was last edited, the same way most
 * consumer gear with "remembers your last session" actually works.
 */

#define TILES_SEQ_STORE_NUM_LANES 4u
#define TILES_SEQ_STORE_ALTS_PER_LANE 6u
#define TILES_SEQ_STORE_NUM_STEPS 24u

/* Mirrors services/op_mode.c's own (private) op_seq_pattern_t field for
 * field -- kept as a SEPARATE, explicitly-laid-out struct rather than
 * sharing that one directly, so this file (and the on-flash format it
 * defines) stays fully decoupled from op_mode.c's internal
 * representation; op_mode.c owns converting between the two. */
typedef struct {
    uint8_t step_armed[TILES_SEQ_STORE_NUM_STEPS];
    uint8_t step_pitch_override[TILES_SEQ_STORE_NUM_STEPS];
    uint8_t step_note[TILES_SEQ_STORE_NUM_STEPS];
    uint8_t step_probability_percent[TILES_SEQ_STORE_NUM_STEPS];
    uint8_t step_ratchet_count[TILES_SEQ_STORE_NUM_STEPS];
    uint8_t probability_enabled;
    uint8_t length;
    uint8_t scale; /* tiles_scale_mode_t, narrowed to a byte -- see note_map.h, well under 256 values */
    uint8_t reserved; /* pad to a round size; always written 0, ignored on load */
} tiles_seq_store_pattern_t;

typedef struct {
    uint8_t lane_channel[TILES_SEQ_STORE_NUM_LANES];
    uint8_t active_alt[TILES_SEQ_STORE_NUM_LANES];
    tiles_seq_store_pattern_t patterns[TILES_SEQ_STORE_NUM_LANES][TILES_SEQ_STORE_ALTS_PER_LANE];
} tiles_seq_store_data_t;

/* Reads whichever of the two flash slots holds the newer VALID (magic +
 * format version + CRC32 all check out) copy into `out`. Returns false
 * (leaving `out` untouched) if neither slot has ever been written, or
 * both are corrupt or from an incompatible older format -- callers
 * should keep their own all-empty defaults in that case, the same as a
 * genuinely first boot. Safe to call anytime (a plain memory-mapped
 * flash read through the XIP cache, not a write) -- call once at boot. */
bool tiles_seq_store_load(tiles_seq_store_data_t *out);

/* Erases and programs whichever flash slot is next in the alternation,
 * then advances the alternation for next time. Takes on the order of
 * tens of milliseconds and disables interrupts for that whole window
 * (hardware_flash's own requirement, since flash can't be read via XIP
 * -- which is where this firmware's own code executes from -- while
 * being erased/programmed) -- see this file's own header for why this
 * must only ever be called from a debounce timer, never a hot path. */
void tiles_seq_store_save(const tiles_seq_store_data_t *data);
