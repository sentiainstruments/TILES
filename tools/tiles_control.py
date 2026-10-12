#!/usr/bin/env python3
"""Host-side (Mac/Linux), not firmware -- a plain script that talks to
SENTIA TILES' USB vendor control interface (see
shared/protocol/README.md for the wire format this implements, and
firmware/src/usb_vendor/usb_vendor.c for the device side). This is the
"prove it end-to-end" tool for that protocol's first version, not the
real Electron companion app (companion-app/ -- not built yet).

Real feedback: "keep cv gate implemented but off rn. we need the
control software."

Setup (see tools/README.md):
    brew install libusb                      # macOS libusb backend (picotool usually pulls it in)
    python3 -m venv ~/.venvs/tiles-tools     # Homebrew Python refuses a system-wide pip install
    ~/.venvs/tiles-tools/bin/pip install pyusb
    # then run this script as ~/.venvs/tiles-tools/bin/python tools/tiles_control.py ...
    # First run may prompt for the OS's own USB-device access
    # permission for this device/interface -- allow it once.

Usage:
    python3 tools/tiles_control.py list                  # every setting's current value
    python3 tools/tiles_control.py schema                # id / type / range / default of every setting
    python3 tools/tiles_control.py get color.root
    python3 tools/tiles_control.py set color.third 00FF66           # colour scheme: RRGGBB, or none
    python3 tools/tiles_control.py set pedal.mode expression
    python3 tools/tiles_control.py reset color.root                 # one setting back to its default
    python3 tools/tiles_control.py reset ALL
    python3 tools/tiles_control.py save                  # write unsaved changes to flash now
    python3 tools/tiles_control.py info                  # flash-store status
    python3 tools/tiles_control.py reboot bootsel         # reboot into the ROM bootloader (for picotool)
    python3 tools/tiles_control.py reboot app             # plain warm restart back into this firmware
    TILES_SERIAL=F1A60E66E44C9D4B python3 tools/tiles_control.py info   # one of several boards (serial = chip ID)
    python3 tools/tiles_control.py test leds 50          # bench: every LED white at 50% (current tests)
    python3 tools/tiles_control.py test motors 4 100     # bench: pads 1-4's motors at 100% for 8 s
    python3 tools/tiles_control.py test mode drums       # bench: switch mode (loop.* timing per mode)
    python3 tools/tiles_control.py test off              # bench: back to normal
    python3 tools/tiles_control.py scales                # custom scales on the device (picker pads 16-24)
    python3 tools/tiles_control.py scale get 1
    python3 tools/tiles_control.py scale put 1 Hirajoshi 0,2,3,7,8 pack=japan item=hirajoshi version=1
    python3 tools/tiles_control.py scale delete 1
    python3 tools/tiles_control.py content list          # everything in the content store
    python3 tools/tiles_control.py content clear         # wipe it (all custom scales)

Changes apply immediately and are saved to flash automatically a couple of
seconds after the last one (only while no pad is being touched); `save` just
skips the wait. Settings survive reboots and reflashing. Scale and content
changes are saved before the device answers OK.
"""

import os
import sys

try:
    import usb.core
    import usb.util
except ImportError:
    print("pyusb is required: pip install pyusb (and `brew install libusb` on macOS)", file=sys.stderr)
    sys.exit(1)

# Must match firmware/src/midi/product_identity.h's TILES_USB_VID/_PID -- the
# pid.codes test ID while TILES is pre-production (see that file). Firmware
# from before 2026-09-29 used 0x2E8A:0x100A, which turned out to be another
# product's ID; it's still recognized so an older board can be updated.
USB_IDS = [(0x1209, 0x0001), (0x2E8A, 0x100A)]

# Must match firmware/src/midi/usb_descriptors.c's own vendor interface
# string ("SENTIA TILES Control") -- used to pick the right interface out
# of the device's composite CDC+MIDI+Vendor descriptor set, and it's what
# actually identifies a TILES board (the IDs above only narrow the search),
# so a future change of USB ID doesn't strand this tool.
VENDOR_INTERFACE_STRING = "SENTIA TILES Control"

TIMEOUT_MS = 2000


def serial_of(device):
    try:
        return usb.util.get_string(device, device.iSerialNumber)
    except (usb.core.USBError, ValueError):
        return None


def find_device():
    # TILES_SERIAL (the USB serial = chip ID, as for tools/flash.sh) picks
    # one board when several are connected; otherwise the first one found.
    want = os.environ.get("TILES_SERIAL", "").strip().upper()
    for vid, pid in USB_IDS:
        for device in usb.core.find(find_all=True, idVendor=vid, idProduct=pid):
            if not want or (serial_of(device) or "").upper() == want:
                return device
    ids = ", ".join(f"{vid:04X}:{pid:04X}" for vid, pid in USB_IDS)
    which = f" with serial {want}" if want else ""
    print(f"No SENTIA TILES device found{which} (looked for USB IDs {ids}). Is it plugged in?", file=sys.stderr)
    sys.exit(1)


def find_vendor_endpoints():
    device = find_device()

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

    def drain(self):
        """Discards whatever an earlier, interrupted run left unread in the device's reply buffer. Found the
        hard way: a SCHEMA piped through `grep` right after a reboot left ~2 KB queued, the next command read
        that stale text as its own reply, and the one after that timed out on write (the device won't take
        a new command while its reply buffer is full)."""
        while True:
            try:
                self.ep_in.read(64, timeout=50)
            except usb.core.USBError:
                return

    def command(self, line, multi_line=False):
        """Sends one command. multi_line: LIST/SCHEMA/INFO/SCALES/SCALE GET/CONTENT LIST send lines then a
        final OK; everything else is answered by exactly one line (a value, OK, or ERR ...)."""
        self.drain()
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
    elif command == "reboot" and argc == 3 and sys.argv[2].lower() in ("bootsel", "app"):
        request, multi = f"REBOOT {sys.argv[2].upper()}", False
    elif command == "test" and argc >= 3:
        # Bench tests for current measurement: test leds 37 | test motors 4 [duty%] | test off
        request, multi = "TEST " + " ".join(a.upper() for a in sys.argv[2:]), sys.argv[2].lower() == "motors"
    elif command == "scales" and argc == 2:
        request, multi = "SCALES", True
    elif command == "scale" and argc >= 4 and sys.argv[2].lower() in ("get", "put", "delete"):
        # Names, packs and items keep their case; only the verb is upper-cased.
        request = f"SCALE {sys.argv[2].upper()} " + " ".join(sys.argv[3:])
        multi = sys.argv[2].lower() == "get"
    elif command == "content" and argc == 3 and sys.argv[2].lower() in ("list", "clear"):
        request, multi = f"CONTENT {sys.argv[2].upper()}", sys.argv[2].lower() == "list"
    else:
        print(__doc__)
        sys.exit(1)

    if request == "REBOOT BOOTSEL":
        # The device does its best to reply OK, but reset_usb_boot() can make it
        # vanish from USB before that reply arrives -- a read timeout/USB error
        # here means it worked, not that it failed. See shared/protocol/README.md's
        # own "REBOOT" section.
        try:
            for line in session.command(request, multi_line=multi):
                print(line)
        except SystemExit:
            print("(no reply -- the device likely rebooted into BOOTSEL before it could send one, which is expected)")
        return

    for line in session.command(request, multi_line=multi):
        print(line)


if __name__ == "__main__":
    main()
