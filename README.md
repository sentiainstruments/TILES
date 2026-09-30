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
- **`daw-integration/`** — Companion software that runs on the computer to
  let TILES's transport remote (diamond button) control a DAW's own
  transport directly, the way a factory-recognized controller does,
  instead of needing a manual per-button MIDI Map. Currently: an Ableton
  Live Control Surface script. See `daw-integration/README.md`.

## Core principle

There is exactly one canonical description of "what pad 7 is wired to" and
exactly one description of "what a REMAP command looks like on the wire."
Both live under `shared/`. Nothing downstream — firmware driver code,
companion-app UI, diagnostics — hand-rolls its own copy of either.

## Status

- **`firmware/`** — running on Rev A0 hardware, under active development.
  Current state: `firmware/README.md`; development history: the
  `HISTORY.md` files under `firmware/`.
- **`daw-integration/`** — the Ableton Live Control Surface script and
  the TILES DISPLAY Max for Live device, both in use.
- **`tools/`** — flash script, settings CLI (`tiles_control.py`) and the
  bootloader watchdog. Board-map codegen not started.
- **`shared/`** — `protocol/` documents the v1 text settings protocol the
  firmware implements. `board-map/` is not authored yet; the firmware's
  `board/pad_config.c` is kept by hand against `docs/hardware/`.
- **`companion-app/`** — planned (brief, package manifest, settings
  fixture); no app code yet.

## Before any unit leaves the building

TILES is pre-production and identifies itself with placeholders that are
only allowed for in-house testing (details, and how to replace each, in
`firmware/src/midi/product_identity.h`):

- **USB ID `1209:0001`** — pid.codes' shared *test* ID. Its rules forbid it
  on any device that's given out, sold or manufactured, beta units
  included. Replace it with a real product ID first: Raspberry Pi gives
  them out free for RP2350-based products (application form linked from
  github.com/raspberrypi/usb-pid). The ID lives in `product_identity.h`
  plus the host copies listed there (settings tool, flash script, Ableton
  script).
- **MIDI SysEx ID `0x7D`** — the MIDI Association's non-commercial/
  development ID. Fine for pre-production; a shipping product should
  register its own.
- **Windows** — driverless access to the settings interface (for the
  companion app) is implemented but has never been tried on a Windows
  machine.

## For agents

Start at `AGENTS.md` — it routes to the module-specific doc you actually
need instead of duplicating it here.
