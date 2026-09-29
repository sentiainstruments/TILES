#pragma once

/*
 * Everything that identifies SENTIA TILES to a host, in one place: the USB
 * vendor/product ID, the firmware version, and the MIDI System Exclusive
 * manufacturer ID. Real feedback: "6-7 lets do whatever makes sense rn for
 * a non registered product thats in pre production." Both IDs below are the
 * standard placeholders for exactly that stage, chosen so that getting real
 * ones later is a change to this file (plus the host-side copies listed at
 * the bottom), not a hunt through the codebase.
 *
 * ---- USB VID/PID ----------------------------------------------------------
 * pid.codes' shared TEST ID, 0x1209:0x0001: "reserved for use in private
 * testing. Anyone may assign it to their device while they're testing
 * in-house, but it MUST NOT be used on any device that will be
 * redistributed, sold, or manufactured" (pid.codes/1209/0001). That is
 * exactly what the two pre-production boards are. It replaces 0x2E8A:0x100A
 * -- Raspberry Pi's vendor ID with a product ID this project picked itself,
 * which turned out to be ALREADY ALLOCATED to Pimoroni's Plasma 2040
 * (github.com/raspberrypi/usb-pid). A host that keys drivers or tools on
 * VID/PID (Windows does both) would have treated TILES as that board.
 *
 * BEFORE ANY UNIT LEAVES THE BUILDING (beta testers included) this must
 * become a real, allocated ID. The natural one: Raspberry Pi gives free
 * product IDs under its own VID 0x2E8A to commercial products built on its
 * chips (RP2350 included) -- apply via the form linked from
 * github.com/raspberrypi/usb-pid, then put that PID here with
 * TILES_USB_VID back at 0x2E8A. (Buying a USB-IF vendor ID is the other
 * route; pid.codes' free non-test PIDs are for open-source hardware only.)
 *
 * The ROM bootloader (BOOTSEL) keeps Raspberry Pi's own 0x2E8A:0x000F no
 * matter what is set here.
 *
 * ---- Firmware version -----------------------------------------------------
 * Reported to hosts as USB bcdDevice (TILES_USB_BCD_DEVICE) and in the MIDI
 * Identity Reply (midi/identity.c). Bump it for every build that leaves a
 * developer's desk; each part must stay 0-9 for the BCD encoding.
 *
 * ---- MIDI SysEx manufacturer ID -------------------------------------------
 * 0x7D is the MIDI Association's ID reserved for non-commercial/educational
 * and development use -- the correct placeholder for an unregistered
 * product, not a borrowed real ID. Every TILES SysEx message (the Identity
 * Reply, Scene Launch's clip/scene state) uses this constant. A product that
 * ships needs its own ID from the MIDI Association (a 3-byte 0x00 xx yy
 * one); that changes the header length from 1 byte to 3, so the Scene
 * Launch parser (services/op_mode.c's scene_on_sysex()) and the Ableton
 * script's copy must move with it.
 *
 * Host-side copies that must match this file:
 *   tools/tiles_control.py                        (VID/PID -- preferred match)
 *   daw-integration/ableton/TILES/__init__.py     (VID/PID -- auto-detection)
 *   daw-integration/ableton/TILES/scene_launch.py (SysEx manufacturer ID)
 *   shared/protocol/README.md                     (documentation)
 */

#define TILES_USB_VID 0x1209u
#define TILES_USB_PID 0x0001u

#define TILES_FW_VERSION_MAJOR 0u
#define TILES_FW_VERSION_MINOR 1u
#define TILES_FW_VERSION_PATCH 0u

/* 0xJJMN, binary-coded decimal: major.minor.patch. */
#define TILES_USB_BCD_DEVICE \
    ((uint16_t)((TILES_FW_VERSION_MAJOR << 8) | (TILES_FW_VERSION_MINOR << 4) | TILES_FW_VERSION_PATCH))

#define TILES_SYSEX_MANUFACTURER_ID 0x7Du
