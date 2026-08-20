# diagnostics/

Command handlers exposed over `usb_vendor/`: list I2C devices, read power
source/USB connection state, select one logical pad and stream raw or
calibrated data, set one pad LED, pulse one motor at bounded duration/duty,
read buttons/pedal, save/inspect/invalidate/erase calibration, run
manufacturing per-key tests. Never gated by whether MIDI or other
higher-level services are running — a failed sensor module must not take
diagnostics down with it.
