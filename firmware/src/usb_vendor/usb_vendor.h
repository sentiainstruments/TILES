#pragma once

/* USB vendor interface: the settings channel for the companion app and
 * scripts (tools/tiles_control.py). Plain text, one command per line,
 * '\n'-terminated, so it can be typed by hand or driven by a short script:
 *
 *   GET <key>            -> "<value>" or "ERR unknown-key"
 *   SET <key> <value>    -> "OK" or "ERR <reason>"
 *   LIST                 -> "<key>=<value>" per setting, then "OK"
 *   SCHEMA, INFO, SAVE, RESET <key>|ALL, REBOOT BOOTSEL|APP
 *
 * Commands are generic over the settings table (profiles/settings.h). Full
 * spec and key list: shared/protocol/README.md. The larger protocol in
 * docs/protocol/README.md (pad remap, guided calibration, live sensor
 * streaming, profiles, firmware update) is not built. */

#include <stdint.h>

/* Resets the command/reply buffers. TinyUSB owns the endpoints after
 * tud_init() (midi/usb_device.c). Safe to call at boot. */
void tiles_usb_vendor_init(void);

/* Reads bytes from the vendor OUT endpoint, runs each complete line and
 * queues the reply for the IN endpoint. Call every main-loop pass; cheap
 * when idle. */
void tiles_usb_vendor_scan(void);
