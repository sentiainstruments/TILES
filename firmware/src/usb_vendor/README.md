# usb_vendor/

A dedicated TinyUSB vendor-class interface (separate from the USB-MIDI
interface) that the companion app uses to remap pads, edit profiles, run
guided calibration, pull live sensor/diagnostic streams, and push
firmware/config updates. Implements the wire protocol defined once in
`../../../shared/protocol/` — no protocol framing gets redefined here or
in the companion app independently.

Kept off the MIDI interface on purpose: calibration/live-monitor streaming
(24 pads × XYZ at ~120Hz) is high-bandwidth and bursty in a way that would
otherwise compete with note/expression MIDI traffic.
