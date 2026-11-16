# audio/

TILES's own sound, streamed to the computer over the same USB-C cable as
MIDI: a stereo 48 kHz / 24-bit USB audio input (UAC2, class compliant on
macOS, Windows 10+, Linux and iPadOS) that a DAW records like any
interface input. A sound is the **wavetable engine** or the **FM
engine** (13 + 3 factory sounds, a sound per track); how they are built
and what they measure:
`../../../docs/architecture/synth-engines.md`.

| File | What it is |
|---|---|
| `audio_engine.c/.h` | The four tracks, a sound (patch) each, with the guardrails: voices admitted by measured cost, at most 8 a track, the stealing order and the live load guard (`../../../docs/architecture/audio-engine.md`, "Guardrails"); MPE, mono/legato note stacks, the stereo mix. Plain C, no Pico code, tested on the computer (`test/test_audio_engine.c`). |
| `synth_voice.c/.h`, `synth_patches.c` | The wavetable voice (up to 3 unison copies, drive/fold, SVF or ladder, two envelopes, LFO, glide) and the factory sounds. Tested on the computer (`test/test_synth.c`). |
| `fm_voice.c/.h`, `fm_patches.c` | The FM voice: six operators playing DX7 voice data (32 algorithms, envelopes, scaling, LFO, pitch envelope), pressure and slide brightening the modulators; DX7 bank and voice unpacking; the factory FM voices. Tested on the computer (`test/test_fm.c`). |
| `synth_dsp.h` | The building blocks: filters, envelope, LFO, shapers, fast maths. |
| `fx.c/.h` | The effects (`../../../docs/architecture/synth-engines.md`, "Effects"): per track overdrive (anti-aliased), bit-crush and the sidechain pump; shared plate reverb, ping-pong delay and ensemble fed by sends. Plain C, tested on the computer (`test/test_fx.c`). |
| `fx_memory.c/.h` | The effects' 182 KB, also the boot-time wavetable build's working space and lent to the pad capture (`diagnostics/capture.c`): core 1 lets go between two blocks and the effects go quiet until `CAPTURE RELEASE`. |
| `wavetable.c/.h`, `wavetable_banks.c` | Band-limited (mip-mapped) wavetables built at boot from spectra; the three factory banks. |
| `audio_hot.h` | `TILES_AUDIO_HOT()`: audio code in RAM on the board (and `TILES_AUDIO_COLD`: rare code of a RAM-placed file left in flash). Everything core 1 touches per block is in RAM, data too (patches are not `const`): a flash read from core 1 stalls behind core 0's code fetches, which doubled the voices' cost. |
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
- **USB runs on core 1.** TinyUSB re-arms the isochronous endpoint only
  inside `tud_task()`, and the 1 ms frame won't wait for core 0's ~2 ms
  pass. So in standalone mode core 1 starts USB (the RP2350 driver wants
  its interrupt and `tud_task()` on one core) and runs `tud_task()` every
  ~0.2 ms, between render blocks too. TinyUSB's pico OS layer
  (`CFG_TUSB_OS OPT_OS_PICO`) locks its queue and the class FIFOs, so
  core 0 still writes MIDI and talks on the settings interface; its own
  calls go through `tiles_usb_task()`, which does nothing then, and the
  console is a small stdio driver without pico_stdio_usb's `tud_task()`.
- **Every packet is 48 frames.** A frame with no packet loses its audio
  (`audio.late_frames`); packets of 96 to make one up stopped macOS.
- **Core 1's USB path runs from RAM.** TinyUSB, its RP2350 driver, this
  folder's USB and note-handling code and the float maths are kept out of
  flash (`src/link/default_text_excludes.incl`). From flash they waited
  behind core 0's fetches through the shared 16 KB XIP cache: a
  `tud_task()` took up to 0.34 ms, packets missed their frame and macOS
  restarted the stream (a second of silence at a time). In RAM:
  `tud_task()` at most ~30 us, `audio.usb_gap_us` ~0.2 ms, no late frame
  through a recorded tour of every sound. Core 1 waits between passes
  without `sleep_us()` (its alarm is core 0's interrupt).
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

## Only in standalone mode

All of this runs in standalone mode only (`services/device_mode.h`); in
controller mode USB has no audio input and core 1 isn't started. The
first build, with USB serviced from core 0's ~2 ms loop, was found on
2026-11-12 (unit 2, ffmpeg on macOS) to lose a third of its frames and
have macOS stop the stream within 0.2 s. Not yet checked on a board:
standalone mode with USB on core 1.

## Checking it

`tools/tiles_control.py info` lists `audio.*`:

| Field | Healthy |
|---|---|
| `audio.streaming` | 1 while an app has the input open |
| `audio.late_frames` | 0: each one is a frame of lost audio (the stream's first few packets, before the host polls, aren't counted) |
| `audio.fx` | `on`, or `lent` while the pad capture holds the effects' memory |
| `audio.fx_cost` | the effects' cycles per sample, followed live (~500-800 while they ring) |
| `audio.usb_gap_us` | core 1's longest stretch between `tud_task()` calls while streaming; ~200-300, under 1,000 or packets miss frames |
| `audio.underruns` | 0 (core 1 never fell behind) |
| `audio.midi_drops` | 0 |
| `audio.load_pct` | core 1 time rendering in the last second |
| `audio.drums` | drum samples ringing now |
| `audio.drum_underruns` | 0 (the stream from flash never ran dry) |
| `audio.shed` | voices the load guard ended; 0 unless the sounds asked for more than the core has |
| `audio.cost` | each track's measured cycles per voice-sample (150 until it has played) |
| `flash.lockout_fails` | 0 |
| `audio.patch` | each track's sound |
| `fault` | `none`; else the last boot ended in a panic or hard fault (`fault.message`, `fault.callers`; `diagnostics/fault.h`) |

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
