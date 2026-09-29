#pragma once

/*
 * The MIDI ports TILES talks on. Real feedback: "do 8 as how standardized
 * stuff works. production ready industry stuff" -- item 8 of the
 * standardization round being "DAW control shares the performance port and
 * channel 1." Every controller that also drives a DAW (Novation Launchkey,
 * Ableton Push, Arturia KeyLab) gives the DAW's control-surface script its
 * OWN USB-MIDI port, so the script's messages never mix with what the
 * instrument plays. TILES used to put both on its one port, on channel 1:
 * the Scene Launch/transport CCs could reach an instrument track, and the
 * script's own button bindings once took over the sustain pedal (CC64) --
 * see daw-integration/README.md.
 *
 * One USB-MIDI interface, two virtual cables (the USB-MIDI 1.0 class's own
 * mechanism for several ports on one device; hosts show each as a separate
 * port -- see midi/usb_descriptors.c):
 *   MAIN (cable 0, "MIDI") -- the instrument: notes, MPE, pedals, clock and
 *                             transport Start/Stop for sync, Identity.
 *   DAW  (cable 1, "DAW")  -- the control-surface script only: Scene Launch
 *                             and transport-button CCs out; clip/scene state
 *                             SysEx and the melodic echo's notes (TILES
 *                             DISPLAY, sent through the script) in. Never
 *                             mirrored to DIN.
 *   DIN                    -- the jack; carries what MAIN carries.
 */

typedef enum {
    TILES_MIDI_PORT_MAIN = 0,
    TILES_MIDI_PORT_DAW = 1,
    TILES_MIDI_PORT_DIN = 2,
} tiles_midi_port_t;

/* USB-MIDI cable numbers, in descriptor order (midi/usb_descriptors.c). */
#define TILES_USB_MIDI_CABLE_MAIN 0u
#define TILES_USB_MIDI_CABLE_DAW 1u
#define TILES_USB_MIDI_NUM_CABLES 2u
