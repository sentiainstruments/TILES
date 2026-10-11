#pragma once

/* Drum sequencer core: the grid layout, the Drum Rack note map, the
 * pattern and the step player. Hardware-free (no SDK, no other module), so
 * firmware/test/test_drum_pattern.c runs it natively; services/drum_seq.c
 * wires it to touch, light, haptics, MIDI and the clock.
 *
 * Layout (pads numbered 1-24, top-left first, 6 per row):
 *   columns 1-4  16 steps, read like a page: pads 1-4 are steps 1-4,
 *                pads 7-10 steps 5-8, and so on. A pattern has two pages
 *                (steps 1-16, 17-32): it plays 32 steps when anything is
 *                armed on page 2, else 16.
 *   columns 5-6  8 drums, two per row, top to bottom: pads 5, 6, 11, 12,
 *                17, 18, 23, 24 are drums 1-8.
 *
 * Notes: drum d of bank b plays TILES_DRUM_FIRST_NOTE + 8 b + d, so bank 0
 * is C1-G1 and bank 1 G#1-D#2: Ableton's Drum Rack pads in order (Live
 * names MIDI 36 "C1"; it's also General MIDI's kick). Banks go down to
 * notes 4-11 and up to 116-123.
 *
 * The pattern holds a 32-step row for every MIDI note, so a beat built in
 * one bank keeps playing while another bank is shown. */

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

/* Saving (services/drum_seq.c keeps it in its own flash region). Sparse:
 * only notes with steps or edited chance/repeats, each as
 *   u8 note, u32 armed, u32 edited (steps whose chance or repeats aren't
 *   the default), then per edited step: u8 chance, u8 repeats
 * (little-endian, version 2). A plain beat is 9 bytes per drum. Version 1
 * (firmware 0.2.8, 16 steps) had u16 masks; it still decodes. Encoding
 * stops before a note that wouldn't fit and says so (`truncated`).
 * Decoding clears first, clamps values and stops at a damaged record. */
#define TILES_DRUM_BLOB_VERSION 2u
uint16_t tiles_drum_pattern_encode(const tiles_drum_pattern_t *p, uint8_t *out, uint16_t cap, bool *truncated);
/* False for an unknown version (nothing decoded). */
bool tiles_drum_pattern_decode(tiles_drum_pattern_t *p, const uint8_t *in, uint16_t len, uint16_t version);

/* ---- player ----
 * Follows MIDI clock pulses like a sequencer lane (services/op_mode.c):
 * runs only while both its own `running` flag and the shared clock run; a
 * clock Start restarts it at step 1; a manual start waits for the nearest
 * beat. Every armed note on a step fires at the step's start, each rolling
 * its own probability; ratchets add evenly spaced repeats within the step.
 * A hit ends at the next hit of the same note or when the step ends. */
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
    bool muted;           /* a step roll owns the notes: the playhead stays on its step, silent */
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
/* Step roll (circle + a step): jump moves the playhead to `step` now, on
 * the step grid (step boundaries stay on the clock's sixteenths), so the
 * pattern carries on from there; fire_step sounds every note armed on
 * `step`, all banks, at `velocity` (chance ignored: it's played live),
 * ending whatever was sounding. While `muted` the playhead stays on its
 * step and the player leaves notes alone; clear it to carry on. */
void tiles_drum_player_jump(tiles_drum_player_t *pl, uint8_t step, uint32_t pulse_count);
void tiles_drum_player_fire_step(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat,
                                 const tiles_drum_output_t *out, uint8_t step, uint8_t velocity);
/* Call every scan with the clock snapshot (pulse count, running,
 * start_edge). */
void tiles_drum_player_advance(tiles_drum_player_t *pl, const tiles_drum_pattern_t *pat, const tiles_drum_output_t *out,
                               uint32_t pulse_count, bool clock_running, bool start_edge);
