#pragma once

/*
 * USB vendor interface: the settings half of "the control software"
 * (real feedback: "keep cv gate implemented but off rn. we need the
 * control software"). Scoped deliberately narrow -- this is a first,
 * intentionally simple version covering only the runtime settings this
 * session already built with a companion-app hook in mind (pedal mode/
 * polarity, MPE enabled, pitch-bend/aftertouch sensitivity, CV/gate
 * enable + both calibration structs), proven end-to-end with a plain
 * script (tools/tiles_control.py) rather than the real Electron
 * companion-app, which doesn't exist yet -- see the real feedback that
 * scoped this exact split.
 *
 * This is NOT the full protocol docs/protocol/README.md's own design
 * notes describe (pad remap, guided calibration, live 24-pad XYZ
 * streaming at ~120Hz, profile read/write, firmware update, real
 * framing/versioning/schema questions) -- those are real, still open,
 * and deliberately not addressed here. What's built instead is the
 * simplest thing that actually proves the USB vendor interface works
 * and that a settings round-trip is real: a plain-text, line-based
 * GET/SET protocol, one command per line, terminated by '\n':
 *
 *   GET <key>\n         -> "<value>\n" or "ERR unknown-key\n"
 *   SET <key> <value>\n -> "OK\n" or "ERR <reason>\n"
 *   LIST\n              -> "<key>=<value>\n" for every known key, then "OK\n"
 *
 * Text, not binary -- deliberately, for this first version: readable
 * and typeable by hand from a terminal or a five-line script, which
 * matters far more right now than wire efficiency for a handful of
 * settings changed occasionally, not a high-rate stream. See shared/
 * protocol/README.md for the full key list and value formats.
 */

#include <stdint.h>

/* Claims the vendor interface's own buffers -- no GPIO/peripheral
 * setup needed, TinyUSB itself owns the endpoints once tud_init() has
 * run (see midi/usb_device.c). Safe to call unconditionally at boot. */
void tiles_usb_vendor_init(void);

/* Drains any bytes TinyUSB has buffered for the vendor OUT endpoint,
 * assembles them into lines, and dispatches each complete line through
 * the GET/SET/LIST handler above, writing the response back on the
 * vendor IN endpoint. Call every main-loop iteration; cheap (an
 * available-byte check) when nothing is pending. */
void tiles_usb_vendor_scan(void);
