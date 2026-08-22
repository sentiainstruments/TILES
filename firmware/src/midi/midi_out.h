#pragma once

/*
 * USB MIDI note output.
 *
 * V1 scope, deliberately minimal: single MIDI channel, fixed velocity.
 * No MPE per-note channel allocation yet (needs the voice-stealing
 * policy described in docs/architecture/defaults-and-safeguards.md),
 * and no real velocity/pressure -- Hall isn't calibrated yet, so there
 * is no depth signal to derive it from (see "V1 sensing scope" in the
 * same doc). This gets sound happening at all; MPE and real
 * velocity/pressure are a later layer once Hall calibration exists.
 */

#include <stdint.h>

void tiles_midi_note_on(uint8_t note);
void tiles_midi_note_off(uint8_t note);
