#pragma once

/* Prism mode (harmony.c): a chord machine; the right-hand columns shape the
 * chord the left-hand roots play. The chord maths (qualities, extensions, voicing,
 * strum notes) is services/harmony_core.h; this file is the hardware side.
 * services/op_mode.c owns the mode switch.
 *
 *   columns 1-3   the 12 roots, chromatic: C bottom-left, three per row
 *                 going up (C C# D / D# E F / F# G G# / A A# B). Push one:
 *                 its chord plays until you lift, at the strike's velocity
 *                 (the depth is read over a short follow-through, as on
 *                 melodic pads). They feel like melodic pads: a tick on
 *                 touch, a kick on the strike, a buzz following pressure.
 *   column 4      voicing (bottom to top): root position, 1st inversion, 2nd
 *                 inversion, open. Stays picked until another is.
 *   column 5      quality (bottom to top): major, minor, sus4, diminished.
 *   column 6      extensions, any together (bottom to top): 6, 7, maj7, 9.
 *                 Maj + 7 = dominant 7, Dim + 6 = dim7, Dim + 7 = m7b5.
 *   "-" / "+"     the chords an octave down / up (two each way).
 *
 * Columns 4-6 act on a press (Hall depth past the strike depth), not on a
 * touch, so a resting finger changes nothing. A quality or extension pressed
 * while a chord sounds latches onto that chord (it changes at once, holding
 * the notes the two share) until a new root is pressed; pressed while
 * nothing sounds, it waits for the next root. Pressing the same one again
 * takes it off. Held down while a new root is struck, it carries onto that
 * chord too (hold the shape, then press the root). No quality = major.
 *
 * The root also sounds an octave below as the bass. Pressing harder sends
 * channel pressure. MIDI channel 12 (shared with chord mode; one mode plays
 * at a time); the pedal's CC64 reaches it (pedal.c).
 *
 * Pedal down: the root pads also strum. A touch that stays light plucks one
 * note of the last chord: the 12 pads, bottom-left to top-right, climb its
 * notes from the bass like a harp (a press is a chord instead). In the
 * pedal's HOLD style TILES holds a released chord itself until the pedal
 * comes up.
 *
 * Key lock: with a scale picked (circle + triangle, the scale picker), roots
 * outside it go dark and play no chord (they still strum). Chords are never
 * changed to fit: their colour notes stay. Chromatic = free.
 *
 * Lights: the key note in the colour scheme's root colour (pink by
 * default), its fifth in the fifth colour, the others dim; the held root
 * white, the chord's notes amber, plucked pads bright amber; the voicing,
 * quality and extensions in use bright (major half-bright when no quality
 * is set); blue underglow. */

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
