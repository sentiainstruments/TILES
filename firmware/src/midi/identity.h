#pragma once

/*
 * MIDI Identity Request/Reply -- the Universal Non-Realtime System
 * Exclusive "device inquiry" handshake every generic MIDI utility, and
 * some DAWs' own auto-detection, uses to ask a device what it is.
 *
 * Real feedback: "before bnooting look into what actually is standardized
 * or good practice in this industry that we havent implemented yet" ->
 * "3. make sure we are ok and fix whaterver needs" (researching this).
 *
 * Wire format (MIDI Association, "Universal System Exclusive Messages" --
 * confirmed against http://midi.teragonaudio.com/tech/midispec/identity.htm
 * and the MIDI.org community's own restatement of it):
 *
 *   Request: F0 7E <device id> 06 01 F7
 *   Reply:   F0 7E <device id> 06 02 <mfr id> <family LSB MSB>
 *            <member LSB MSB> <version 4 bytes> F7
 *
 * `7E` is the Universal Non-Realtime ID; `06`/`01`/`02` are the General
 * Information / Identity Request / Identity Reply sub-IDs. `<device id>`
 * is conventionally 0x7F ("disregard channel") from a requester that
 * isn't targeting one specific device among several on a bus -- this file
 * replies to a request carrying ANY device id byte (TILES has no real
 * per-device addressing concept of its own to filter on) and always
 * answers with 0x7F itself, the same "unaddressed" convention.
 *
 * This is a SEPARATE, standard SysEx family from Scene Launch's own
 * custom protocol (daw-integration/, manufacturer byte 0x7D as the first
 * byte after F0, no relation to the Universal Non-Realtime ID 0x7E here)
 * -- the two coexist on the same shared services/midi_in.h sysex-callback
 * mechanism exactly like any other two listeners would, each ignoring
 * frames that aren't theirs.
 *
 * Answered on the USB port the request came in on (MAIN or DAW -- see
 * midi/midi_ports.h); never on DIN, like tiles_midi_send_sysex() itself
 * (midi/midi_out.h) -- DIN
 * MIDI OUT has no SysEx support at all yet (din_midi_queue.c rejects any
 * status byte >= 0xF0 outside the Real-Time range by design; see that
 * file's own header). A device connected to TILES only via the DIN jack,
 * with no USB host also present, won't receive a reply -- accepted for
 * now rather than building DIN SysEx support for this one feature; Scene
 * Launch is the only other thing that would want it, and that's
 * permanently USB/DAW-only by design anyway.
 */

void tiles_midi_identity_init(void);
