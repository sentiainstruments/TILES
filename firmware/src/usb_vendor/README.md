# usb_vendor/

A TinyUSB vendor-class interface, separate from the CDC console and
USB-MIDI: the companion app's and scripts' channel for settings. It uses
WinUSB on Windows via the MS OS 2.0 descriptors, so no driver install is
needed.

`usb_vendor.c` speaks a plain-text line protocol (`GET`, `SET`, `LIST`,
`SCHEMA`, `INFO`, `SAVE`, `RESET`, `REBOOT BOOTSEL|APP`), generic over
the settings table (`../profiles/`). A new setting needs no change here,
and changes are saved to flash automatically. Full spec and key list:
`../../../shared/protocol/README.md`. Reference client:
`../../../tools/tiles_control.py`.

How it works:

- Replies go into a 4 KB queue drained into the 64-byte TinyUSB FIFO as
  room appears, one command at a time, so long replies (SCHEMA ~3 KB)
  arrive whole.
- `REBOOT BOOTSEL` flushes its `OK` (bounded wait) before
  `reset_usb_boot()`, which never returns. `picotool -f` uses the
  separate standard USB reset interface instead (`../midi/README.md`).
- It is kept off the MIDI interface on purpose: future live sensor
  streaming (24 pads × XYZ at ~120 Hz) would compete with note traffic.

Not built yet: the larger protocol in `../../../docs/protocol/README.md`
(pad remap, guided calibration, live streaming, profiles, firmware
update).
