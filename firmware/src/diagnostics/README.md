# diagnostics/

Bring-up and calibration tools, over the USB-CDC serial console
("SENTIA TILES Diagnostics"). They never depend on MIDI or other
higher-level services, so a failed sensor can't take them down.

| File | What it does |
|---|---|
| `i2c_scan` | At boot, probes every expected I2C0/I2C1 address and prints pass/fail per device. `tiles_diag_i2c_full_scan()` lists every address that answers. |
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
