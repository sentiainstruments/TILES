# storage/

Versioned, CRC-protected configuration storage in Pico flash, with two
alternating slots so a failed/interrupted write never bricks the active
data.

## Built

- **`kv_store.{h,c}`** -- the two-slot blob store, pure logic (no Pico SDK).
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
- **`storage_flash.{h,c}`** -- the real flash ops (XIP reads; erase/program
  with interrupts off and the watchdog petted before/after, the same
  single-core pattern the pattern bank uses) and a boot-time check that the
  application image hasn't grown into the settings region (if it has,
  nothing writes).
- **`flash_map.h`** -- every flash region in one place (all at the END of
  flash, far from the image; `picotool load` never touches them, so they
  survive a firmware update):
  - last sector: sequencer pattern bank (`services/op_mode.c`)
  - 4 sectors below: Song mode store (`services/op_mode.c`)
  - 2 sectors below that: settings (`profiles/settings_persist.c`)

## What uses it

Today only the settings table (`../profiles/README.md`). The store is generic
-- a payload plus a version number -- so per-pad calibration and, later,
layouts can use the same mechanism (another region + `tiles_kv_ops_t`).

## Costs and limits

- A sector erase stops the whole firmware for tens of milliseconds
  (memory-mapped flash can't be read while it is written). Callers should
  only save when idle -- `settings_persist.c` waits for hands off the pads.
- Two 4 KB sectors, alternating: each sees half the saves. At the debounced
  rate settings change (a handful of saves per tuning session) that is far
  inside NOR flash endurance.
- One payload per store. If layouts get large, give them their own region
  rather than growing this one.
