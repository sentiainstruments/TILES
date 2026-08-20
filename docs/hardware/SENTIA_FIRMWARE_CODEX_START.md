# Start the SENTIA TILES Pico 2 firmware

Use these two files as the only hardware authority for the pre-production Rev A0 board:

- `SENTIA_TILES_FIRMWARE_HANDOFF.md`
- `sentia_tiles_board_map_v1.json`

Build with the Raspberry Pi Pico SDK C/C++ and TinyUSB. Keep the code modular and safe to bring up one subsystem at a time.

Required architecture:

1. `board/` owns every GPIO, bus, PIO resource and the canonical 24-pad table. No other module may contain raw GPIO numbers or physical controller channels.
2. `drivers/` contains small, independently testable drivers for TCA9548A, TMAG5273, MPR121, PCA9685, TCA9554, SK6805 and DAC80502.
3. `services/` contains Hall scanning, touch events, lighting, haptics, power/current governance, pedal input and calibration.
4. `midi/` contains TinyUSB MIDI/MPE, DIN MIDI input, and polarity-selectable DIN MIDI output.
5. `profiles/` contains runtime feature flags, power limits, button bindings and note layouts.
6. `diagnostics/` exposes a USB command interface that can enumerate buses, read power state, stream one pad's raw data, set one LED, pulse one motor at bounded duty and save/erase calibration.
7. Configuration/calibration uses versioned, CRC-protected, two-slot flash storage.

Implement in safe phases and keep the project building after each phase:

1. Board constants, logging, watchdog, USB diagnostics and SAFE_BRINGUP profile.
2. I2C discovery with every output forced off.
3. Buttons, GP22 power status and pedal ADC.
4. One Hall sensor at a time, then the canonical 24-pad scan and raw calibration stream.
5. MPR121 IRQ and logical touch events.
6. USB MIDI, then MPE channel allocation.
7. One pad LED, underglow, then the full multiplexed LED service with current caps.
8. One haptic at low duty, then the current governor and external-power profile.
9. DIN MIDI input/output.
10. External-power-only CV and gate.

Hard safety requirements:

- Never drive GP20; it is a PCA9685 address strap.
- Close all Hall mux channels before selecting one sensor.
- Keep all LED mux banks disabled while changing selectors.
- Initialize both PCA9685 devices to all-off before enabling any other output service.
- Never auto-select a USB budget above 500mA; larger USB profiles are explicit validated-source overrides.
- CV and gate fail off unless GP22 proves external IN2 is selected.
- A failed subsystem stays disabled while USB diagnostics remains operational.

Before writing implementation code, parse `sentia_tiles_board_map_v1.json`, generate or define one strongly typed `PadConfig[24]`, and add tests that assert all 24 touch, Hall, LED, haptic and FPC routes are unique.
