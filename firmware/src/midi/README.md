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
  That same gate also disables pico_stdio_usb's automatic background-IRQ
  `tud_task()` servicing, and a real bug slipped through as a result:
  nothing anywhere in this firmware called `tud_task()`, so the USB
  stack was never actually serviced past whatever the low-level
  enumeration ISR handles on its own -- found while chasing why the
  USB-CDC debug console printed nothing at all on real hardware. Fixed:
  `main.c` now calls `tud_task()` at the top of every main-loop
  iteration; see its call site and `usb_device.h`'s updated header for
  the full explanation. This plausibly also explains why USB MIDI below
  has never been confirmed working in a DAW (queued
  `tud_midi_stream_write()` bytes need `tud_task()` to actually reach
  the host) -- not proven yet, but a real hardware test of MIDI note
  output is now worth retrying specifically because of this fix.
- `midi_out.{h,c}` — done: `tiles_midi_note_on(note, velocity)`,
  `tiles_midi_note_off(note)`, `tiles_midi_send_poly_aftertouch(note, pressure)`,
  `tiles_midi_send_cc(controller, value)`, `tiles_midi_send_pitch_bend(bend_14bit)`
  -- all on a single V1 MIDI channel (channel 1). Driven by
  `services/expression.c` (note on/off/aftertouch/pitch bend, from
  touch+Hall fusion) and `services/pedal.c` (sustain/expression CC). No
  MPE (per-note channel allocation) yet -- see
  `docs/architecture/defaults-and-safeguards.md`. Pitch bend in
  particular is a real, direct consequence of that gap: Pitch Bend
  Change is a channel-wide MIDI message with no per-note addressing in
  the spec itself, so bending one held note's pitch on this single
  channel unavoidably bends every other note currently held on it too --
  `services/expression.c`'s pitch-bend feature works around this with a
  single "owner pad" concept rather than pretending independent per-note
  bend is possible without real MPE channel allocation; see its own
  "Pitch bend from sideways motion" section for the full reasoning.
- DIN MIDI IN/OUT, MPE channel allocation -- not built yet.
