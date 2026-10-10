# firmware/

Raspberry Pi Pico 2 (RP2350) firmware for SENTIA TILES Rev A0, in C on
the Pico SDK with TinyUSB. Hardware truth for this revision lives in
`../docs/hardware/`; nothing here hard-codes a GPIO, I2C address or
channel that isn't in that board map.

Build and flash: `BUILD.md`. Working on this code (conventions, flashing
several boards, the feedback loop): `AGENTS.md`. Tests: `test/README.md`.

## Modules

| Module | Owns | Read |
|---|---|---|
| `src/board/` | Every pin, bus handle and the 24-pad routing table. Nothing else touches a raw pin or mux channel. | `src/board/README.md` |
| `src/drivers/` | One small driver per chip (TCA9548A, TMAG5273, MPR121, PCA9685, TCA9554, SK6805, DAC80502) plus the shared bounded I2C layer. | `src/drivers/README.md` |
| `src/services/` | Everything musical and interactive: sensing, expression/MPE, modes and menus, lighting, haptics, standby, games, CV/gate. | `src/services/README.md` |
| `src/midi/` | The USB device (MIDI on two ports, CDC console, vendor, reset), MIDI in/out, DIN MIDI, the MPE wire protocol, product identity. | `src/midi/README.md` |
| `src/usb_vendor/` | The text settings protocol for the companion app and scripts. | `src/usb_vendor/README.md` |
| `src/profiles/` | The settings table: every user-tunable value, saved to flash. | `src/profiles/README.md` |
| `src/storage/` | Power-cut-safe two-slot flash store and the flash region map. | `src/storage/README.md` |
| `src/diagnostics/` | Boot I2C scan and the serial Hall calibration commands. | `src/diagnostics/README.md` |
| `src/main.c` | Boot order and the main loop: one `scan()` per module per pass. | the file |

Each module with a long past also has a `HISTORY.md` (the development
log, with the hardware findings and feedback behind each change);
`HISTORY.md` in this directory is the older project-wide status log.

## Boot order

Per `../docs/hardware/SENTIA_FIRMWARE_CODEX_START.md`: safe GPIO levels
first, then I2C discovery with every output off, then the output
drivers configured before their outputs are enabled, then sensing, USB,
lighting, haptics, DIN MIDI and CV/gate. `main.c` follows it and says
why each step is where it is. A crash-recovery boot (watchdog) skips the
print-only steps and the boot animation.

## Non-negotiables

- GP20 is the shared PCA9685 output enable (active low), not an address
  strap. Drive it low only after every PCA9685 channel is configured;
  never drive it high.
- Only one Hall mux channel open across all three TCA9548As at a time.
- All pad-LED mux banks disabled while the select lines change.
- Both PCA9685s start all-off before any other output service.
- Never select a USB power budget above 500 mA automatically.
- CV/gate stay off unless GP22 shows external power (and CV/gate is
  explicitly enabled).
- A failed subsystem disables itself; it never blocks USB or the rest.
- No unbounded waits: every I2C, PIO and USB wait has a timeout, and no
  code prints periodically from the main loop (a blocked USB-CDC print
  stalls everything).

## Status (2026-10-04)

Four pre-production units (Rev A0), firmware 0.3.0 (`src/midi/product_identity.h`),
built with Arm GNU Toolchain 15.3 (native arm64). Each unit names itself "SENTIA TILES N" over
USB (`src/board/unit_id.h`), so several can play in one DAW session.

Set for now (compile-time switches, see `daw-integration/README.md`):

- Performance transport (`OP_TRANSPORT_SHIFT_STOP 1` in `services/op_mode.c`):
  diamond plays, hold records, circle + diamond stops; while Live plays,
  diamond alone does nothing. Circle + diamond's other jobs (Ableton stop-all,
  Song capture) are off meanwhile.
- Ableton mode: a plain press on an empty slot does nothing; circle +
  press records a new clip there.

- Played and working on hardware: melodic, chord, bass guitar,
  sequencer (4 lanes, pattern bank, capture), Ableton mode with the
  control surface script and TILES DISPLAY, MPE (and plain MIDI with MPE
  off), velocity, pressure, per-note pitch bend and vibrato, pedal and
  melodic harmonics, haptics, lighting, standby and deep sleep, games,
  settings saved to flash.
- **Song mode is beta**: built, not yet played on hardware.
- **Drum mode** (menu pad 3; bass guitar on pad 7 since 0.2.9): played on
  units 2 and 4 (0.2.8); 0.2.9-0.3.0 add pages, rolls and lag fixes. Colour schemes and custom scales (0.2.7) not
  yet played.
- Main loop: ~2.2-2.5 ms per pass (INFO `loop.*`, 2026-10-10, unit 2).
- DIN MIDI (TRS jacks) works on hardware (confirmed 2026-10-04).
- Supply current measured on USB (2026-10-06, `docs/hardware/current-measurements.md`):
  0.15 A idle, 0.34 A mashing every pad, ~0.42 A worst case at the USB LED
  ceiling (now 50%). TILES declares 500 mA to the USB host.
- Not yet verified on hardware: CV/gate (and the DAC80502 driver), the
  Windows WinUSB descriptors, an expression pedal.
- **Before any unit leaves the building**: replace the test USB ID (see
  `src/midi/product_identity.h` and the root `README.md` checklist).
- Not built: per-pad Hall calibration curves, the companion app's larger
  protocol (remap, guided calibration, live streaming, firmware update).
