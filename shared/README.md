# shared/

The single source of truth that `firmware/` and `companion-app/` both
consume, instead of each keeping its own copy of hardware or protocol
knowledge.

- **`board-map/`** — the canonical 24-pad table (which touch electrode,
  Hall mux/channel, LED mux/channel, haptic PWM channel, and FPC number
  each logical pad uses), extending `docs/hardware/sentia_tiles_board_map_v1.json`
  with whatever the runtime/config layer needs (default note/scale
  assignment, calibration slot references, etc.).
- **`protocol/`** — the USB vendor interface wire protocol: command/
  response message IDs, packet framing, versioning, and the calibration/
  diagnostics/remap message definitions.

Both are meant to be code-generated from (`tools/`) into the firmware's
`board/pad_config.*` and the companion app's `src/shared/` TypeScript
types, so there is exactly one place each fact is authored.

## Status

- `protocol/`: v1, a plain-text settings protocol, implemented in
  `firmware/src/usb_vendor/` and used by `tools/tiles_control.py`. The
  larger binary protocol (remap, calibration, streaming, firmware
  update) is still at the design-notes stage (`docs/protocol/`).
- `board-map/`: not authored yet. No codegen exists; the firmware's
  `board/pad_config.c` is kept in sync with
  `docs/hardware/sentia_tiles_board_map_v1.json` by hand and checked by
  `firmware/test/test_pad_config.c`.
