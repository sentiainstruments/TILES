# firmware/

Raspberry Pi Pico 2 (RP2350) firmware for SENTIA TILES Rev A0. Pico SDK
C/C++ + TinyUSB. Hardware truth for this revision lives in
`../docs/hardware/` — nothing in here should hard-code a GPIO number, I2C
address, or channel that isn't already in that board map.

## Module boundary

| Module | Owns |
|---|---|
| `src/board/` | Raw GPIO, I2C/SPI/PIO resources, hardware constants, and the canonical `PadConfig[24]` table. Nothing outside this module touches a physical pin or channel number directly. |
| `src/drivers/` | Small, independently testable per-chip drivers: TCA9548A, TMAG5273, MPR121, PCA9685, TCA9554, SK6805, DAC80502. |
| `src/services/` | Hall scanning, touch events, lighting, haptics, pedal input, calibration, expression mapping, current/power governance. |
| `src/midi/` | TinyUSB USB-MIDI/MPE, DIN MIDI in, polarity-selectable DIN MIDI out. |
| `src/usb_vendor/` | The custom USB vendor interface used by the companion app for remap/calibration/diagnostics/live-monitor traffic. Implements the protocol defined in `../shared/protocol/`. Kept separate from `midi/` so config traffic never competes with MIDI bandwidth. |
| `src/profiles/` | Runtime feature flags, power budgets, note/scale layouts, button bindings, visual/haptic themes. What the companion app edits and downloads to the device. |
| `src/diagnostics/` | Per-pad test commands, I2C enumeration, raw sensor streaming, fault reporting — exposed over `usb_vendor/`. |
| `src/storage/` | Versioned, CRC-protected, two-slot flash configuration (profiles + per-pad calibration). |

## Bring-up order

Per `docs/hardware/SENTIA_FIRMWARE_CODEX_START.md`: board constants + safe
boot + diagnostics first, then I2C discovery with outputs forced off, then
buttons/power-status/pedal, then Hall scanning, then touch, then USB MIDI,
then lighting, then haptics, then DIN MIDI, then CV/gate. Each phase must
leave the project building and the previous phase's safety guarantees
intact.

## Non-negotiables (see hardware docs for the full list)

