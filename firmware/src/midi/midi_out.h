#pragma once

/* MIDI output, USB and DIN, wire layer only (channel allocation lives in
 * services/expression.c; the channel map in services/midi_channels.h).
 * Everything goes to the USB MAIN port and DIN, except daw_cc (USB DAW port
 * only) and sysex (one USB port). MPE Lower Zone: channel 1 is the Master
 * (RPNs, pedals; every note with MPE off), 2-9 Members, one per held note. */

#include <stdbool.h>
#include <stdint.h>

#include "midi_ports.h"

/* The PARTS USB port: channels 10-16, and anything between parts_begin and
 * parts_end (nestable). False (midi.parts_port = main) keeps it all on
 * MAIN. DIN gets everything either way. */
#define TILES_MIDI_FIRST_PART_CHANNEL 9u /* status nibble: MIDI channel 10 */
void tiles_midi_out_set_parts_separate(bool separate);
bool tiles_midi_out_get_parts_separate(void);
void tiles_midi_out_parts_begin(void);
void tiles_midi_out_parts_end(void);

/* Messages between begin and end (nestable) leave USB in one transfer: wrap
 * anything that sounds at once. Nothing goes out until end. */
void tiles_midi_out_batch_begin(void);
void tiles_midi_out_batch_end(void);

#define TILES_MIDI_MPE_MASTER_CHANNEL 0u       /* status nibble; 0 = MIDI channel 1 */
#define TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL 1u /* status nibble; 1 = MIDI channel 2 */
/* The zone is 0-8 channels (10-16 are fixed, 2-9 shared with Song);
 * tiles_midi_mpe_init() takes the size. */

/* Bend range per Member Channel (RPN 0). 12, not MPE's 48 (48 made tilts
 * swoop octaves); expression.c also scales the wire value for receivers
 * that ignore RPN 0. Change both together. */
#define TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES 12u

/* Zone setup: RPN 6 (member count) on the Master, then RPN 0 on the Master
 * and every Member (for receivers that don't apply it zone-wide). At boot
 * after settings load, and on every USB mount. */
/* `member_channel_count` = the zone's size (0-8); RPN 0 only inside it.
 * 0 withdraws the zone (MPE off). RPN 0 always follows a non-zero RPN 6
 * (JUCE receivers reset to 48 on it). Only expression.c calls this. */
void tiles_midi_mpe_init(uint8_t member_channel_count);

/* MPE per-note setup, right before a Note-On: centre bend and zero
 * pressure on `channel`, skipping what's already there. Not at Note-Off:
 * the tail rings as played. */
void tiles_midi_send_note_setup(uint8_t channel);

/* Note on/off on a channel. `release_velocity`: only expression.c's
 * end_held_note() sends a real one (lift speed); else 0. */
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

/* Same CC on channels 1-16: only for panic (pedals use the Master only). */
void tiles_midi_send_cc_broadcast(uint8_t controller, uint8_t value);

/* Panic: Sustain off, All Notes Off, All Sound Off on every channel, sustain
 * first (else pedal-held notes ring). The manual sleep gesture only. */
void tiles_midi_send_panic(void);

/* Pitch Bend Change (0xE0|ch, LSB, MSB). bend_14bit is the unsigned wire
 * value (0-16383, 8192 = center); callers convert from signed. */
void tiles_midi_send_pitch_bend(uint8_t channel, uint16_t bend_14bit);

/* Realtime Start / Stop, sent with the diamond's DAW CCs for hosts synced
 * to TILES. Never Continue. */
void tiles_midi_send_start(void);
void tiles_midi_send_stop(void);
/* (Also sent on DIN, for slaved hardware. op_mode.c skips them while an
 * external clock is driving TILES.) */

/* Wraps `data` in 0xF0/0xF7 on a USB `port` (no DIN SysEx). Short messages
 * only. Catalog: shared/protocol/README.md. */
void tiles_midi_send_sysex(tiles_midi_port_t port, const uint8_t *data, uint32_t len);
