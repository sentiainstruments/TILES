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

Not yet implemented. This is the module skeleton; `CMakeLists.txt` and
`src/main.c` are placeholders pending the board/ design pass.
