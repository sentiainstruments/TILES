# midi/

Musical output only — never carries config/calibration traffic (that's
`usb_vendor/`).

Planned contents: TinyUSB USB-MIDI class + MPE channel allocation
(dynamic, 15 lower-zone member channels across 24 pads, deterministic
voice-steal policy), DIN MIDI IN (GP1 UART, 31,250 baud), DIN MIDI OUT
(polarity-selectable via GP0/GP2, PIO or software UART), and DIN-specific
rate limiting for continuous expression data.
