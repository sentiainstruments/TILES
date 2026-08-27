#pragma once

/*
 * USB MIDI note output -- MPE (MIDI Polyphonic Expression) Lower Zone.
 *
 * Real feedback: "we need to make sure we have individual per note
 * pitch bend not just regular all key pitch bend. like the roli
 * seaboard." Every earlier round of services/expression.c's pitch-bend
 * work had to route around this file's old single-channel limitation --
 * "a channel-wide message with no per-note addressing... this module
 * tracks a single owner pad" -- because Pitch Bend Change (and Poly
 * Aftertouch, though that one's at least addressed by note number) is
 * a channel-wide concept in the MIDI spec itself; there's no way to
 * bend one held note without also bending every other note on the same
 * channel. MPE's fix is real per-note channels, not a workaround: give
 * every simultaneously-held note its own MIDI channel, and Pitch Bend
 * Change on that channel is now genuinely that ONE note's bend.
 *
 * Zone layout (a single "Lower Zone," the simpler and far more common
 * of the two MPE zone configurations -- an "Upper Zone" would only
 * matter for a controller wanting BOTH zones simultaneously, which
 * nothing about this board's 24-pad, single-region layout calls for):
 *   - Channel 1 (TILES_MIDI_MPE_MASTER_CHANNEL) is the Zone Master
 *     Channel -- carries ONLY the zone configuration RPN messages
 *     tiles_midi_mpe_init() sends once at boot, never note data.
 *   - Channels 2-16 (TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL through
 *     TILES_MIDI_MPE_NUM_MEMBER_CHANNELS channels) are Member Channels,
 *     one per currently-held note.
 * This file is only the wire-protocol layer -- every function here just
 * sends whatever channel it's told to. The actual per-note channel
 * ALLOCATION (claim on strike, release on note-off, steal-the-oldest if
 * all 15 are already in use -- mirroring services/haptics.c's own
 * voice-stealing policy for the exact same "ran out of a limited
 * resource" reasoning) lives in services/expression.c, the module that
 * already owns each pad's note lifecycle.
 *
 * Sustain/expression pedal CCs (services/pedal.c) are the one thing
 * that still needs to reach every note at once rather than a single
 * channel -- see tiles_midi_send_cc_broadcast() below.
 */

#include <stdint.h>

#define TILES_MIDI_MPE_MASTER_CHANNEL 0u       /* status-byte channel nibble; 0 = MIDI channel 1 */
#define TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL 1u /* status-byte channel nibble; 1 = MIDI channel 2 */
#define TILES_MIDI_MPE_NUM_MEMBER_CHANNELS 15u /* MIDI channels 2-16 -- the full remaining range */

/* This Lower Zone's declared per-Member-Channel pitch bend range, in
 * semitones, sent via RPN 0 as part of tiles_midi_mpe_init() below. This
 * is purely a RECEIVER-side interpretation setting -- it does not change
 * what 14-bit wire value tiles_midi_send_pitch_bend() computes/sends for
 * a given tilt (that's services/expression.c's own sensitivity tuning,
 * an entirely separate concern); it only tells an MPE-aware receiver how
 * many semitones that +/-8191 wire range should musically span.
 *
 * History: 48 (the MPE specification's own recommended default, and what
 * a real ROLI Seaboard ships with) -- real feedback after trying it:
 * "the pitch bend is so extreme the glide in equator is too extreme."
 * 48 semitones is 4 full octaves of swing at full-scale wire value, which
 * services/expression.c's own sensitivity tuning reaches on any
 * comfortable deliberate tilt (see s_pitch_bend_max_cosine_deviation's
 * own comment) -- musically that's a dramatic swoop, not the subtler
 * per-note "glide" a Seaboard is normally played with. Lowered to 12 (one
 * octave full-scale) as a more reasonable middle ground between the
 * legacy single-channel MIDI default (2, far too tight for an expressive
 * per-note glide) and the MPE spec's own wide default -- a musical/design
 * choice, not something a hardware capture could measure; worth further
 * tuning against real feedback like every other constant in this
 * pipeline. */
#define TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES 12u

/* Sends this Lower Zone's required setup on the Zone Master Channel:
 * the MPE Configuration Message (RPN 6, "MCM" -- declares
 * TILES_MIDI_MPE_NUM_MEMBER_CHANNELS Member Channels in the zone, the
 * message an MPE-aware DAW/synth uses to auto-detect this is an MPE
 * controller at all) followed by the zone's Pitch Bend Sensitivity RPN
 * (RPN 0, TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES). Call once, from
 * main.c after USB MIDI is expected to be reachable -- harmless to call
 * before a host has actually enumerated, every send in this file is
 * already gated on tud_midi_mounted(). */
void tiles_midi_mpe_init(void);

/* Note on/off, on a specific MPE Member Channel (status-byte nibble --
 * see services/expression.c's per-pad MPE channel allocator for how a
 * pad's currently-held note gets one). */
void tiles_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void tiles_midi_note_off(uint8_t channel, uint8_t note);

/* Channel Pressure (0xD0 | channel, pressure) on a specific Member
 * Channel -- this, not Poly Key Pressure, is MPE's actual Z-dimension
 * message. Real feedback after the first MPE flash: "you broke mpe
 * preassure." Root cause: this used to send Poly Key Pressure (0xA0),
 * which is valid MIDI but isn't what an MPE-aware receiver listens for --
 * MPE's three per-note dimensions are Pitch Bend (X), CC74 (Y), and
 * Channel Pressure (Z) specifically, because under MPE a channel IS a
 * note, so channel-wide pressure is already per-note pressure with no
 * note field needed. Channel Pressure is a 2-data-byte message (no note
 * number), unlike every other message in this file. */
void tiles_midi_send_channel_pressure(uint8_t channel, uint8_t pressure);

/* Sends a Control Change message (0xB0 | channel, controller, value) on
 * one specific channel. */
void tiles_midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value);

/* Same CC on the Zone Master Channel AND every one of the 15 Member
 * Channels -- what services/pedal.c uses for sustain (CC64) and
 * expression (CC11) instead of the single-channel function above. Under
 * MPE there is no single "right" channel for a pedal message: sustain
 * needs to hold EVERY currently-sounding note across however many
 * Member Channels are in use, and unlike a note-specific message
 * there's no per-note channel to target. Broadcasting to the full fixed
 * range (not just currently-active channels) is simpler and safer than
 * services/expression.c's allocator having to expose which channels are
 * live right now -- 16 short CC messages on a state change (sustain
 * press/release, or an expression pedal value crossing a MIDI-CC step)
 * is cheap and infrequent. */
void tiles_midi_send_cc_broadcast(uint8_t controller, uint8_t value);

/* Sends a Pitch Bend Change (0xE0 | channel, LSB, MSB) on one specific
 * Member Channel. bend_14bit is the full unsigned wire value (0-16383,
 * 8192 = center/no bend) -- callers do the signed-to-wire conversion
 * themselves. Genuinely per-note now that every held note has its own
 * channel -- see this file's header for the full MPE reasoning. */
void tiles_midi_send_pitch_bend(uint8_t channel, uint16_t bend_14bit);
