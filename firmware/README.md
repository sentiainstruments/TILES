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

- Never drive GP20 (PCA9685 address strap).
- Only one Hall mux channel enabled across all three TCA9548A devices at a
  time.
- All LED mux banks disabled while changing selector bits.
- Both PCA9685 devices initialize all-off before any other output service
  starts.
- Never auto-select a USB power budget above 500mA.
- CV/gate stay off unless GP22 proves external power is selected.
- A failed subsystem disables itself; it never blocks USB diagnostics.

## Status

- `board/` — done for this phase: GPIO/I2C-address constants, the
  `PadConfig[24]` table (verified unique by `test/test_pad_config.c`),
  GPIO-safe-state + I2C-bus-init.
- `diagnostics/` — I2C bus discovery (`i2c_scan`), looped in `main.c`
  every 2s over the temporary USB-CDC stdio.
- `drivers/` — `sk6805` (PIO one-wire RGB), `tca9554` (LED mux control),
  `tca9548a` (Hall I2C mux), `tmag5273` (3-axis Hall sensor, V1: raw
  reads only), `pca9685` (PWM/LED, button LEDs + future motors), and
  `mpr121` (capacitive touch) are done; only `dac80502` (CV DAC) is not
  built.
- `services/` — `lighting` (V1 default: solid white, underglow always
  on at idle baseline, pads at idle baseline brightening to the ceiling
  when touched), `buttons` (debounced, LED lit while held), `touch`
  (drives lighting brightness), and `hall` (V1: round-robin raw XYZ scan
  of all 24 pads) are done; everything else is not built.
- `midi/`, `usb_vendor/`, `profiles/`, `storage/` are still empty
  module skeletons.

Builds clean end-to-end against a real pico-sdk checkout (`cmake` +
`arm-none-eabi-gcc`; see `BUILD.md`) with zero warnings, and produces a
flashable `.uf2`. Not yet flashed to real hardware — see the open items
below.

**Known gaps to close before flashing to real hardware:**
- The LED brightness ceiling and idle-baseline percentages in
  `services/lighting.c` are the estimates from
  `docs/architecture/defaults-and-safeguards.md`, not measured values.
- `sk6805.pio`'s bit timing mirrors Raspberry Pi's reference `ws2812.pio`
  program but has only been verified by `pioasm` (syntax), not on a
  scope/logic analyzer against real SK6805 parts.
- `tiles_lighting_init()`'s return value is checked in `main.c` (prints
  a failure message) but nothing yet falls back to a safe/degraded
  lighting state on failure.
- `tmag5273.c`'s register configuration is transcribed from the real TI
  datasheet (not guessed), but has never talked to a real TMAG5273 --
  the identify/init/read sequence is unverified against actual silicon.
- `hall.c`'s scan rate is whatever the main loop's `sleep_ms(10)` and
  round-robin happen to produce -- not measured, and almost certainly
  well under the 120Hz full-sweep target, since nothing budgets loop
  timing yet. Revisit once Hall + touch + lighting are all integrated.
- No axis-selection or calibration logic exists yet -- `hall.c` reports
  raw X/Y/Z; deciding which axis is vertical press depth per pad, and
  turning raw counts into calibrated engineering values, is the next
  layer.
- `pca9685.c`'s register map and the "full on"/"full off" bit behavior
  are confirmed against the real NXP datasheet, and `mpr121.c`'s
  against the real Freescale/NXP datasheet -- but neither has talked to
  real silicon yet.
- **PCA9685 power-on characteristic to expect, not a bug:** the chip's
  own reset/init state drives every pin low, which briefly lights the
  active-low-wired function-button LEDs before `services/buttons.c`'s
  post-init correction runs (a handful of I2C writes after
  `tiles_pca9685_init()` returns). Expect a very brief flash of all 6
  button LEDs right at boot before they settle dark. Cosmetic only --
  well within the LED current budget, and does not affect the motor
  channels on the same chips (their "off" state is correct from the
  first write).
- MPR121 touch thresholds (12/6) are Freescale's generic quickstart
  defaults, not tuned for this board's actual electrode/keycap/acrylic
  stack -- expect touch sensitivity to need real calibration once the
  full assembly exists, per the hardware handoff.
- I2C now correctly raises to 400kHz after the initial discovery scan
  (`board_i2c_set_run_speed()`, called from `main.c`) -- this was
  previously wired up in `board_init.h` but never actually called;
  fixed during this review pass.
