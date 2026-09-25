# usb_vendor/

A dedicated TinyUSB vendor-class interface (separate from both the CDC
diagnostics console and the USB-MIDI interface). Eventually the
companion app's own channel for remapping pads, editing profiles,
running guided calibration, pulling live sensor/diagnostic streams, and
pushing firmware/config updates -- see `../../../docs/protocol/README.md`
for that full design.

**Built so far** (real feedback: "keep cv gate implemented but off rn.
we need the control software"): `usb_vendor.c` implements a first,
deliberately simple settings protocol -- plain-text `GET`/`SET`/`LIST`
lines, one command per line -- covering the runtime toggles this
codebase already built with a companion-app hook in mind (pedal mode/
polarity, MPE enabled, pitch-bend/aftertouch sensitivity, CV/gate enable
+ calibration). Full spec and key catalog: `../../../shared/protocol/
README.md`. Proven end-to-end with `../../../tools/tiles_control.py`, a
plain script, not yet the real Electron companion app.

**Now on the settings table** (`../profiles/`): the strcmp() chain is gone --
GET/SET/LIST/RESET/SCHEMA are generic over the registry, so a new setting needs
no change here, and changes are saved to flash (`SAVE` forces it, `INFO` shows
the store's state). Replies go through a 4 KB output queue drained into the
64-byte TinyUSB FIFO as it has room, one command at a time: the old code wrote
each line straight into that FIFO with nothing draining it between lines, so any
response longer than one packet could silently lose its tail.

Kept off the MIDI interface on purpose: calibration/live-monitor streaming
(24 pads × XYZ at ~120Hz, not built yet either) is high-bandwidth and
bursty in a way that would otherwise compete with note/expression MIDI
traffic.
