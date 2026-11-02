#pragma once

/* MIDI Identity Request/Reply (device inquiry):
 *   Request: F0 7E <id> 06 01 F7
 *   Reply:   F0 7E 7F 06 02 <mfr> <family> <member> <version> F7
 * Any device id is answered; the reply goes back on the request's USB port
 * (never DIN, which has no SysEx). */

void tiles_midi_identity_init(void);
