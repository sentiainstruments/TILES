# AGENTS.md

SENTIA TILES: a 24-key multidimensional MIDI/MPE + CV performance
controller. This file orients any person or AI agent (Claude Code,
Antigravity, Codex, Cursor, ...) working in this repo. Keep it short —
follow its pointers to the doc that actually owns the detail you need
rather than duplicating that detail here.

## Team rules (every person, every agent)

Sentia (Mats2000) has the final say on what lands on `main`. The long
form of how the team works is `CONTRIBUTING.md`.

- **Never commit to `main`.** Work on a branch named `<you>/<topic>`,
  open a pull request, and wait for the checks and an owner's approval.
  GitHub refuses direct pushes to `main`.
- **Never force-push or delete someone else's branch.** To catch up,
  merge `main` into your branch (`git merge origin/main`); don't rebase
  anything you have pushed.
- **Stay in your folder** (owners below). A change in someone else's
  folder goes in its own pull request, and that folder's owner reviews it.
- **The parts meet only in `shared/`** (protocol, mapping file,
  footprint). Change a contract in its own pull request, never as a side
  effect of other work.
- **Write it down in the repo.** Anything the next person or agent must
  know goes in the nearest README/AGENTS.md or the pull request — never
  only in a chat or in one AI tool's private memory, which nobody else's
  tool can read.
- **No AI attribution.** No `Co-Authored-By:` trailer for an AI and no
  "Generated with ..." footer in pull requests, from any tool. GitHub
  shows the trailer as a commit-author badge; commits are the person's.
- **Name nothing outside Sentia in user-facing text** (manuals, guides,
  app UI) without Mats2000's OK. Describe a TILES feature in its own
  terms, never by comparison with another instrument or product.
- **Only Mats2000 publishes:** firmware releases and update kits,
  start.sentiainstruments.com, anything public.
- **No secrets** in the repo or issues, and none pasted into an AI.
  Everyone signs in to GitHub on their own machine.

## Who owns what

| Path | Owner (reviews changes) | What it is | Read this when |
|---|---|---|---|
| `firmware/` | Mats2000 | Pico 2 (RP2350) firmware, Pico SDK C/C++. | Touching firmware — start with `firmware/AGENTS.md`, not this file. |
| `companion-app/` | Samrepasky4, Mats2000 | Electron desktop configurator (remap/calibrate/diagnose over USB). | Touching the desktop app — see `companion-app/README.md`. |
| `manuals/` | sebastianruedac, Mats2000 | On-demand manuals for custom mappings, in the Sentia style. | Touching manuals — start with `manuals/AGENTS.md`. |
| `shared/` | Mats2000 (`mapping/`: also Samrepasky4) | The contracts: USB settings protocol, the mapping file, TILES' footprint; later the canonical board map. | Changing anything two parts must agree on. |
| `daw-integration/` | Mats2000 | Ableton Live Control Surface script and the TILES DISPLAY Max for Live device. | Changing how TILES talks to a DAW — see `daw-integration/README.md`. |
| `docs/hardware/` | Mats2000 | Ground truth for GPIO, bus addresses, and the pad routing table for Rev A0. | Anything touching a physical pin, channel, or address. |
| `docs/style/` | Mats2000 | How a Sentia manual looks and reads. | Making anything a TILES player reads. |
| `docs/architecture/`, `docs/protocol/` | Mats2000 | Cross-cutting design notes not owned by one module. | Rare — check before a big structural change. |
| `docs/reference/legacy-prototype-v1/` | Mats2000 | First prototype's Arduino sketch. Behavior reference only, none of it reused. | Looking for prior art on a feature (scale modes, voice stealing, standby animation). |
| `tools/` | Mats2000 | Flash script, settings CLI, bootloader watchdog (codegen planned). | Flashing, or scripting the settings protocol — see `tools/README.md`. |
| `.github/`, `AGENTS.md`, `CONTRIBUTING.md`, `.agent/` | Mats2000 | Team rules, code owners, checks. | Changing how the team works. |

`.github/CODEOWNERS` is the enforced version of this table.

## Conventions

- Prefer the README/AGENTS.md closest to what you're editing over this
  top-level file or general knowledge — module-local docs carry the
  actual design history and hardware constraints; this file only routes
  you to them.
