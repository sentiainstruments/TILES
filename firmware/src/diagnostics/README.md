# diagnostics/

Command handlers exposed over `usb_vendor/`: list I2C devices, read power
source/USB connection state, select one logical pad and stream raw or
calibrated data, set one pad LED, pulse one motor at bounded duration/duty,
read buttons/pedal, save/inspect/invalidate/erase calibration, run
manufacturing per-key tests. Never gated by whether MIDI or other
higher-level services are running — a failed sensor module must not take
diagnostics down with it.

## Status

- `i2c_scan.h`/`.c` — probes every expected I2C0/I2C1 device address and
  prints pass/fail per device. This is the "I2C discovery" bring-up
  phase. Currently printed over the temporary USB-CDC stdio enabled in
  `src/CMakeLists.txt`; rewire to `usb_vendor/` once that exists instead
  of adding a second output path.
- Everything else in this list (per-pad commands, calibration
  save/erase, manufacturing tests) is not built yet.
