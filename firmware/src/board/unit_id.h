#pragma once

/* Human-assigned pre-production unit number ("2 of 4"), set by hand before
 * building and flashing each board (plain header, a normal incremental
 * build picks it up). Not the RP2350 chip ID, which is already the USB
 * serial number but can't be read off a box.
 *
 * Shown in the USB product name ("SENTIA TILES N", which names the MIDI
 * ports, so units played together stay apart in a DAW; see
 * midi/usb_descriptors.c), the CDC interface name ("SENTIA TILES
 * Diagnostics (Unit N/M)"), the vendor INFO reply (`unit=`) and a boot
 * printf (main.c). */
#define TILES_UNIT_NUMBER 2u
#define TILES_UNIT_COUNT 4u
