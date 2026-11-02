#pragma once

/* What identifies TILES to a host: USB VID/PID, firmware version, SysEx ID.
 *
 * USB ID 0x1209:0x0001 is pid.codes' shared TEST ID, forbidden on any unit
 * that leaves the building (beta included). Get a real PID first (free from
 * Raspberry Pi for RP2350 products, github.com/raspberrypi/usb-pid), then
 * set TILES_USB_VID back to 0x2E8A. BOOTSEL is always 0x2E8A:0x000F.
 *
 * Firmware version Alpha N.P: TILES_FW_BUILD N for big changes (P resets),
 * TILES_FW_POINT P for every other build. INFO says alpha.N.P, bcdDevice
 * 0xSNPP, the Identity Reply stage, N, P, 0.
 *
 * SysEx ID 0x7D (development use). A registered ID is 3 bytes, which
 * changes the Scene Launch header (op_mode.c and the Ableton script).
 *
 * Host copies that must match: tools/tiles_control.py,
 * daw-integration/ableton/TILES/__init__.py and scene_launch.py,
 * shared/protocol/README.md. */

#define TILES_USB_VID 0x1209u
#define TILES_USB_PID 0x0001u

#define TILES_FW_STAGE_ALPHA 0u
#define TILES_FW_STAGE_BETA 1u
#define TILES_FW_STAGE TILES_FW_STAGE_ALPHA
#define TILES_FW_STAGE_NAME "alpha" /* matches TILES_FW_STAGE */
#define TILES_FW_BUILD 6u
#define TILES_FW_POINT 6u

_Static_assert(TILES_FW_BUILD <= 9u && TILES_FW_POINT <= 99u, "bcdDevice holds N 0-9, P 0-99");

#define TILES_USB_BCD_DEVICE                                       \
    ((uint16_t)((TILES_FW_STAGE << 12) | (TILES_FW_BUILD << 8) |   \
                ((TILES_FW_POINT / 10u) << 4) | (TILES_FW_POINT % 10u)))

#define TILES_SYSEX_MANUFACTURER_ID 0x7Du
