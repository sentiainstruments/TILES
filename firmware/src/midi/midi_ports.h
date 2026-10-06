#pragma once

/* The MIDI ports TILES talks on. Like other controllers that drive a DAW
 * (Launchkey, Push, KeyLab), the DAW's control surface script gets its OWN
 * USB-MIDI port, so its messages never mix with what the instrument plays.
 * (When both shared one port on channel 1, Scene Launch/transport CCs could
 * reach an instrument track, and the script once took over the sustain
 * pedal; see daw-integration/README.md.)
 *
 * One USB-MIDI interface, two cables (hosts show each as its own port;
 * midi/usb_descriptors.c):
 *   MAIN (cable 0, "MIDI") -- the instrument: notes, MPE, pedals, clock and
 *                             transport Start/Stop, Identity.
 *   DAW  (cable 1, "DAW")  -- the control surface script only: Scene Launch
 *                             and transport CCs out; clip/scene SysEx and
 *                             the melodic echo's notes (TILES DISPLAY) in.
 *                             Never mirrored to DIN.
 *   DIN                    -- the jack; carries what MAIN carries. */

typedef enum {
    TILES_MIDI_PORT_MAIN = 0,
    TILES_MIDI_PORT_DAW = 1,
    TILES_MIDI_PORT_DIN = 2,
} tiles_midi_port_t;

/* USB-MIDI cable numbers, in descriptor order (midi/usb_descriptors.c). */
#define TILES_USB_MIDI_CABLE_MAIN 0u
#define TILES_USB_MIDI_CABLE_DAW 1u
#define TILES_USB_MIDI_NUM_CABLES 2u
