#!/usr/bin/env python3
"""Host-side (Mac/Linux), not firmware -- a plain script that talks to
SENTIA TILES' USB vendor control interface (see
shared/protocol/README.md for the wire format this implements, and
firmware/src/usb_vendor/usb_vendor.c for the device side). This is the
"prove it end-to-end" tool for that protocol's first version, not the
real Electron companion app (companion-app/ -- not built yet).

Real feedback: "keep cv gate implemented but off rn. we need the
control software."

Setup:
    pip install pyusb
    # macOS also needs a libusb backend: `brew install libusb`
    # First run may prompt for the OS's own USB-device access
    # permission for this device/interface -- allow it once.

Usage:
    python3 tools/tiles_control.py list
    python3 tools/tiles_control.py get cv_gate.enabled
    python3 tools/tiles_control.py set cv_gate.enabled 1
    python3 tools/tiles_control.py set pedal.mode expression
"""

import sys

try:
    import usb.core
    import usb.util
except ImportError:
    print("pyusb is required: pip install pyusb (and `brew install libusb` on macOS)", file=sys.stderr)
    sys.exit(1)

# Must match firmware/src/midi/usb_descriptors.c's own USB_VID/USB_PID.
VID = 0x2E8A
PID = 0x100A

# Must match firmware/src/midi/usb_descriptors.c's own vendor interface
# string ("SENTIA TILES Control") -- used only to pick the right
# interface out of the device's composite CDC+MIDI+Vendor descriptor
# set, not sent over the wire.
VENDOR_INTERFACE_STRING = "SENTIA TILES Control"

TIMEOUT_MS = 2000


def find_vendor_endpoints():
    device = usb.core.find(idVendor=VID, idProduct=PID)
    if device is None:
        print(f"No SENTIA TILES device found (VID=0x{VID:04X} PID=0x{PID:04X}). Is it plugged in?", file=sys.stderr)
        sys.exit(1)

    for cfg in device:
        for interface in cfg:
            try:
                name = usb.util.get_string(device, interface.iInterface)
            except (usb.core.USBError, ValueError):
                name = None
            if name != VENDOR_INTERFACE_STRING:
                continue

            # pyusb/libusb (not the OS) needs to detach the interface
            # from any kernel driver before claiming it -- harmless
            # no-op on macOS/Windows, where no kernel driver claims an
            # unrecognized vendor interface in the first place.
            try:
                if device.is_kernel_driver_active(interface.bInterfaceNumber):
                    device.detach_kernel_driver(interface.bInterfaceNumber)
            except (usb.core.USBError, NotImplementedError):
                pass

            usb.util.claim_interface(device, interface.bInterfaceNumber)

            ep_out = usb.util.find_descriptor(
                interface, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
            )
            ep_in = usb.util.find_descriptor(
                interface, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
            )
            if ep_out is None or ep_in is None:
                print("Vendor interface found but missing an endpoint -- descriptor mismatch?", file=sys.stderr)
                sys.exit(1)
            return ep_out, ep_in

    print(f'No interface named "{VENDOR_INTERFACE_STRING}" found on the device.', file=sys.stderr)
    sys.exit(1)


def send_command(ep_out, ep_in, line):
    ep_out.write((line + "\n").encode("ascii"), timeout=TIMEOUT_MS)
    responses = []
    while True:
        try:
            raw = ep_in.read(64, timeout=TIMEOUT_MS)
        except usb.core.USBError as exc:
            print(f"USB read timed out/failed waiting for a response to {line!r}: {exc}", file=sys.stderr)
            sys.exit(1)
        text = bytes(raw).decode("ascii", errors="replace").strip("\n")
        responses.append(text)
        # LIST sends one line per key, then a final OK -- everything
        # else sends exactly one line. Stop as soon as we see a
        # terminal-looking line (OK or ERR ...) for anything that isn't
        # a bare "key=value" LIST row, or once LIST's own trailing OK
        # arrives.
        if text == "OK" or text.startswith("ERR ") or "=" not in text:
            break
    return responses


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    ep_out, ep_in = find_vendor_endpoints()

    command = sys.argv[1].lower()
    if command == "list":
        for line in send_command(ep_out, ep_in, "LIST"):
            print(line)
    elif command == "get" and len(sys.argv) == 3:
        for line in send_command(ep_out, ep_in, f"GET {sys.argv[2]}"):
            print(line)
    elif command == "set" and len(sys.argv) == 4:
        for line in send_command(ep_out, ep_in, f"SET {sys.argv[2]} {sys.argv[3]}"):
            print(line)
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
