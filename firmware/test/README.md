# test/

Host-side unit tests for the logic that doesn't need the Pico SDK or
hardware. Run them all from `firmware/`:

```bash
./test/run.sh
```

Each test compiles the real module source with the host `cc`; where a
module calls into TinyUSB or pico time, `stubs/` stands in. Hardware
paths are checked on the board instead (`src/diagnostics/`).

| Test | Covers |
|---|---|
| `test_pad_config.c` | `board/pad_config.c`: all 24 touch, Hall, LED, haptic and FPC routes unique; row/col/FPC order; LED mux index -> enable port. |
| `test_note_map.c` | `services/note_map.c`: the default chromatic layout, pad 19 = lowest C, bottom-to-top and left-to-right through all 24 pads. |
| `test_mpe_alloc.c` | `services/mpe_alloc.c`: MPE Member Channel choice (spec section 3.2: same-note reuse, longest idle first) and steal order (pedal-held first, then oldest, never a harmonic). |
| `test_midi_channels.c` | `services/midi_channels.c`: the fixed channels, the shared pool Song and the live MPE zone draw from (highest free first, contiguous zone size, declared vs. actual zone size), and that neither can take a channel the other is using. |
| `test_usb_midi_packet.c` | `midi/usb_midi_packet.c`: USB-MIDI 1.0 event packets (cable + Code Index Number) for every message TILES sends, SysEx chunking, receive-side byte counts. |
| `test_midi_in.c` | `midi/midi_in.c`: USB MAIN, USB DAW and DIN parsed with separate state, running status per source, one clock owner at a time, loss recovery, SysEx tagged with its port. |
| `test_din_midi_queue.c` | `midi/din_midi_queue.c`: DIN OUT ordering, coalescing of pitch bend/pressure/expression, Real-Time priority, whole-message overflow drops, wraparound. |
| `test_identity.c` | `midi/identity.c`: the Universal MIDI Identity Request/Reply; malformed or unrelated SysEx ignored. |
| `test_kv_store.c` | `storage/kv_store.c` on simulated NOR flash with a power cut after every erase/program step: always the old or the new payload, never garbage. |
| `test_settings.c` | `profiles/settings.c` and `settings_persist.c`: parsing and ranges, schema text, the sparse flash blob, and the debounced saver (idle gating, retry, RESET). |
| `pio_sim_din_tx.py` | The assembled DIN OUT PIO program, simulated and decoded as 8N1 at 31,250 baud. Needs a firmware build first (reads pioasm's header); skipped otherwise. |

None of these cover the DIN electrical side (jacks, buffer, opto, TRS
polarity); that is only testable on hardware.
