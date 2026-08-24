# drivers/

Small, independently testable drivers for each chip on the board. Each
driver knows its own register map and protocol; it does not know about
logical pads, muxes' *meaning*, or musical semantics — that translation
lives in `board/` (which mux/channel a pad uses) and `services/` (what to
do with the data).

Planned drivers: `tca9548a` (Hall I2C mux), `tmag5273` (3-axis Hall
sensor), `mpr121` (capacitive touch), `pca9685` (PWM: motors + button
LEDs), `tca9554` (LED mux selector/enable), `sk6805` (addressable RGB
bit-banged over PIO), `dac80502` (CV DAC).

## Status

- `sk6805.h`/`.c` (+ `sk6805.pio`) — done. PIO-based one-wire driver,
  timing mirrors Raspberry Pi's reference `ws2812.pio` (same protocol
  family, 800kb/s NZR). Generic pixel writer; knows nothing about which
  physical LED it's driving — see `services/lighting.c` for that.
- `tca9554.h`/`.c` — done, scoped specifically to the pad-LED mux
  control use (S0-S2 select + three active-low enables), not a general
  8-bit-expander driver.
- `tca9548a.h`/`.c` — done. Generic 8-channel I2C mux driver (one
  control-register byte); the three chips' instances and the "only one
  channel open across all three at once" policy live in
  `services/hall.c`, not here.
- `tmag5273.h`/`.c` — done for V1: continuous-measure mode, X/Y/Z
  enabled, ±80mT range on every axis, raw (uncalibrated) 16-bit reads.
  Register map transcribed directly from the TI datasheet (local copy),
  not from memory — see the file header. No gain/offset/threshold/angle
  features configured; that's the calibration layer, not built yet.
- `pca9685.h`/`.c` — done. Register map and the "full on"/"full off"
  bit behavior confirmed against the real NXP datasheet (Rev 4), fetched
  and read directly. Important, datasheet-confirmed fact baked into the
  driver: the chip's own power-on/init "every channel off" state drives
  every pin LOW, which is correct for this board's active-high motor
  channels but actually *lights* the active-low-wired function-button
  LEDs — `services/buttons.c` corrects those specific channels
  immediately after init. See the file header for the full reasoning.
- `mpr121.h`/`.c` — done. Register map and init sequence (soft reset,
  baseline filter, per-electrode touch/release thresholds, Run Mode)
  confirmed against the real Freescale/NXP datasheet. Threshold values
  (12/6) are Freescale's own published quickstart defaults, not final
  per-key tuning — real thresholds still need calibration once the
  enclosure/keycaps are assembled, per the hardware handoff. One
  deviation from the quickstart defaults, for latency: ESI (electrode
  sample interval) is set to 1ms instead of Freescale's 16ms default --
  the chip's own internal sample interval is a real latency floor no
  amount of firmware polling can beat, and 16ms alone was a meaningful
  chunk of end-to-end touch latency. Verified safe given our FFI/CDT
  settings (actual scan time ~72us, well under 1ms) rather than silently
  overridden by scan time -- see the comment in `mpr121.c`. Tradeoff:
  less noise averaging; revisit if touch gets jittery on the real
  assembly.
- `dac80502` — not built yet.
