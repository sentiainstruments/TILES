# firmware/AGENTS.md

Raspberry Pi Pico 2 (RP2350) firmware, Pico SDK C/C++ + TinyUSB. This is
where nearly all active work on this repo happens, developed iteratively
against real hardware rather than written once and shipped.

## Build

```bash
cmake -S firmware -B firmware/build -DPICO_BOARD=pico2
cmake --build firmware/build -j4
```

**`-DPICO_BOARD=pico2` is not optional.** Without it, cmake silently
targets plain `pico` (RP2040) instead of the real RP2350 board — the
build still succeeds, but produces a `.uf2` the RP2350 bootrom rejects
outright (no boot metadata block), and the failure shows up as "won't
boot," not as a build error. If `firmware/build/` already exists from
before this was known, wipe it and reconfigure — don't assume an
existing `build/` is correctly configured.

A clean build must produce **zero warnings**. That's the bar every
change here is held to, not just "it compiles."

## Flash

**Use `picotool`, not drag-and-drop UF2 copy.** Plain mass-storage copy
onto this hardware was found unreliable (it's how the `-DPICO_BOARD`
issue above went unnoticed for a while — a bad `.uf2` copied via Finder
gives no error, just a board that won't boot). `brew install picotool`
once, then:

```bash
picotool load -x -v --ignore-partitions firmware/build/src/sentia_tiles_firmware.uf2
```

The board must be in BOOTSEL mode first — `picotool info -a` confirms
("No accessible RP-series devices in BOOTSEL mode" means it isn't). This
one command flashes, verifies, and reboots into the application. Confirm
the reboot with `ls /dev/cu.usbmodem*` (present = booted into the app;
absent = still in bootloader, or didn't come up).

## Workflow for a real-hardware feedback round

1. Implement the change.
2. Build clean (above) — zero warnings, non-negotiable.
3. Document it in the nearest `services/README.md` / `board/README.md`
   (see below): what real feedback prompted it, what the actual bug or
   gap was, and why this specific fix. Future rounds — by you or another
   agent — rely on this history to avoid re-breaking something that was
   already fixed for a reason that isn't obvious from the code alone.
4. Commit (see repo-root `AGENTS.md` for the commit-trailer rule) and
   push.
5. Flash (above), confirm boot.
6. A human tests on the real board and reports back. Iterate from step 1.

Don't skip step 3 to save time. This codebase's comments are
deliberately dense with `"real feedback: '...'"` quotes for exactly this
reason — it's the only record of *why* a constant or gesture is shaped
the way it is, and the only defense against silently re-introducing a
bug a past round already diagnosed and fixed on real hardware.

## Module boundary

| Module | Owns |
|---|---|
| `src/board/` | Raw GPIO, I2C/SPI/PIO resources, hardware constants, the canonical `PadConfig[24]` table. Nothing outside this module touches a pin/channel number directly. |
| `src/drivers/` | Per-chip drivers (TCA9548A, TMAG5273, MPR121, PCA9685, TCA9554, SK6805, DAC80502). |
| `src/services/` | Hall scanning, touch, lighting, haptics, pedal, expression/MPE mapping, op-mode (melodic/chord/sequencer/guitar), MIDI clock, power. **This is where almost every real-hardware fix this project has made lives — see `services/README.md` below before changing anything here.** |
| `src/midi/` | USB-MIDI/MPE (TinyUSB), DIN MIDI in, DIN MIDI out. |
| `src/usb_vendor/` | Companion-app protocol (remap/calibration/diagnostics), implementing `../shared/protocol/`. |
| `src/profiles/` | Runtime feature flags, layouts, bindings, themes — what the companion app edits/downloads. |
| `src/diagnostics/` | Per-pad test commands, I2C enumeration, sensor streaming. |
| `src/storage/` | Versioned, CRC-protected flash config storage. |

## `services/README.md` — read it, but don't load it wholesale

At 2900+ lines and growing every round, it's a running design-and-
incident log for every service, not a quick reference. **Grep it for the
specific service, constant, or gesture you're about to touch** before
changing behavior there. Many thresholds, gesture shapes, and edge cases
exist because of a specific, already-diagnosed real-hardware bug, and
the fix's own rationale — often a direct tester quote — is the only
place that context survives. Reading the whole file up front rarely pays
for its size; reading the 20-40 lines around your target function almost
always does. `board/README.md` is the same idea, scoped to `src/board/`.

## Hardware non-negotiables

Full authoritative list: this directory's own `README.md` ("Non-
negotiables" section) and `../docs/hardware/`. Short version:

- Never drive GP20 high (shared PCA9685 output-enable pin) — low enables
  outputs, and only after every channel is already configured.
- Only one Hall mux channel enabled across all three TCA9548A devices at
  a time.
- Both PCA9685 devices initialize all-off before any other output
  service starts.
- Never auto-select a USB power budget above 500mA.
- CV/gate stay off unless GP22 proves external power is selected.
- A failed subsystem disables itself; it never blocks USB diagnostics.
