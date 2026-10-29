#pragma once

/* The MIDI ports TILES talks on. Like other controllers that drive a DAW
 * (Launchkey, Push, KeyLab), the DAW's control surface script gets its OWN
 * USB-MIDI port, so its messages never mix with what the instrument plays.
 * (When both shared one port on channel 1, Scene Launch/transport CCs could
 * reach an instrument track, and the script once took over the sustain
 * pedal; see daw-integration/README.md.)
 *
 * One USB-MIDI interface, three cables (hosts show each as its own port;
 * midi/usb_descriptors.c):
 *   MAIN  (cable 0, "MIDI")  -- the instrument played live: the MPE zone
 *                               (channel 1 and its member channels), pedals,
 *                               clock and transport Start/Stop, Identity.
 *   DAW   (cable 1, "DAW")   -- the control surface script only: Scene
 *                               Launch and transport CCs out; clip/scene
 *                               SysEx and the melodic echo's notes (TILES
 *                               DISPLAY) in. Never mirrored to DIN.
 *   PARTS (cable 2, "Parts") -- everything on a fixed part channel or
 *                               running by itself: sequencer lanes (13-16),
 *                               chords (12: Duet's strip, Prism), Bouncing
 *                               notes and games (11), drums (10), and Song
 *                               tracks and Volca, which use 1-9 as well. So
 *                               a DAW's MPE track on MAIN, listening to all
 *                               channels, hears only the MPE zone. Output
 *                               only (input on it is ignored). Setting
 *                               midi.parts_port = main puts it all back on
 *                               MAIN (midi_out.h).
 *   DIN                      -- the jack; carries what MAIN and PARTS
 *                               carry. */

typedef enum {
    TILES_MIDI_PORT_MAIN = 0,
    TILES_MIDI_PORT_DAW = 1,
    TILES_MIDI_PORT_DIN = 2,
} tiles_midi_port_t;

/* USB-MIDI cable numbers, in descriptor order (midi/usb_descriptors.c). */
#define TILES_USB_MIDI_CABLE_MAIN 0u
#define TILES_USB_MIDI_CABLE_DAW 1u
#define TILES_USB_MIDI_CABLE_PARTS 2u
#define TILES_USB_MIDI_NUM_CABLES 3u
