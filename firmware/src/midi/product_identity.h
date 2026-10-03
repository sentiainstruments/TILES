#pragma once

/* Everything that identifies SENTIA TILES to a host: USB VID/PID, firmware
 * version and the MIDI SysEx manufacturer ID. Both IDs are the standard
 * placeholders for an unregistered pre-production product; real ones are
 * a change to this file plus the host-side copies listed below.
 *
 * ---- USB VID/PID ----------------------------------------------------------
 * pid.codes' shared TEST ID 0x1209:0x0001, "reserved for use in private
 * testing ... MUST NOT be used on any device that will be redistributed,
 * sold, or manufactured" (pid.codes/1209/0001). It replaced 0x2E8A:0x100A,
 * a self-picked PID under Raspberry Pi's VID that is actually Pimoroni's
 * Plasma 2040 (github.com/raspberrypi/usb-pid).
 *
 * BEFORE ANY UNIT LEAVES THE BUILDING (beta testers included) this must
 * become a real ID. Easiest: Raspberry Pi gives free PIDs under VID 0x2E8A
 * to commercial RP2350 products (form linked from
 * github.com/raspberrypi/usb-pid); then set TILES_USB_VID back to 0x2E8A.
 * (Alternatives: a USB-IF VID; pid.codes' free PIDs are open hardware only.)
 *
 * BOOTSEL (ROM bootloader) always enumerates as Raspberry Pi 0x2E8A:0x000F.
 *
 * ---- Firmware version -----------------------------------------------------
 * Reported as USB bcdDevice (TILES_USB_BCD_DEVICE) and in the MIDI Identity
 * Reply (midi/identity.c). Bump for every build that leaves a developer's
 * desk; each part must stay 0-9 for the BCD encoding.
 *
 * ---- MIDI SysEx manufacturer ID -------------------------------------------
 * 0x7D: the MIDI Association's ID for non-commercial/development use, the
 * correct placeholder until TILES has its own. Used by every TILES SysEx
 * message (Identity Reply, Scene Launch). A registered ID is 3 bytes
 * (0x00 xx yy), which changes the header length, so the Scene Launch
 * parser (services/op_mode.c scene_on_sysex()) and the Ableton script
 * must change with it.
 *
 * Host-side copies that must match this file:
 *   tools/tiles_control.py                        (VID/PID)
 *   daw-integration/ableton/TILES/__init__.py     (VID/PID, auto-detection)
 *   daw-integration/ableton/TILES/scene_launch.py (SysEx manufacturer ID)
 *   shared/protocol/README.md                     (documentation) */

#define TILES_USB_VID 0x1209u
#define TILES_USB_PID 0x0001u

#define TILES_FW_VERSION_MAJOR 0u
#define TILES_FW_VERSION_MINOR 2u
#define TILES_FW_VERSION_PATCH 4u

/* 0xJJMN, binary-coded decimal: major.minor.patch. */
#define TILES_USB_BCD_DEVICE \
    ((uint16_t)((TILES_FW_VERSION_MAJOR << 8) | (TILES_FW_VERSION_MINOR << 4) | TILES_FW_VERSION_PATCH))

#define TILES_SYSEX_MANUFACTURER_ID 0x7Du
