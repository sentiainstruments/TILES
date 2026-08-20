# SENTIA TILES — Code Monorepo

24-key multidimensional MIDI/MPE + CV performance controller. This repo holds
the Raspberry Pi Pico 2 firmware, the companion desktop configurator app, and
the shared definitions that keep them in sync.

## Layout

- **`firmware/`** — Pico 2 firmware (Pico SDK C/C++ + TinyUSB). Modular by
  subsystem: `board/`, `drivers/`, `services/`, `midi/`, `usb_vendor/`,
  `profiles/`, `diagnostics/`, `storage/`. See `firmware/README.md`.
- **`companion-app/`** — Self-contained desktop configurator (Electron), in
  the spirit of ROLI Dashboard / Roland editors. Remaps pads, edits scales
  and profiles, runs calibration, and streams live diagnostics over USB.
  See `companion-app/README.md`.
- **`shared/`** — The single source of truth for anything both sides must
  agree on: the canonical 24-pad board map and the USB vendor protocol
  definition. Firmware and companion app both consume this instead of
  duplicating hardware/protocol knowledge.
- **`docs/hardware/`** — Canonical hardware authority for Rev A0: the
  firmware handoff doc, the machine-readable board map, and the firmware
  bring-up guide. Treat these as ground truth for GPIO, bus addresses, and
  the 24-pad routing table.
- **`docs/reference/legacy-prototype-v1/`** — The first prototype's Arduino
  sketch (16-pad, Teensy-class board). Kept for feature/behavior reference
  only (scale modes, voice stealing, standby animation, haptic confirm
  clicks) — none of this code is reused. The Pico 2 architecture, pad count,
  and I/O are entirely different.
- **`docs/protocol/`** — Design notes for the USB vendor config/diagnostics
  protocol, ahead of it being formalized in `shared/protocol/`.
- **`docs/architecture/`** — Cross-cutting system design notes that don't
  belong to firmware or companion-app alone.
- **`tools/`** — Codegen and build helper scripts (e.g. board-map JSON →
  generated `PadConfig[24]` C header + companion-app TypeScript types).

## Core principle

There is exactly one canonical description of "what pad 7 is wired to" and
exactly one description of "what a REMAP command looks like on the wire."
Both live under `shared/`. Nothing downstream — firmware driver code,
companion-app UI, diagnostics — hand-rolls its own copy of either.

## Status

Scaffolding only. Modules currently contain placeholder READMEs describing
intent; implementation starts once each piece is designed in conversation.
