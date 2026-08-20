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

Not yet authored — this is a placeholder pending designing the runtime
board-map schema and protocol together in a follow-up pass.
