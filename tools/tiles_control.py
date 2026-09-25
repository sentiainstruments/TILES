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
    python3 tools/tiles_control.py list                  # every setting's current value
    python3 tools/tiles_control.py schema                # id / type / range / default of every setting
    python3 tools/tiles_control.py get look.natural_pad_percent
    python3 tools/tiles_control.py set look.natural_pad_percent 30
    python3 tools/tiles_control.py set pedal.mode expression
    python3 tools/tiles_control.py reset look.natural_pad_percent   # one setting back to its default
    python3 tools/tiles_control.py reset ALL
    python3 tools/tiles_control.py save                  # write unsaved changes to flash now
    python3 tools/tiles_control.py info                  # flash-store status

Changes apply immediately and are saved to flash automatically a couple of
seconds after the last one (only while no pad is being touched); `save` just
skips the wait. Settings survive reboots and reflashing.
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


class Session:
    """A byte stream over the vendor endpoints, split into lines. The device streams a response as
    fast as the USB FIFO drains, so a line can straddle two 64-byte packets -- read lines, not packets."""

    def __init__(self, ep_out, ep_in):
        self.ep_out = ep_out
        self.ep_in = ep_in
        self.buf = b""

    def read_line(self, what):
        while b"\n" not in self.buf:
            try:
                self.buf += bytes(self.ep_in.read(64, timeout=TIMEOUT_MS))
            except usb.core.USBError as exc:
                print(f"USB read timed out/failed waiting for a response to {what!r}: {exc}", file=sys.stderr)
                sys.exit(1)
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("ascii", errors="replace").strip("\r")

    def command(self, line, multi_line=False):
        """Sends one command. multi_line: LIST/SCHEMA/INFO send many lines then a final OK; everything else
        is answered by exactly one line (a value, OK, or ERR ...)."""
        self.buf = b""
        self.ep_out.write((line + "\n").encode("ascii"), timeout=TIMEOUT_MS)
        lines = []
        while True:
            text = self.read_line(line)
            if multi_line and text == "OK":
                return lines
            lines.append(text)
            if not multi_line or text.startswith("ERR "):
                return lines


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    session = Session(*find_vendor_endpoints())
    command = sys.argv[1].lower()
    argc = len(sys.argv)

    if command in ("list", "schema", "info") and argc == 2:
        request, multi = command.upper(), True
    elif command == "save" and argc == 2:
        request, multi = "SAVE", False
    elif command == "get" and argc == 3:
        request, multi = f"GET {sys.argv[2]}", False
    elif command == "set" and argc == 4:
        request, multi = f"SET {sys.argv[2]} {sys.argv[3]}", False
    elif command == "reset" and argc == 3:
        request, multi = f"RESET {sys.argv[2]}", False
    else:
        print(__doc__)
        sys.exit(1)

    for line in session.command(request, multi_line=multi):
        print(line)


if __name__ == "__main__":
    main()
