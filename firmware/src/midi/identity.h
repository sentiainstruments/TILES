#pragma once

/* MIDI Identity Request/Reply: the Universal Non-Realtime SysEx "device
 * inquiry" that MIDI utilities and some DAWs use to ask what a device is.
 *
 *   Request: F0 7E <device id> 06 01 F7
 *   Reply:   F0 7E 7F 06 02 <mfr id> <family LSB MSB> <member LSB MSB>
 *            <version, 4 bytes> F7
 *
 * 7E = Universal Non-Realtime; 06/01/02 = General Information / Identity
 * Request / Identity Reply. A request with any device id is answered
 * (TILES has no device addressing), always with 0x7F ("unaddressed").
 *
 * A separate SysEx family from Scene Launch's (manufacturer 0x7D); both
 * listen on the same midi_in.h SysEx callback and ignore frames that
 * aren't theirs.
 *
 * Replies go back on the USB port the request came from. Never on DIN,
 * which has no SysEx path, so a DIN-only setup gets no reply (accepted). */

void tiles_midi_identity_init(void);
