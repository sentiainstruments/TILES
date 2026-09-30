#pragma once

/* MIDI output, USB and DIN, including the MPE (MIDI Polyphonic Expression)
 * Lower Zone.
 *
 * Every function sends on the USB MAIN port (when mounted) and the DIN OUT
 * jack (when initialized), except:
 *   - tiles_midi_send_daw_cc(): USB DAW port only (midi/midi_ports.h), the
 *     DAW remote script's private protocol, which no instrument or DIN
 *     device should see.
 *   - tiles_midi_send_sysex(): one USB port, the caller's choice.
 *
 * MPE gives each held note its own channel, so channel-wide messages
 * (pitch bend, pressure) become per-note. Lower Zone layout:
 *   - Ch 1 (TILES_MIDI_MPE_MASTER_CHANNEL): Zone Master Channel. Zone RPNs
 *     and zone-wide controls (pedals), never a note. With MPE off, every
 *     note goes here.
 *   - Ch 2-9: Member Channels, one per held note. How many are really in
 *     the zone depends on Song mode (services/midi_channels.h, which owns
 *     the full channel map). Ch 10-16 are fixed parts or unused.
 *
 * This file is the wire layer only: it sends on whatever channel it is
 * given. Channel allocation (claim on strike, release on Note-Off, steal
 * when full) lives in services/expression.c. Pedals reach all notes via
 * the Master Channel, not a broadcast; only panic broadcasts. */

#include <stdint.h>

#include "midi_ports.h"

#define TILES_MIDI_MPE_MASTER_CHANNEL 0u       /* status nibble; 0 = MIDI channel 1 */
#define TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL 1u /* status nibble; 1 = MIDI channel 2 */
/* The zone size is not fixed at 15: ch 10-16 are fixed parts or GM drums,
 * and ch 2-9 are shared with Song mode, so the zone is 0-8 channels.
 * tiles_midi_mpe_init() takes the size (services/midi_channels.h,
 * tiles_midi_channels_declare_zone()). */

/* Pitch bend range declared per Member Channel (RPN 0), in semitones. Also
 * used by services/expression.c's PITCH_BEND_WIRE_RANGE_COMPENSATION to
 * scale the wire value, so change them together.
 *
 * 12 (one octave), not the MPE default 48: 48 made normal tilts swoop
 * several octaves. Real receivers (Equator, Serum) often ignore the RPN
 * and stay at 48, which is why expression.c also compensates on the wire
 * side. Background: services/HISTORY.md. */
#define TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES 12u

/* Sends the zone setup: the MPE Configuration Message (RPN 6, declaring
 * `member_channel_count` Member Channels; how an MPE host detects an MPE
 * controller) on the Master Channel, then Pitch Bend Sensitivity (RPN 0)
 * on the Master Channel AND each Member Channel. The per-member copies are
 * a common robustness measure for receivers that don't apply the Master
 * Channel's RPN zone-wide.
 *
 * Sent by services/expression.c's tiles_expression_announce_mpe_zone():
 * at boot after settings load (so DIN receivers get it; ~300 bytes,
 * ~100 ms), and on every USB mount. The USB half is gated on
 * tud_midi_mounted(). */
/* `member_channel_count` is the zone's current size (0-8). RPN 0 goes only
 * to channels inside it, never to a fixed part or Song channel.
 *
 * 0 withdraws the zone (RPN 6 only; "an MPE zone can be turned off by
 * sending an MCM without any member channels"), used when
 * `expression.mpe_enabled` is off. A non-zero RPN 6 resets JUCE receivers
 * to 48 semitones, so RPN 0 always follows it. Only services/expression.c
 * calls this, so the declaration always matches the MPE setting. */
void tiles_midi_mpe_init(uint8_t member_channel_count);

/* MPE per-note setup (spec 3.3.1/3.3.4), right before a Note-On: centers
 * `channel`'s pitch bend and zeroes its pressure, skipping either if it
 * is already there (the last sent values are tracked per channel). Bend is
 * NOT recentered at Note-Off: control ends at Note-Off and the tail rings
 * as played; the reset is setup for the next note on that channel. */
void tiles_midi_send_note_setup(uint8_t channel);

/* Note on/off on a channel (status nibble). `release_velocity` is
 * Note-Off's third byte, an MPE per-note dimension. Only
 * services/expression.c's end_held_note() sends a real value, from how
 * fast the pad was rising before the finger lifted (an unmeasured first
 * curve). Everything else sends 0 ("no release velocity"). */
void tiles_midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void tiles_midi_note_off(uint8_t channel, uint8_t note, uint8_t release_velocity);

/* Channel Pressure (0xD0|ch, value): MPE's Z dimension. Not Poly Key
 * Pressure (0xA0), which MPE receivers ignore; under MPE a channel is a
 * note. Two data bytes, no note number. */
void tiles_midi_send_channel_pressure(uint8_t channel, uint8_t pressure);

/* Control Change (0xB0|ch, controller, value) on one channel, USB and DIN. */
void tiles_midi_send_cc(uint8_t channel, uint8_t controller, uint8_t value);

/* Same, on the USB DAW port ONLY: CCs for the DAW remote script (transport
 * and Scene Launch, services/op_mode.c), kept off DIN and MAIN so no
 * synth or instrument track reacts to them. */
void tiles_midi_send_daw_cc(uint8_t channel, uint8_t controller, uint8_t value);

/* Same CC on channels 1-16. ONLY for tiles_midi_send_panic(), where
 * reaching every channel regardless of setup is the point (an MPE
 * receiver ignores the member copies). Not for pedals: services/pedal.c
 * uses the Master Channel only (see its send_pedal_cc()). */
void tiles_midi_send_cc_broadcast(uint8_t controller, uint8_t value);

/* MIDI panic: Sustain off (CC 64), All Notes Off (CC 123), All Sound Off
 * (CC 120) on every channel. Sustain first, because All Notes Off alone
 * leaves pedal-held notes ringing on many synths. Called by
 * services/standby.c's manual forced-sleep gesture only, not the idle
 * timeout. Doesn't touch internal note tracking (see the definition). */
void tiles_midi_send_panic(void);

/* Pitch Bend Change (0xE0|ch, LSB, MSB). bend_14bit is the unsigned wire
 * value (0-16383, 8192 = center); callers convert from signed. */
void tiles_midi_send_pitch_bend(uint8_t channel, uint16_t bend_14bit);

/* System Realtime Start (0xFA) / Stop (0xFC). op_mode.c's diamond
 * transport sends them alongside its DAW CCs (the primary path), for hosts
 * synced to TILES. Start always begins from the top; nothing here ever
 * sends Continue. */
void tiles_midi_send_start(void);
void tiles_midi_send_stop(void);
/* (Also sent on DIN, for slaved hardware. op_mode.c skips them while an
 * external clock is driving TILES.) */

/* Wraps `data` in 0xF0/0xF7 and sends it on a USB `port`
 * (TILES_MIDI_PORT_DIN is a no-op: no DIN SysEx path). Used by Scene
 * Launch mode and identity replies. Sized for this firmware's short
 * messages, not bulk SysEx. Message catalog: shared/protocol/README.md. */
void tiles_midi_send_sysex(tiles_midi_port_t port, const uint8_t *data, uint32_t len);
