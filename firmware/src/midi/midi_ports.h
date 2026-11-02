#pragma once

/* One USB-MIDI interface, three cables, so the DAW script never mixes with
 * what the instrument plays (sharing once let the script take the pedal):
 *   MAIN  (0, "MIDI")   the live instrument: MPE zone, pedals, clock,
 *                       Start/Stop, Identity
 *   DAW   (1, "DAW")    the control surface script only; never on DIN
 *   PARTS (2, "Parts")  fixed parts and self-running ones (channels 10-16,
 *                       Song, Volca), so an MPE track on MAIN hears only
 *                       the zone. Output only; midi.parts_port = main merges
 *   DIN                 carries what MAIN and PARTS carry */

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
