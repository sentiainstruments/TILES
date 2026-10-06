# midi/

Musical I/O and the USB device itself: USB-MIDI (two ports), DIN MIDI in
and out, and the MPE wire protocol. Configuration and calibration traffic
never goes over MIDI; that's `usb_vendor/`.

| File | What it is |
|---|---|
| `usb_device`, `usb_descriptors.c`, `tusb_config.h` | The composite USB device: CDC console, MIDI (2 cables), vendor (settings), picotool reset interface. MS OS 2.0 descriptors give Windows WinUSB for the vendor and reset interfaces. |
| `product_identity.h` | USB VID/PID, firmware version, SysEx manufacturer ID, all in one place. **The USB ID is a test ID and must be replaced before any unit ships.** |
| `midi_ports.h` | The ports: MAIN (cable 0, "MIDI"), DAW (cable 1, "DAW"), DIN. |
| `midi_out` | Every outgoing message, including the MPE zone setup (RPN 6 + RPN 0), per-note setup, panic, DAW-only CCs and SysEx. |
| `midi_in` | The one reader of all MIDI input. It parses each source with its own parser (running status, SysEx, Real-Time) and fires callbacks. |
| `usb_midi_packet` | USB-MIDI 1.0 packet packing, used instead of TinyUSB's byte-stream API. Pure, tested. |
| `din_midi`, `din_midi_queue`, `din_midi_tx.pio` | DIN: a PIO UART out, UART0 in, and the hardware-free queues (coalescing, running status, Real-Time priority). The queues are pure and tested. |
| `identity` | Answers the standard MIDI Identity Request. |

## Ports

| Port | Carries | macOS / Live name |
|---|---|---|
| MAIN | Notes, MPE, pedals, clock and transport Start/Stop, Identity | "SENTIA TILES 2 MIDI" / "SENTIA TILES 2 (MIDI)" (unit 2) |
| DAW | Only the Ableton control surface script: Scene Launch and transport CCs out; clip/scene SysEx and TILES DISPLAY echo notes in | "SENTIA TILES 2 DAW" / "SENTIA TILES 2 (DAW)" (unit 2) |
| DIN | Everything MAIN carries | TRS jack |

The DAW port exists so script traffic never reaches an instrument track,
the same way Launchkey, Push and KeyLab do it. Setup in Live:
`../../../daw-integration/README.md`.

## MPE

A single Lower Zone. Channel 1 is the Master Channel (zone RPNs, pedals;
all notes when MPE is off). Channels 2-9 are Member Channels, one per
held note. How many of them are in the zone depends on Song mode; the
channel map is `../services/midi_channels.h` and per-note allocation is
`../services/mpe_alloc.h`. Conformance to MMA RP-053:

- The zone is declared with RPN 6, always followed by RPN 0 (pitch bend
  range 12, also sent on each member). JUCE receivers reset to 48 on
  every RPN 6. The zone is re-declared only when idle, because a zone
  change stops notes.
- Before each Note-On the channel's bend is recentered and its pressure
  zeroed (only if needed). At Note-Off nothing is reset, so tails ring
  as played.
- Pressure is Channel Pressure (not Poly Aftertouch). Release velocity
  is real. CC74 (Y/timbre) is not sent yet.
- Pedals go on the Master Channel only. Panic is the one broadcast.

## USB details worth knowing

- `main.c` must call `tud_task()` every loop pass (linking
  `tinyusb_device` turns off the SDK's background task).
- USB writes wait for FIFO room for up to 5 ms, pumping `tud_task()`, then
  give up and log. They never block indefinitely.
- On macOS, a device that once enumerated with a different port layout can
  leave stale entries in CoreMIDI; the cleanup is in
  `../../../daw-integration/README.md`.

## DIN details worth knowing

- Output is 31,250 baud, about 1 ms per message. Notes and ordinary CCs
  are reliable and ordered. Bend, pressure and continuous CCs are
  coalesced to the latest value, and a channel's pending values flush
  before its next note.
- Running status is on (DIN only). Active Sensing is sent after 250 ms
  of silence.
- TRS polarity defaults to Type A and changes only through the
  `midi.din_trs_type` setting; it is never auto-detected. Type A = GP0
  is inferred from the handoff docs, so check it first if a Type A
  device hears nothing.
- SysEx is not sent to DIN.

## Tests

`firmware/test/`: `test_midi_in.c`, `test_usb_midi_packet.c`,
`test_din_midi_queue.c` (plus `pio_sim_din_tx.py` for the PIO program)
and `test_identity.c`.

## History

`HISTORY.md` holds the full development log: the MPE pitch bend range
saga, the USB truncation and stuck-sustain fixes, DIN bring-up, the
USB ID change and the two-port split.
