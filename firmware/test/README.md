# test/

Host-buildable unit tests (no hardware required) for logic that can be
isolated from the Pico SDK runtime. Hardware-dependent code stays covered
by the `diagnostics/` manufacturing test commands instead.

- `test_pad_config.c` — asserts all 24 touch/Hall/LED/haptic/FPC routes
  in `board/pad_config.c` are unique, plus row/col/fpc sequencing and the
  LED mux-index→enable-port relationship. Required by
  `SENTIA_FIRMWARE_CODEX_START.md` before writing further implementation
  code. Run it: `./run.sh`.

Planned additions as those modules get built: scale/note mapping, voice
allocation, calibration math, `usb_vendor/` protocol framing.
