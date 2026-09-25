#pragma once

/*
 * MIDI output -- USB and DIN -- MPE (MIDI Polyphonic Expression) Lower Zone.
 *
 * Every function below EXCEPT tiles_midi_send_daw_cc() and
 * tiles_midi_send_sysex() sends on both USB (when a host has the device
 * mounted) and the DIN MIDI OUT jack (whenever DIN initialized, host or not)
 * -- see midi/din_midi.h. Those two are USB-only on purpose: they carry the
 * DAW remote script's private control protocol, which an instrument on the
 * DIN jack must never receive.
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
 * NAME is a RECEIVER-side interpretation setting -- it doesn't directly
 * determine what tiles_midi_send_pitch_bend() puts on the wire -- but as
 * of services/expression.c's PITCH_BEND_WIRE_RANGE_COMPENSATION, this
 * constant's VALUE is also read there to derive that wire-level scaling,
 * so it is no longer safe to change this number alone expecting only the
 * RPN to change; see that constant's own comment for why both now have
 * to move together.
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
 * per-note glide) and the MPE spec's own wide default.
 *
 * That alone didn't hold up under real testing: "reduce the range of
 * pitch bend, rn we can bend 4 ocvave" came back even with this already
 * at 12, traced (see tiles_midi_mpe_init()'s own comment) to the RPN
 * only being sent on the Zone Master Channel and not every receiver
 * generalizing that zone-wide -- fixed by also sending it on every
 * Member Channel. Still didn't hold up: "even tho you say that its
 * reduced to one octave it still does more in Equator mpe mode," then,
 * after trying a completely different synth from a different vendor,
 * "tried serum and also is bending too far. so its not roli. the tilt
 * pushes too far." Two unrelated receivers both still swinging at
 * roughly the spec's 48-semitone default regardless of the RPN sent on
 * every channel means dynamically honoring a third-party controller's
 * Pitch Bend Sensitivity RPN just isn't something real-world MPE hosts/
 * plugins reliably do in practice, spec-legal or not -- ROLI's own docs
 * confirm Equator's range is a value the user sets manually to match the
 * controller, not one it negotiates automatically. The RPN sends here
 * stay (correct and harmless for any receiver that does honor them), but
 * services/expression.c no longer trusts them alone -- see PITCH_BEND_
 * WIRE_RANGE_COMPENSATION's own comment for the defensive fix that
 * doesn't depend on receiver cooperation at all. */
#define TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES 12u

/* Sends this Lower Zone's required setup: the MPE Configuration Message
 * (RPN 6, "MCM" -- declares TILES_MIDI_MPE_NUM_MEMBER_CHANNELS Member
 * Channels in the zone, the message an MPE-aware DAW/synth uses to
 * auto-detect this is an MPE controller at all), sent once on the Zone
 * Master Channel per spec, followed by the Pitch Bend Sensitivity RPN
 * (RPN 0, TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES) -- sent on the
 * Master Channel (the spec's own "applies zone-wide" convention) AND
 * redundantly on EVERY Member Channel individually. Real feedback:
 * "reduce the range of pitch bend, rn we can bend 4 octave" -- reported
 * with TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES already set to 12 (one
 * octave) in firmware, not 48 -- "4 octaves" is EXACTLY the MPE spec's
 * own recommended default a receiver would fall back to if it never
 * received an explicit override on the channel it's actually reading
 * pitch bend from. Not every real MPE receiver fully generalizes a
 * Master-Channel-only Pitch Bend Sensitivity to the whole zone despite
 * what the spec says should happen; sending the same RPN on each
 * Member Channel too is a well-known, low-risk robustness workaround
 * for exactly that gap -- redundant on a receiver that already handles
 * the Master-Channel version correctly, but a real fix for one that
 * doesn't. Call once, from main.c after USB MIDI is expected to be
 * reachable -- harmless to call before a host has actually enumerated:
 * the USB half of every send is gated on tud_midi_mounted(). main.c ALSO
 * calls it once at boot, when only DIN is up, so a receiver on the DIN jack
 * gets the zone configuration too (~300 bytes, ~100 ms of wire time); the
 * mount-time call then repeats it to both. */
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
 * one specific channel -- USB and DIN. */
void tiles_midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value);

/* Same message, USB ONLY -- for CCs that steer the DAW's remote script (the
 * transport Play/Stop/Record CCs and every Scene Launch grid/stop/offset/
 * delete/capture CC in services/op_mode.c) rather than an instrument. They
 * ride channel 1 with controller numbers a hardware synth may well have
 * mapped to something, so mirroring them to DIN would have made pressing
 * the transport button or a scene pad twiddle whatever is plugged into the
 * jack. Added with DIN MIDI OUT for exactly that reason. */
void tiles_midi_send_daw_cc(uint8_t channel, uint8_t controller, uint8_t value);

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

/* MIDI System Realtime Start (0xFA) / Stop (0xFC) -- single status byte,
 * no channel nibble at all (these apply to the whole MIDI stream, not
 * one channel), used by services/op_mode.c's diamond transport toggle to
 * remote-control a DAW's transport. Real feedback: "the diamond for now
 * will play and stop in ableton like a toggle and stop brings back to
 * the start always." With a DAW's MIDI input "Sync"/"Ext" enabled (in
 * Ableton Live: Preferences -> Link/MIDI, Sync column on the relevant
 * input port, then the transport's own Ext button), these two messages
 * fully drive its transport, and Start is spec-defined to always begin
 * from position 0 -- never resumes from wherever a Continue message
 * would. That's what makes "stop brings back to the start always" true
 * for free: this pair is deliberately never joined by a Continue sender
 * anywhere in this codebase, so every "play" is a Start, never a
 * resume. */
void tiles_midi_send_start(void);
void tiles_midi_send_stop(void);
/* (Both also go out the DIN jack -- a hardware sequencer or drum machine
 * slaved to this controller's transport wants them just as much as a DAW
 * does. Still skipped by op_mode.c while an external clock is driving us.) */

/* Real feedback: "lets implemebt a new mode that triggers scenes in
 * ableton live... can we pull the colors of the scenes from ableton?"
 * -- services/op_mode.c's Scene Launch mode's own outgoing half (fire
 * clip/launch scene) needs a real SysEx sender, this codebase's first;
 * see midi/midi_in.h for the matching incoming half and shared/protocol/
 * README.md's own "Scene Launch" section for the actual message
 * catalog. Wraps `data`/`len` in 0xF0/0xF7 and writes it in one
 * tud_midi_stream_write() call -- `len` is expected to comfortably fit
 * this codebase's own message sizes (well under TUD_MIDI's own 64-byte
 * packet), not a general large-SysEx streaming API. */
void tiles_midi_send_sysex(const uint8_t *data, uint32_t len);
/* (USB only -- see the header comment.) */
