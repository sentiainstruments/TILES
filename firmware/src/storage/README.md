# storage/

Versioned, CRC-protected configuration storage in Pico flash, with two
alternating slots so a failed/interrupted write never bricks the active
data.

## Files

- **`kv_store.{h,c}`** -- the two-slot blob store, pure logic (no Pico SDK).
  Each store is a `tiles_kv_t` the caller owns, bound to one region's ops, so
  several regions use it side by side (settings, content).
  One caller-defined payload (up to 4076 bytes) lives in one of two 4 KB
  sectors. A save goes to the OTHER slot: erase it, program every page except
  the first, then the first page (the one holding the magic) LAST, then read
  the whole thing back and check the CRC before it counts. The newest valid
  slot (highest sequence number) wins at boot. A power cut or crash at ANY
  instant of a save leaves the previous payload intact -- tested natively
  with a simulated flash that loses power after every erase/program step,
  with partially-applied operations (`firmware/test/test_kv_store.c`).
  Header (20 bytes, little-endian): magic, layout version, the caller's
  payload version, sequence number, length, CRC-32.
- **`log_store.{h,c}`** -- an append-only record log over two sectors, for
  data that changes while music plays (drum mode's pattern; the mode,
  scale, key and octave in `services/play_state.c`). Each save is a
  new page-aligned record after the last (page programs only, well under a
  millisecond each, and `tiles_log_append_start/step` puts out one page
  per call so a big record never stalls more than one); erasing the spare
  sector is a separate call the owner makes when a stall doesn't matter
  (the clock stopped and TILES's own sound silent). Same power-cut guarantee as kv_store, tested
  the same way (`firmware/test/test_log_store.c`).
- **`flash_staging.{h,c}`** -- one 4 KB buffer shared by the saves that
  build and write within one call (op_mode's pattern and song sectors,
  content's next blob), instead of 4 KB each. A save that writes over
  several passes (the log stores' appends) keeps its own.
- **`storage_flash.{h,c}`** -- the real flash ops (XIP reads; erase/program
  with interrupts off and the watchdog petted before/after, the same
  single-core pattern the pattern bank uses) and a boot-time check that the
  application image hasn't grown into a region (if it has, nothing writes).
  One set of ops per region (settings, content, drums, play state), from
  one implementation.
- **`flash_map.h`** -- every flash region in one place (all at the END of
  flash, far from the image; `picotool load` never touches them, so they
  survive a firmware update):
  - last sector: sequencer pattern bank (`services/op_mode.c`)
  - 4 sectors below: Song mode store (`services/op_mode.c`)
  - 2 sectors below that: settings (`profiles/settings_persist.c`)
  - 2 sectors below that: content store, custom scales (`profiles/content.c`)
  - 2 sectors below that: drum mode's pattern (`services/drum_seq.c`, log_store)
  - 2 sectors below that: mode, scale, key, octave (`services/play_state.c`, log_store)
  - the saved data keeps a 256 KB band at the top; below it, the drum
    sampler's 8 slots, 192 KB each (1.5 MB, `audio/sample_store.c`): its
    own format (a header programmed last over 16-bit audio), erased a
    sector at a time as a load reaches it

## What uses it

The settings table and the content store (`../profiles/README.md`), each on
its own region. The store is generic -- a payload plus a version number -- so
per-pad calibration can use the same mechanism (another region, another
`tiles_kv_t`).

## Costs and limits

- A sector erase stops the whole firmware for tens of milliseconds
  (memory-mapped flash can't be read while it is written). Callers should
  only save when idle -- `settings_persist.c` waits for hands off the pads.
- Two 4 KB sectors, alternating: each sees half the saves. At the debounced
  rate settings change (a handful of saves per tuning session) that is far
  inside NOR flash endurance.
- One payload per store. If layouts get large, give them their own region
  rather than growing this one.
