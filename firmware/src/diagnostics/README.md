# diagnostics/

Bring-up and calibration tools, over the USB-CDC serial console
("SENTIA TILES Diagnostics"). They never depend on MIDI or other
higher-level services, so a failed sensor can't take them down.

| File | What it does |
|---|---|
| `i2c_scan` | At boot, probes every expected I2C0/I2C1 address and prints pass/fail per device. `tiles_diag_i2c_full_scan()` lists every address that answers. |
| `capture` | Records one pad's raw Hall readings (in the effects' memory, `audio/fx_memory.h`: TILES's reverb and delay are quiet from the first `CAPTURE START`/`WATCH` until `CAPTURE RELEASE`, which `tools/tiles_control.py` and the studio send when done) and what the expression code made of them (strokes, note-on with velocity and strike timing, pressure, bend, note-off with release velocity), per pass, into RAM (`CAPTURE` over the settings interface) for replaying on the computer; `CAPTURE WATCH` keeps streaming for the studio (`tools/studio/`). |
| `fault` | A hard fault or `panic()` on either core, or core 1 standing still for a second (core 0 rings it a doorbell to learn where it is stuck), is recorded in RAM a reset keeps and the chip restarts; the next boot reports it in INFO (`fault=`, `fault.callers=`, `fault.message=`), pulses the underglow red like a watchdog restart, sends all notes off, and copies the record to flash (its own 2-sector region, `storage/flash_map.h`) so it outlives unplugging: INFO `fault.saved=` (build, count since cleared) until `tiles_control.py fault clear`. Its restart is a `watchdog_reboot()`, which `watchdog_enable_caused_reboot()` doesn't count, so the rest of the boot is a fresh one (animation, I2C scan). |
| `boot_stage` | The last stage `main()` reached before a watchdog reset (INFO `boot.hang=`). |
| `chip` | What a clock above the RP2350's 150 MHz rating is qualified on: INFO `clk_khz`, `vreg_mv`, `temp_c`, and `FLASH CRC` (the program read back through the uncached window, compared with the build's `.bin` by `tiles_control.py flash crc <bin>`). The clock itself is a build setting, 230.4 MHz by default: `cmake -DTILES_CLK_KHZ=150000` for the rated speed (also 153600, and 268800 / 288000 as margin steps; `-DTILES_VREG=1_20` overrides the core voltage); each step sets the PLL, the core voltage and, above 160 MHz, the flash at a third of the clock (`src/CMakeLists.txt`). `main()` sets the build's exact core voltage first thing: the SDK only raises it, and a reset keeps the last one. Above 150 MHz `main()` moves the UART and SPI to the USB PLL's 48 MHz. |
| `calibration` | Single-key serial commands for Hall calibration: `r` recaptures the rest baseline, `m` prints each pad's strong-strike depth against it (`f`, a "regular press" snapshot, is not part of the procedure). |

## Calibration rule

Two values per pad: **rest** and **one strong strike**. A "regular"
press isn't repeatable (unit 2, pad 1: 33 for a normal press, 1697 for a
hard strike); "as hard as it goes" is. Sample a few pads (the corners 1,
6, 19, 24), not all 24.

Data so far:

- First unit, all 24 pads: full press measured 784-1184 raw depth,
  average 918, which was already the mechanical bottom-out. That set
  `expression.c`'s aftertouch full scale to 900 (now adjustable from the
  expression menu).
- Unit 2, pad 1: rest z=384, strong strike 1697. The other corners are
  still to do before trusting constants derived from unit 2.
- `TILES_STANDBY_HALL_WAKE_DEPTH` needs a light-touch data point that
  hasn't been captured, so Hall wake stays disabled.

Nothing is saved or applied automatically; the numbers are for picking
constants by hand. Per-pad stored calibration is future work (through
the companion app and `storage/`).
