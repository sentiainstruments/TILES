# drivers/

One small driver per chip on the board. Each knows its own register map
and protocol, nothing more: which pad or mux channel a chip serves lives
in `board/`, and what the data means lives in `services/`.

| Driver | Chip | Notes |
|---|---|---|
| `i2c_bus` | (all I2C chips) | Shared bounded transactions: 5 ms timeout, and bus recovery on any failure. Also closes a pico-sdk 2.3.0 read path that ignores its timeout. Every I2C driver goes through it. |
| `tca9548a` | Hall I2C mux | Generic 8-channel mux. The "one channel open across all three muxes" rule lives in `services/hall.c`. |
| `tmag5273` | 3-axis Hall sensor | Continuous mode, X/Y/Z, ±80 mT, raw counts. Calibration is not this driver's job. |
| `mpr121` | Capacitive touch | Freescale quickstart filtering, touch/release thresholds 12/9, 1 ms sample interval for latency. |
| `pca9685` | PWM: haptic motors + button LEDs | POR/init state is "full off" = pin low, which **lights** the active-low button LEDs; `services/buttons.c` turns them off right after init. |
| `tca9554` | GPIO expander | Only the pad-LED mux select (S0-S2) and the three active-low enables. |
| `sk6805` (+ `.pio`) | Addressable RGB | PIO one-wire, timing from Raspberry Pi's `ws2812.pio`. Bounded FIFO waits; `busy_wait_us` for the latch. |
| `dac80502` | CV DAC (SPI1) | Written from the datasheet; **not yet brought up on hardware**. |

Rules for this layer:

- Never block without a bound. Every I2C and PIO wait has a timeout,
  because an unbounded wait once froze the whole main loop.
- Register values cite the datasheet section or table they come from.
- `sk6805.c` is the one exception to "drivers don't include services":
  it emits crash-recorder trace marks from inside the pixel loop.
