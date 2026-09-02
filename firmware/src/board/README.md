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
- `board_layout.h` — header-only. How the 6 function buttons + 24 pads +
  4 underglow pixels map onto one unified 5-row x 6-col logical grid
  (row 0 = buttons, rows 1-4 = the pad grid), for any module that treats
  the board as a single low-res animated display:
  `services/standby.c`'s idle animations and `services/boot_sequence.c`'s
  power-on animation both include it rather than each keeping their own
  copy of this mapping. The underglow anchor points are based on the
  user's verbal description of the physical board, not a hardware doc
  (confirmed absent from `docs/hardware/`).
- `unit_id.h` — header-only, `TILES_UNIT_NUMBER`/`TILES_UNIT_COUNT` --
  real feedback: "were moving to have identifiers." A human-assigned
  pre-production sequence number ("2 of 4"), edited by hand before each
  physical board's own build+flash -- not the RP2350's own unique
  silicon ID (`pico_get_unique_board_id()`, already used as the USB
  serial number in `midi/usb_descriptors.c`), which identifies a CHIP,
  not something you could read off a box. No persistent per-unit
  storage exists yet (`storage/` isn't built), so this is compile-time
  only -- reconfiguring isn't needed, it's a plain header, just edit and
  do a normal incremental build. Surfaced two places: the USB product
  string (`midi/usb_descriptors.c`'s `STRID_PRODUCT` case, so `picotool
  info -a` or the host OS's own USB device listing shows it without a
  serial terminal) and one `printf` at the very top of `main()`, for
  correlating a captured serial log back to a specific physical board.
