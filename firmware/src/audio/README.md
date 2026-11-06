# audio/

TILES's own sound, streamed to the computer over the same USB-C cable as
MIDI: a stereo 48 kHz / 24-bit USB audio input (UAC2, class compliant on
macOS, Windows 10+, Linux and iPadOS) that a DAW records like any
interface input. For now the sound is a placeholder **test voice**; the
real engines replace the voice code, not the plumbing.

| File | What it is |
|---|---|
| `audio_engine.c/.h` | The test voice: up to 16 MPE voices, a sine with a little harmonic colour. Plain C, no Pico code, tested on the computer (`test/test_audio_engine.c`). |
| `audio_ring.h` | The lock-free single-producer/single-consumer ring the two cores talk through. |
| `usb_audio.c/.h` | Starts core 1, feeds it TILES's MIDI, and answers TinyUSB's audio callbacks. The descriptor is in `midi/usb_descriptors.c`, the TinyUSB options in `midi/tusb_config.h`. |

## How it runs

- **Core 1 makes the sound.** Core 0 keeps the instrument loop as it was;
  core 1 (idle until now) runs the engine. It keeps 3 ms of audio
  rendered ahead in a ring and sleeps in between.
- **The engine hears what TILES plays.** `midi_out.c` copies every
  channel message it sends into a queue for core 1, except drums
  (channel 10) and what Song and Volca route to the Parts port. The same
  messages go to USB MIDI and DIN, so recording MIDI and audio together
  lines up.
- **USB takes 48 frames a millisecond.** TinyUSB re-arms the isochronous
  endpoint only inside `tud_task()`, which the main loop calls a few
  times a pass. Each packet therefore carries 48 frames per USB frame
  since the last one: if a frame was missed, the next packet carries two
  frames' audio (up to 96 frames, the endpoint's size) and nothing is
  lost from the recording. `audio.late_frames` counts how often.
- **Flash writes pause core 1.** XIP is off while flash is erased or
  programmed, so no core may run from flash. Every write goes through
  `tiles_flash_write_begin()/end()` (`storage/storage_flash.h`), which
  locks core 1 out (`multicore_lockout`) and masks interrupts; the USB
  stream stops for that time too (an erase is tens of ms). Saves already
  wait for idle pads or a stopped clock, so this lands on silence.
- **Latency** added by TILES: the 3 ms ring plus one packet, about 4 ms,
  on top of the host's own input buffer.

## Checking it

`tools/tiles_control.py info` lists `audio.*`:

| Field | Healthy |
|---|---|
| `audio.streaming` | 1 while an app has the input open |
| `audio.late_frames` | grows slowly or not at all; made up, not lost |
| `audio.underruns` | 0 (core 1 never fell behind) |
| `audio.midi_drops` | 0 |
| `audio.load_pct` | core 1 time rendering in the last second |
| `flash.lockout_fails` | 0 |

On a Mac: Audio MIDI Setup shows "SENTIA TILES N" with 2 inputs at
48 kHz. In Live, pick it as the audio input device, arm an audio track
with input 1/2, and play. If macOS shows the old device without audio
after flashing, delete the greyed SENTIA TILES in Audio MIDI Setup >
MIDI Studio and replug (the same cache as the Parts port).

## Next

- The real engines (the web player's sounds are the starting point:
  `TILES_QuickStart/play/synth-worklet.js`).
- The same engine output to an I2S DAC for headphones and line out on
  the next board (`../../../docs/hardware/audio-output.md`).
