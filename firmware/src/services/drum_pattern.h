#pragma once

/* Drum sequencer core: layout, Drum Rack notes, pattern, step player. No
 * hardware; tested in test_drum_pattern.c; drum_seq.c wires it up.
 *   columns 1-4  16 steps, read like a page; a second page (17-32) plays
 *                when anything on it is armed
 *   columns 5-6  8 drums, two per row
 * Drum d of bank b plays TILES_DRUM_FIRST_NOTE + 8b + d (bank 0 = C1-G1,
 * the Drum Rack order). Every note keeps its own 32-step row. */

#include <stdbool.h>
#include <stdint.h>

#define TILES_DRUM_STEPS 32u      /* two pages */
#define TILES_DRUM_PAGE_STEPS 16u
#define TILES_DRUM_VOICES 8u
#define TILES_DRUM_NOTES 128u
#define TILES_DRUM_FIRST_NOTE 36u
#define TILES_DRUM_BANK_MIN (-4)
#define TILES_DRUM_BANK_MAX 10
#define TILES_DRUM_MAX_RATCHET 4u
/* One step = a sixteenth note: 6 of MIDI clock's 24 pulses per beat. */
#define TILES_DRUM_CLOCKS_PER_STEP 6u
#define TILES_DRUM_CLOCKS_PER_BEAT 24u
/* Clock-fired hits have no strike: a fixed velocity, like the sequencer. */
#define TILES_DRUM_STEP_VELOCITY 100u

/* ---- layout ---- */
uint8_t tiles_drum_pad_for_step(uint8_t step);               /* step 0-31 -> its pad on its page */
bool tiles_drum_step_for_pad(uint8_t pad, uint8_t *out_step); /* 0-15 on the page; false for a drum pad */
uint8_t tiles_drum_pad_for_voice(uint8_t voice);              /* 0-7 -> pad */
bool tiles_drum_voice_for_pad(uint8_t pad, uint8_t *out_voice);
/* The note drum `voice` plays in `bank` (bank clamped to the range). */
uint8_t tiles_drum_note(int8_t bank, uint8_t voice);
int8_t tiles_drum_clamp_bank(int bank);

/* ---- pattern ---- */
typedef struct {
    uint32_t armed[TILES_DRUM_NOTES];                         /* bit s = step s */
    uint8_t probability[TILES_DRUM_NOTES][TILES_DRUM_STEPS]; /* 0-100 */
    uint8_t ratchet[TILES_DRUM_NOTES][TILES_DRUM_STEPS];     /* 1-4 hits */
} tiles_drum_pattern_t;

void tiles_drum_pattern_clear(tiles_drum_pattern_t *p);
/* Empties one note's row (steps off, probability 100, no ratchet). */
void tiles_drum_pattern_clear_note(tiles_drum_pattern_t *p, uint8_t note);
bool tiles_drum_pattern_is_armed(const tiles_drum_pattern_t *p, uint8_t note, uint8_t step);
void tiles_drum_pattern_toggle(tiles_drum_pattern_t *p, uint8_t note, uint8_t step);
bool tiles_drum_pattern_note_has_steps(const tiles_drum_pattern_t *p, uint8_t note);
/* 32 if any note has a step armed on page 2, else 16. */
uint8_t tiles_drum_pattern_length(const tiles_drum_pattern_t *p);
/* Clamped to 0-100 and 1-TILES_DRUM_MAX_RATCHET. */
void tiles_drum_pattern_set_probability(tiles_drum_pattern_t *p, uint8_t note, uint8_t step, uint8_t percent);
void tiles_drum_pattern_set_ratchet(tiles_drum_pattern_t *p, uint8_t note, uint8_t step, uint8_t hits);

/* Saving: sparse records (u8 note, u32 armed, u32 edited, then chance and
 * repeats per edited step), little-endian, version 2; version 1 still
 * decodes. Encoding stops before a note that won't fit (`truncated`);
 * decoding clamps and stops at a damaged record. */
#define TILES_DRUM_BLOB_VERSION 2u
uint16_t tiles_drum_pattern_encode(const tiles_drum_pattern_t *p, uint8_t *out, uint16_t cap, bool *truncated);
/* False for an unknown version (nothing decoded). */
bool tiles_drum_pattern_decode(tiles_drum_pattern_t *p, const uint8_t *in, uint16_t len, uint16_t version);

/* ---- player ----
 * Runs while its own flag and the shared clock run; a Start restarts it; a
 * manual start waits for the nearest beat (or, phase-locked, joins on its
 * song step). Each armed note rolls its own chance; ratchets repeat within
 * the step. A hit ends at the note's next hit or the step's end. */
typedef struct {
    void (*note_on)(uint8_t note, uint8_t velocity, void *ctx);
    void (*note_off)(uint8_t note, void *ctx);
    uint32_t (*random)(void *ctx); /* probability rolls */
    void *ctx;
} tiles_drum_output_t;

typedef struct {
    bool running;
    bool pending_start;   /* waiting for the nearest beat */
    bool pending_restart; /* at that beat: step 1 (true) or resume (false) */
    uint8_t step;         /* 0-31: the step playing or parked on */
    bool muted;           /* a step roll owns the notes: the pattern runs on silently (slip) */
    bool phase_lock;      /* steps follow the song position (setting seq.lock) */
    uint32_t step_started_pulse;
    bool sounding[TILES_DRUM_NOTES];
    uint8_t hits_total[TILES_DRUM_NOTES]; /* this step's hits for the note, 0 = not firing */
    uint8_t hits_done[TILES_DRUM_NOTES];
} tiles_drum_player_t;

void tiles_drum_player_init(tiles_drum_player_t *pl);
/* Starts at the nearest beat: from step 1 if `restart`, else resuming the
 * parked step. Sets `running`. */
void tiles_drum_player_start(tiles_drum_player_t *pl, bool restart);
/* Stops in place (the next start resumes there). Notes end on the next
 * advance. */
void tiles_drum_player_pause(tiles_drum_player_t *pl);
/* Parks on step 1 (when stopped). */
void tiles_drum_player_rewind(tiles_drum_player_t *pl);
void tiles_drum_player_end_all(tiles_drum_player_t *pl, const tiles_drum_output_t *out);
/* True while a repeat is still due within the current step. */
bool tiles_drum_player_repeats_pending(const tiles_drum_player_t *pl);
/* Step roll (circle + step): fires every note armed on `step`, all banks,
 * ignoring chance. While `muted` the pattern runs silently. */
void tiles_drum_player_fire_step(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat,
                                 const tiles_drum_output_t *out, uint8_t step, uint8_t velocity);
/* A clock Start `song_pulse` pulses into the song (midi_clock.h): the step
 * the pattern would be on had it played from the top, now. */
void tiles_drum_player_start_at(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat,
                                const tiles_drum_output_t *out, uint32_t pulse_count, uint32_t song_pulse);
/* Call every scan with the clock snapshot (pulse count, song position,
 * running, start_edge: a Start at the song's top). */
void tiles_drum_player_advance(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat, const tiles_drum_output_t *out,
                               uint32_t pulse_count, uint32_t song_pulse, bool clock_running, bool start_edge);
