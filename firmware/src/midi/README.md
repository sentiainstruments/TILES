# midi/

Musical output only — never carries config/calibration traffic (that's
`usb_vendor/`).

Planned contents: TinyUSB USB-MIDI class + MPE channel allocation
(dynamic, 15 lower-zone member channels across 24 pads, deterministic
voice-steal policy), DIN MIDI IN (GP1 UART, 31,250 baud), DIN MIDI OUT
(polarity-selectable via GP0/GP2, PIO or software UART), and DIN-specific
rate limiting for continuous expression data.

## Status

- `tusb_config.h`, `usb_descriptors.c`, `usb_device.{h,c}` — done. A
  composite CDC (diagnostics console) + MIDI USB device, modeled on
  pico-sdk's own `pico_stdio_usb` reference config and TinyUSB's
  `cdc_msc`/`midi_test` example descriptor patterns rather than
  hand-built from scratch. `firmware/src/CMakeLists.txt` links
  `tinyusb_device` explicitly, which makes `pico_stdio_usb` defer both
  `tusb_init()` and USB descriptor provision to us
  (`LIB_TINYUSB_DEVICE`-gated, pico-sdk's own documented mechanism for
  this) -- see the comment in `tusb_config.h` for the full reasoning.
- `midi_out.{h,c}` — done: `tiles_midi_note_on(note, velocity)`,
  `tiles_midi_note_off(note)`, `tiles_midi_send_poly_aftertouch(note, pressure)`,
  `tiles_midi_send_cc(controller, value)` -- all on a single V1 MIDI
  channel (channel 1). Driven by `services/expression.c` (note
  on/off/aftertouch, from touch+Hall fusion) and `services/pedal.c`
  (sustain/expression CC). No MPE (per-note channel allocation) yet --
  see `docs/architecture/defaults-and-safeguards.md`.
- DIN MIDI IN/OUT, MPE channel allocation -- not built yet.
