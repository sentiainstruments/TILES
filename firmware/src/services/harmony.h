#pragma once

/* Prism mode, hardware side (chord maths: harmony_core.h), channel 12.
 *   columns 1-3   12 chromatic roots, C bottom-left: push to play its chord
 *                 (with an octave-down bass) until you lift
 *   column 4      voicing: root, 1st and 2nd inversion, open
 *   column 5      quality: major, minor, sus4, diminished
 *   column 6      extensions, any together: 6, 7, maj7, 9
 *   "-" / "+"     chords an octave down / up
 * Columns 4-6 act on a press: pressed while a chord sounds they latch onto
 * it; held while a root strikes they carry over. Pedal down: light touches
 * on the roots strum the last chord like a harp. A picked scale darkens
 * roots outside it (chords never change). */

#include <stdint.h>

/* The mode's color (menu pad, underglow): blue, the harmonic family. */
#define TILES_HARMONY_COLOR_R 0.15f
#define TILES_HARMONY_COLOR_G 0.35f
#define TILES_HARMONY_COLOR_B 1.0f

void tiles_harmony_init(void);
/* Entering: a finger already down isn't a new press. */
void tiles_harmony_enter(void);
/* Leaving: ends the chord and any plucks. */
void tiles_harmony_leave(void);
/* While another feature has the pads (the Feel menu, transpose): the held
 * chord keeps sounding and ends when its root lifts (or the pedal lets go);
 * plucks still end; no new chords. */
void tiles_harmony_track_releases(uint32_t now_ms);

/* Every scan while the mode is shown. */
void tiles_harmony_handle_input(uint32_t now_ms);
void tiles_harmony_render(uint32_t now_ms, float beat_flash, float diamond_led);
