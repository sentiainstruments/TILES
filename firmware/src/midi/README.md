# midi/

Musical output only — never carries config/calibration traffic (that's
`usb_vendor/`).

Planned contents: DIN MIDI IN (GP1 UART, 31,250 baud), DIN MIDI OUT
(polarity-selectable via GP0/GP2, PIO or software UART), and DIN-specific
rate limiting for continuous expression data. USB-MIDI + MPE channel
allocation (dynamic, 15 lower-zone member channels across 24 pads,
deterministic voice-steal policy) are done — see Status below.

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
- `midi_out.{h,c}` — done: real MPE (MIDI Polyphonic Expression), a
  single Lower Zone -- channel 1 is the Zone Master Channel (carries only
  the zone-configuration RPN messages `tiles_midi_mpe_init()` sends once,
  the first main-loop iteration `tud_midi_mounted()` reads true after
  boot or a remount, never note data itself), channels 2-16 are Member
  Channels, one per currently-held note. Added after real feedback: "we
  need to make sure we have individual per note pitch bend not just
  regular all key pitch bend. like the roli seaboard." Every function
  here (`tiles_midi_note_on/off(channel, ...)`,
  `tiles_midi_send_poly_aftertouch(channel, ...)`,
  `tiles_midi_send_pitch_bend(channel, ...)`,
  `tiles_midi_send_cc(channel, ...)`) takes an explicit channel -- this
  file is only the wire-protocol layer, it doesn't decide which channel
  a note gets. `services/expression.c` owns that: its per-pad MPE channel
  allocator (`claim_mpe_channel()`/`end_held_note()`) claims a free
  Member Channel the instant a strike commits and frees it (always
  centering pitch bend first, so a reused channel never inherits a
  stale bend) at note-off/retrigger, stealing the oldest-claimed channel
  -- forcibly ending THAT note cleanly first -- if all 15 are already in
  use (a real possibility on a 24-pad board, mirrors
  `services/haptics.c`'s own voice-stealing policy almost exactly).
  Pitch bend and poly aftertouch are now genuinely independent per note,
  each on its own channel -- no more single-"owner"-pad workaround; see
  `services/expression.c`'s "Pitch bend from sideways motion" section for
  the fuller history of what that workaround used to be and why MPE
  removes the need for it entirely.
  `tiles_midi_send_cc_broadcast(controller, value)` exists alongside the
  single-channel `tiles_midi_send_cc()` specifically for
  `services/pedal.c`'s sustain/expression CCs -- under MPE there's no
  single "right" channel for a pedal message that needs to reach every
  currently-sounding note, so it's sent to the Zone Master Channel and
  all 15 Member Channels at once.
  Declares a 48-semitone Member Channel pitch bend range via RPN 0 (the
  MPE specification's own recommended default, and what a real ROLI
  Seaboard ships with) -- purely a receiver-side interpretation setting,
  independent of `services/expression.c`'s own sensitivity tuning for
  how much raw wire value a given tilt produces.
  **Not hardware-verified at all** -- the whole MPE implementation
  (zone-config RPN messages, per-note channel allocation, channel
  stealing) has not been tried against a real MPE-aware DAW/synth yet.
- DIN MIDI IN/OUT -- not built yet.