- Never drive GP20 to a *high* level -- GP20 is the shared PCA9685 OE
  pin (see `board/board_pins.h`; the original hardware doc's "A5
  address strap" claim was wrong, corrected 2026-08-21 against the real
  fabricated board's flying-probe netlist). It's fine, and required, to
  drive it *low* to enable outputs -- `board_pca9685_enable_outputs()`
  does exactly that, but only once every channel is already configured.
- Only one Hall mux channel enabled across all three TCA9548A devices at a
  time.
- All LED mux banks disabled while changing selector bits.
- Both PCA9685 devices initialize all-off before any other output service
  starts.
- Never auto-select a USB power budget above 500mA.
- CV/gate stay off unless GP22 proves external power is selected.
- A failed subsystem disables itself; it never blocks USB diagnostics.

## Status

**Real hardware, not just simulated/reasoned-through.** As of
2026-08-21 this has been flashed to and run on an actual soldered Rev
A0 board: all 8 expected I2C devices ACK, underglow and all 24 pad LEDs
are lit white and touch-reactive, function buttons light while held,
and Hall gives live changing raw XYZ. USB MIDI has not yet had its
first on-hardware note-on test.

- `board/` — done: GPIO/I2C-address constants, the `PadConfig[24]`
  table (verified unique by `test/test_pad_config.c`), GPIO-safe-state
  + I2C-bus-init, plus `board_pca9685_enable_outputs()` (see
  non-negotiables above).
- `diagnostics/` — `i2c_scan` (expected-device scan, looped every 2s)
  and a full 0x08-0x77 bus scan (`tiles_diag_i2c_full_scan()`, not
  currently called from `main.c` -- it did its job finding the real
  PCA9685 addresses and is kept around for the next time something
  isn't where it's expected).
- `drivers/` — `sk6805`, `tca9554`, `tca9548a`, `tmag5273`, `pca9685`,
  `mpr121` all done and now verified against real silicon (register
  maps were datasheet-confirmed from the start; behavior confirmed
  on hardware 2026-08-21). Only `dac80502` (CV DAC) is not built.
- `services/` — `lighting`, `buttons`, `touch`, `hall` (raw XYZ + rest
  baseline + depth, touched-pad-priority scan) all done and
  hardware-verified. `pedal` done (sustain on by default, expression
  built but off) but not yet hardware-tested. `note_map` done: pad ->
  MIDI note layout confirmed against real hardware's stated pattern
  (pad 19 = lowest note, bottom-to-top/left-to-right), scale-mode
  architecture in place with only chromatic implemented, verified by
  `firmware/test/test_note_map.c`. `expression` done for V1: touch+Hall
  fusion deriving real velocity (from strike acceleration) and
  aftertouch (from press depth) -- see `services/README.md` for the
  state machine and the explicitly-unmeasured scaling constants.
  Everything else (haptics, power governance, per-pad Hall calibration)
  not built.
- `midi/` — composite USB CDC+MIDI device done (see `midi/README.md`);
  note on/off with real velocity, poly aftertouch, and CC (sustain/
  expression) all wired up, single MIDI channel. Not yet verified with
  a real MIDI-receiving host. DIN MIDI and MPE channel allocation not
  built.
- `usb_vendor/`, `profiles/`, `storage/` still empty module skeletons.

Builds clean end-to-end against a real pico-sdk checkout (`cmake` +
`arm-none-eabi-gcc`; see `BUILD.md`) with zero warnings, and produces a
flashable `.uf2`.

**Known gaps / open items:**
- USB MIDI hasn't had a real on-hardware note-on/off test in a DAW or
  MIDI monitor yet -- the composite descriptor builds and the device
  should enumerate, but "builds clean" and "a host actually receives
  the right notes" are different claims until checked.
- The LED brightness ceiling and idle-baseline/underglow percentages in
  `services/lighting.c` are engineering estimates (tuned once against
  real hardware for "too dim" feedback, but not measured against an
  actual current budget).
- `sk6805.pio`'s bit timing mirrors Raspberry Pi's reference `ws2812.pio`
  program; confirmed working on real SK6805 parts (all pads + underglow
  show correct white output on hardware), not independently verified on
  a scope.
- `hall.c` now prioritizes touched pads (scanned every call) over the
  background round-robin for untouched ones, and the main loop's
  throttle was cut from `sleep_ms(10)` to `sleep_ms(1)` specifically so
  `services/expression.c`'s strike-detection window gets more samples
  -- but the actual achieved sample rate for a touched pad is still
  unmeasured (depends on real I2C transaction timing, and on how many
  other pads are touched at once). This is a direction, not a verified
  number.
- No per-pad axis-selection or calibration curve exists yet -- Z is
  assumed to be the vertical-press axis for every pad (unverified per-
  pad), and `hall.c`'s depth is a raw, uncalibrated magnitude with no
  dead zone, offset correction, or saturation margin.
- `services/expression.c`'s acceleration->velocity scale, depth->
  aftertouch range, and strike-detection window durations are all
  explicitly-flagged placeholder constants -- there is no calibrated
  mT/LSB relationship to derive them from yet, so they're starting
  guesses that will need real-hardware tuning once this can actually be
  played and heard.
- MPR121 touch thresholds (12/6) are Freescale's generic quickstart
  defaults, not tuned for this board's actual electrode/keycap/acrylic
  stack. Touch works and feels responsive on hardware as of the
  latency fix in `services/lighting.c`, but sensitivity tuning is still
  open.
- No MPE (per-note channel allocation) yet -- V1 MIDI is single
  channel. Velocity and aftertouch are now real (Hall-derived), not
  fixed, but unverified against actual playing since USB MIDI itself
  hasn't been hardware-tested.
