# profiles/

Everything that's *configuration*, not *code*: what the companion app edits
and downloads, and what survives a reboot.

## The settings table

One registry of every user-tunable value (`settings.{h,c}`), the table itself
(`settings_table.c`), and saving it to flash (`settings_persist.{h,c}`, on top
of `../storage/`).

**A setting is one row** -- `id`, `key`, type, range, and two functions: how to
read the value from the module that owns it, and how to apply a new one. From
that single row it gets GET/SET/LIST/SCHEMA/RESET over USB, range checking and
saving to flash, with no protocol change. Types: bool, uint, int, float, enum,
color (`RRGGBB` or `none`).

**Rules the design depends on**
- **The modules stay the source of truth.** Rows call into pedal, expression,
  lighting, DIN, etc.; the table holds no second copy that could drift, and a
  change made on the device is seen exactly like one made by the app.
- **Defaults are captured from the module at boot**, before any saved value is
  applied -- they aren't written a second time in the table, so the schema
  reports the true default and there's nothing to keep in sync.
- **Saving is sparse**: only values that differ from their default are written.
  A blank store means "all defaults"; a setting the user never touched follows
  the firmware default if a later build changes it. (A value the user set equal
  to today's default will follow it too -- deliberate.)
- **IDs are permanent.** The numeric id is what's written to flash and what a
  future binary protocol will use; never reuse or renumber one. The text key is
  for humans and scripts. Ids are grouped by hundreds: `0x01xx` pedal, `0x02xx`
  expression, `0x03xx` CV/gate, `0x04xx` LED look, `0x05xx` MIDI, `0x06xx`
  features, `0x07xx` colours (scheme and per-pad). Retired: `0x0401`-`0x0404`
  (the old root/fifth/natural levels, replaced by the colour scheme in 0.2.7).
- **`TILES_SETTING_VOLATILE`** rows are settable but never saved or restored --
  used for `cv_gate.enabled`, which must stay an explicit per-session switch
  that boots off. Everything else is saved.
- **Changes are found by diffing**, not by hooking setters: every 500 ms the
  current sparse snapshot is compared with what flash holds; a changed one is
  written only after 2 s without further change AND with every pad untouched (a
  flash erase stalls the firmware for tens of ms). A failed write retries after
  30 s. Unknown ids / changed types / out-of-range values in a saved snapshot
  are skipped and cleaned up by the next save; a snapshot from an unknown blob
  version is ignored.
- **Nothing here can raise the LED power ceiling or bypass the CV/gate power
  gate** -- LED settings are fractions of the fixed ceiling; CV ranges are
  limited to what the jack can use, and external-power gating is separate
  hardware-backed logic.

**Adding a setting**: add one `TILES_SETTING(...)` row (and, if the value lives
in a module without an accessor pair, add the pair). Pick the next free id in
its group. Nothing else -- no protocol, storage or docs plumbing.

**Tested natively** (`firmware/test/test_settings.c`): parsing and range rules,
the schema text, sparse encode/decode incl. bad/unknown/volatile entries,
debounce, idle-gating, retry, RESET, "new build changed a default", plus
`test_kv_store.c` for the flash store's power-loss behaviour.

## The content store

Things the companion app *pushes* rather than tunes (`content.{h,c}`): today
custom scales for the picker's 9 custom slots, each with the pack, item and
version it came from. Its own two-sector flash region (`../storage/flash_map.h`),
one blob of typed records, so layouts and other data join later as new record
types. Unlike settings, a change is written to flash before it's applied or
acknowledged (the app pushes deliberately; no debounce). Records of an unknown
type are kept; a blob from a newer format makes the store read-only until
`CONTENT CLEAR`. Commands: `SCALES`, `SCALE GET/PUT/DELETE`, `CONTENT
LIST/CLEAR` (`shared/protocol/README.md`). Where it's heading (layouts, paid
packs, unlocking modes): `docs/architecture/content-packs.md`. Tested natively
in `firmware/test/test_content.c`, against the real note map.

## Not built yet

The fuller "profile" this directory was always meant to hold -- feature flags,
per-mode layouts (pad->note maps), button bindings, and the
power-budget demo profiles (SAFE_BRINGUP, USB_DEMO_SAFE, ...). Those become more
rows and, for layouts, their own stored region. Firmware-side "next scale" style
button actions will mutate the same settings through the module setters, so app
edits and on-device edits stay consistent by construction (both end up in the
module the table reads from).
