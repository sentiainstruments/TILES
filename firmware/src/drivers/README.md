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
- `mpr121`, `pca9685`, `dac80502` — not built yet.
