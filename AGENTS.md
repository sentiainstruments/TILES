# AGENTS.md

SENTIA TILES: a 24-key multidimensional MIDI/MPE + CV performance
controller. This file orients any agent working in this repo. Keep it
short — follow its pointers to the doc that actually owns the detail you
need rather than duplicating that detail here.

## Layout

| Path | What it is | Read this when |
|---|---|---|
| `firmware/` | Pico 2 (RP2350) firmware, Pico SDK C/C++. Where nearly all active development happens right now. | Touching firmware — start with `firmware/AGENTS.md`, not this file. |
| `companion-app/` | Electron desktop configurator (remap/calibrate/diagnose over USB). | Touching the desktop app — see `companion-app/README.md`. |
| `shared/` | Canonical 24-pad board map + USB vendor protocol definition, consumed by both firmware and companion-app. | Changing anything both sides must agree on. |
| `docs/hardware/` | Ground truth for GPIO, bus addresses, and the pad routing table for Rev A0. | Anything touching a physical pin, channel, or address. |
| `docs/architecture/`, `docs/protocol/` | Cross-cutting design notes not owned by one module. | Rare — check before a big structural change. |
| `docs/reference/legacy-prototype-v1/` | First prototype's Arduino sketch. Behavior reference only, none of it reused. | Looking for prior art on a feature (scale modes, voice stealing, standby animation). |
| `tools/` | Codegen: board-map JSON -> generated C header + companion-app TS types. | Changing the board map or protocol. |

## Repo-wide conventions

- **Never add a `Co-Authored-By: Claude` trailer to commits, or a
  "Generated with Claude Code" (or similar) footer to PR descriptions, in
  this repo.** GitHub renders the trailer as a visible commit-author
  badge, which reads as "Claude pushed this" rather than "a tool was
  used to help" — flagged directly by the repo owner after it shipped
  once. Applies everywhere in this repo, not just firmware.
- Prefer the README/AGENTS.md closest to what you're editing over this
  top-level file or general knowledge — module-local docs carry the
  actual design history and hardware constraints; this file only routes
  you to them.
