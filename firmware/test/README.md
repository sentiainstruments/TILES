# test/

Host-buildable unit tests (no hardware required) for logic that can be
isolated from the Pico SDK runtime. Hardware-dependent code stays covered
by the `diagnostics/` manufacturing test commands instead.

- `test_pad_config.c` — asserts all 24 touch/Hall/LED/haptic/FPC routes
  in `board/pad_config.c` are unique, plus row/col/fpc sequencing and the
  LED mux-index→enable-port relationship. Required by
  `SENTIA_FIRMWARE_CODEX_START.md` before writing further implementation
  code. Run it: `./run.sh`.

Planned additions as those modules get built: scale/note mapping, voice
allocation, calibration math, `usb_vendor/` protocol framing.
- `test_din_midi_queue.c` -- `midi/din_midi_queue.c`, the hardware-free half
  of DIN MIDI: ordering, coalescing of pitch bend/pressure/expression,
  Real-Time priority, whole-message overflow drops, ring wraparound.
- `test_midi_in.c` -- the real `midi/midi_in.c` (tusb/pico-time stubbed in
  `stubs/`): USB and DIN each parse with their own state, running status
  per source, one clock owner at a time, loss recovery, SysEx from DIN.
- `pio_sim_din_tx.py` -- runs the *assembled* DIN OUT PIO program through a
  tiny instruction-level simulator and decodes its waveform as 8N1 at
  31,250 baud. Needs a firmware build first (it reads pioasm's header).
  None of these touch the electrical side (jacks, buffer, opto, TRS
  polarity) -- that only shows up on real hardware.
