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
 * ---- Firmware version: Alpha N.P -----------------------------------------
 * Pre-release builds are numbered by stage: Alpha while features still
 * change, Beta once they freeze (the count restarts), then 1.0, the
 * release. Within a stage:
 *   TILES_FW_BUILD  N: a big change only (new architecture, new features
 *                   or layouts); its point goes back to 0.
 *   TILES_FW_POINT  P: every other build that leaves a developer's desk
 *                   (fixes, tuning, small additions): Alpha 4.1, 4.2 ...
 * Reported as:
 *   INFO              firmware=alpha.4, alpha.4.1 (usb_vendor.c)
 *   USB bcdDevice     0xSNPP in BCD: stage, N (0-9), P (0-99); a host
 *                     shows Alpha 4.1 as "4.01"
 *   Identity Reply    software revision bytes: stage, N, P, 0
 *                     (midi/identity.c)
 * (Until 2026-10-23 builds were 0.2.0 .. 0.9.9; Alpha 1-4 followed.)
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

#define TILES_FW_STAGE_ALPHA 0u
#define TILES_FW_STAGE_BETA 1u
#define TILES_FW_STAGE TILES_FW_STAGE_ALPHA
#define TILES_FW_STAGE_NAME "alpha" /* matches TILES_FW_STAGE */
#define TILES_FW_BUILD 6u
#define TILES_FW_POINT 5u

_Static_assert(TILES_FW_BUILD <= 9u && TILES_FW_POINT <= 99u, "bcdDevice holds N 0-9, P 0-99");

#define TILES_USB_BCD_DEVICE                                       \
    ((uint16_t)((TILES_FW_STAGE << 12) | (TILES_FW_BUILD << 8) |   \
                ((TILES_FW_POINT / 10u) << 4) | (TILES_FW_POINT % 10u)))

#define TILES_SYSEX_MANUFACTURER_ID 0x7Du
