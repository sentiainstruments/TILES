# board/

Owns every raw pin, bus handle and the pad routing table. Everything
else addresses hardware through logical pad ids (1-24) or the named
functions here, never a raw pin number or mux channel.

| File | What it is |
|---|---|
| `board_pins.h` | Every GPIO, I2C address and device count as a named constant, transcribed from `docs/hardware/`. |
| `pad_config.h`/`.c` | The 24-pad `tiles_pad_config_t` table and `board_pad_config(logical_pad)`. `test/test_pad_config.c` checks every route is unique. |
| `board_init.h`/`.c` | GPIO safe boot levels, I2C bus init (100 kHz discovery, 400 kHz run), I2C bus recovery, PCA9685 output enable. |
| `board_layout.h` | The board as one 5x6 grid (button row + 4 pad rows + underglow anchors) for animations, plus the button ids by role. |
| `unit_id.h` | Hand-set pre-production unit number (`TILES_UNIT_NUMBER`/`_COUNT`). Edit before building for a specific board. |

Hardware facts that bit us, kept here so they aren't lost:

- GP20 is the PCA9685s' shared **OE**, not an address strap (the
  original handoff doc was wrong). The PCA9685 addresses are 0x40/0x41.
- Enabling OE makes every PCA9685 register live immediately, so it
  happens only after all channels are configured.
