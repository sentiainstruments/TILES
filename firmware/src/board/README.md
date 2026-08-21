# board/

Owns every raw GPIO, I2C/SPI bus handle, and PIO resource, plus the
canonical `PadConfig[24]` struct array generated/derived from
`../../../shared/board-map/`. Every other module addresses hardware only
through logical pad IDs (1–24) or named board functions this module
exposes — never a raw pin number or mux channel.

Contents so far:

- `board_pins.h` — every GPIO, I2C address, and bus/device count as a
  named constant. Transcribed from `docs/hardware/`, not re-derived.
- `pad_config.h`/`.c` — the 24-pad `tiles_pad_config_t` table and
  `board_pad_config(logical_pad)` accessor. Verified against
  `firmware/test/test_pad_config.c` (all touch/Hall/LED/haptic/FPC
  routes unique).
- `board_init.h`/`.c` — GPIO safe-boot states and I2C bus init at
  100kHz. This is step 1 (and the bus half of step 3) of the boot order
  in `SENTIA_FIRMWARE_CODEX_START.md`. It does not touch any I2C device
  — that's `drivers/`, not built yet.
