# Current measurements

Supply current of a TILES Rev A0 unit, measured so the firmware's power
budgets (`firmware/src/services/power.c`, `lighting.c`) rest on numbers
instead of datasheet estimates.

## Setup (2026-10-06)

- Unit 4, firmware 0.2.6, on USB only (no 12 V), through an inline USB-C
  meter to a MacBook Pro. Settings shell reported `power=usb_only`.
- States set with the settings shell's bench tests (`TEST LEDS`, `TEST
  MOTORS`), stepped by `tools/current_test.py`; the meter read by hand, so
  each value is the meter's displayed (averaged) current.
- Bus voltage stayed at 5.21-5.25 V throughout.

## Readings

| State | Current |
|---|---|
| Normal idle, melodic mode | 0.15 A |
| Screensaver | under 0.20 A |
| Mashing all pads, normal play (USB cap 37%) | 0.34 A |
| Board only, LEDs and motors off | 0.09 A |
| All 28 LEDs white at 10% / 25% / 37% / 50% / 75% / 100% | 0.12 / 0.18 / 0.23 / 0.29 / 0.39 / 0.49 A |
| 1 motor at 100% duty | 0.12 A (+0.03) |
| 4 motors at 100% / 60% duty | 0.20-0.21 / 0.17 A |
| All LEDs white at 37% + 4 motors at 100% | 0.35 A (re-check, read after 3 s) |

A first pass read 0.53 A for the last row; the re-check (each state held
3 s before reading) gave 0.35 A, which matches the parts added up, so the
first value caught the motors spinning up.

## What it means

- **Board** (RP2350, 24 Hall sensors, touch, muxes, PWM ICs, button LEDs):
  ~80 mA, the LED line's intercept. The datasheet estimate was ~220 mA.
- **LEDs**: ~4.1 mA per percent of brightness with all 28 pixels white,
  ~0.41 A at 100%, ~14.6 mA per pixel (the board map's model said 16).
- **Motors**: ~28 mA each running at full duty, ~0.11 A for four (the
  estimate was 60-100 mA each). Start-up surges are brief and an averaging
  meter doesn't show them.

Decisions taken from this:

- USB-only pad ceiling 37% -> **50%**. Worst case at 50% (every LED white,
  underglow at its fixed level, 4 motors): 80 + 205 + ~24 + 110 = **~0.42 A**
  of the 500 mA budget. Normal play is far below that.
- USB configuration descriptor: **500 mA** (was 100 mA; TILES draws
  ~0.15 A idle).
- 4 haptic voices on USB kept. External power keeps 90% and 12 voices
  (~0.6 A worst case of 2.5 A).

## Not measured yet

- Motor start-up (stall) current: needs a peak-holding meter or a scope on
  one motor through its flex (the handoff's FH34 contact limit is 0.35 A).
- Current from the 12 V input (the USB side should read near zero with 12 V
  plugged in, since the TPS2121 selects external).
- Other units: unit 4 only so far.
- Before USB configuration the spec allows 100 mA; TILES lights its LEDs
  at boot (~0.15 A) before the host configures it. No host has objected,
  but a strict bus-powered hub could.
