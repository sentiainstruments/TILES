# audio/

TILES's own sound, streamed to the computer over the same USB-C cable as
MIDI: a stereo 48 kHz / 24-bit USB audio input (UAC2, class compliant on
macOS, Windows 10+, Linux and iPadOS) that a DAW records like any
interface input. For now the sound is a placeholder **test voice**; the
real engines replace the voice code, not the plumbing.

| File | What it is |
|---|---|
| `audio_engine.c/.h` | The test voice, a sine with a little harmonic colour, on the four tracks, with the guardrails: voices admitted by measured cost, at most 8 a track, the stealing order and the live load guard (`../../../docs/architecture/audio-engine.md`, "Guardrails"). Plain C, no Pico code, tested on the computer (`test/test_audio_engine.c`). |
| `sampler.c/.h` | The drum sampler's 5 voices on core 1: a slot's first 1 KB waits in RAM so a hit starts at once, the rest streams into a 2 KB ring per voice; a pad's new hit restarts its own voice, a sixth takes the oldest. Plain C, tested on the computer (`test/test_sampler.c`). |
| `sample_slot.c/.h` | One slot's format in flash: a 256-byte header (written last), then 16-bit 48 kHz audio, mono up to 2 s or stereo up to 1 s. |
| `sample_store.c/.h` | Loading (page by page, each sector erased as the data reaches it, CRC checked against flash before the header goes on), deleting and finding the 8 slots. Plain C over flash ops, power-cut tested on the computer. |
| `sample_stream.c/.h` | The board side: the slots' flash ops, and streaming by DMA from the XIP stream FIFO (flash idle cycles, no cache pollution), stopped around every flash write. |
| `audio_ring.h` | The lock-free single-producer/single-consumer ring the two cores talk through. |
| `usb_audio.c/.h` | Starts core 1, feeds it TILES's MIDI, and answers TinyUSB's audio callbacks. The descriptor is in `midi/usb_descriptors.c`, the TinyUSB options in `midi/tusb_config.h`. |
| `dsp_bench.c/.h` | The DSP stress test: high-quality prototypes of every planned engine, drum and effect, timed on core 1. Only in `-DTILES_DSP_BENCH=ON` builds (`tools/tiles_control.py bench`); results in `../../../docs/architecture/audio-engine.md`. |

## How it runs

- **Core 1 makes the sound.** Core 0 keeps the instrument loop as it was;
  core 1 (idle until now) runs the engine. It keeps 3 ms of audio
  rendered ahead in a ring and sleeps in between.
- **The engine hears what TILES plays.** `midi_out.c` copies every
  channel message it sends into a queue for core 1, except drum hits
  (sent between `tiles_midi_out_drums_begin/end`, any channel in Volca
  mode). The channel says the track (`tiles_audio_engine_track()`). The
  same messages go to USB MIDI and DIN, so recording MIDI and audio
  together lines up.
- **Drums are the sampler.** Drum mode (`services/drum_seq.c`) sends each
  hit of its first bank's 8 drums to `tiles_usb_audio_drum_hit()`, a
  queue of its own to core 1. Slots are loaded over the settings link
  (`SAMPLE BEGIN`, `shared/protocol/README.md`; `tools/tiles_control.py
  sample put`). The engine mixes the drums in stereo next to the voices.
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
  stream stops for that time too (an erase is tens of ms). So an erase
  waits for the clock to stop and this sound to be silent
  (`tiles_usb_audio_is_quiet()`: no host listening, or no voice
  sounding): the autosaves, settings, pattern bank and Song. Saves while
  playing write one 256-byte page at a time, at least 8 ms apart (under
  a millisecond each; core 1 refills the 3 ms ring between them, and a
  late USB frame is made up).
  Custom scales from the companion app still save at once.
- **Latency** added by TILES: the 3 ms ring plus one packet, about 4 ms,
  on top of the host's own input buffer.

## Known issue: macOS closes the stream

Found 2026-11-12 recording the input with ffmpeg on macOS (unit 2):
the Mac opens "SENTIA TILES 2", takes 40-130 packets (under 0.2 s,
silent) and stops; `audio.late_frames` was a third of the packets.
One `tud_task()` a ~2 ms pass misses the 1 ms deadline to re-arm the
isochronous endpoint about every other frame, and the catch-up packets
of 96 frames don't keep macOS streaming. Calling `tud_task()` between
I2C transactions as well didn't lower the late frames. Next: run USB
(`tud_task()`) on core 1, which loops every 200 us, with the classes'
FIFOs made safe across the cores; until then the sound can't be heard
or recorded, and the sampler is checked on the computer
(`test/test_sampler.c`) and on the board by its counters.

## Checking it

`tools/tiles_control.py info` lists `audio.*`:

| Field | Healthy |
|---|---|
| `audio.streaming` | 1 while an app has the input open |
| `audio.late_frames` | grows slowly or not at all; made up, not lost |
| `audio.underruns` | 0 (core 1 never fell behind) |
| `audio.midi_drops` | 0 |
| `audio.load_pct` | core 1 time rendering in the last second |
| `audio.drums` | drum samples ringing now |
| `audio.drum_underruns` | 0 (the stream from flash never ran dry) |
| `audio.shed` | voices the load guard ended; 0 unless the sounds asked for more than the core has |
| `audio.cost` | each track's measured cycles per voice-sample (150 until it has played) |
| `flash.lockout_fails` | 0 |

On a Mac: Audio MIDI Setup shows "SENTIA TILES N" with 2 inputs at
48 kHz. In Live, pick it as the audio input device, arm an audio track
with input 1/2, and play. If macOS shows the old device without audio
after flashing, delete the greyed SENTIA TILES in Audio MIDI Setup >
MIDI Studio and replug (the same cache as the Parts port).

## Next

The plan (voices, engines, effects, budgets, clocks) is
`../../../docs/architecture/audio-engine.md`; TILES v1 (First Edition)
adds the DAC and jack (`../../../docs/hardware/audio-output.md`,
`tiles-v1.md`).
