# manuals/AGENTS.md

On-demand manuals for custom TILES mappings, in the Sentia style. Owner:
sebastianruedac; Mats2000 approves every change. The team rules in the
root `AGENTS.md` and `CONTRIBUTING.md` apply here too.

## Inputs — only these

- **The mapping:** the file format in `shared/mapping/`. It isn't frozen
  yet; agree on it there with Sam first, don't invent a private one here.
- **The look:** `docs/style/manuals.md`.
- **TILES' face:** `shared/footprint/`. Never redraw keys or pads.
- **What modes and keys do:** the full manual
  (start.sentiainstruments.com/manual/) and `firmware/README.md`. When
  they don't answer it, ask Mats2000 in the pull request or an issue —
  don't guess firmware behaviour.

## Rules

- Work only in `manuals/`. Changes to `shared/mapping/` go in their own
  pull request; never edit `firmware/`, `companion-app/` or `tools/`.
- Never talk to a TILES over USB from here; the companion app does that.
- Never publish anything; Mats2000 publishes.
- Exo 2 and Sora only, magenta only for press/lit, drawings only from
  the footprint, no drawn fingers.
- As soon as there is code, add tests and a job in
  `.github/workflows/ci.yml` in the same pull request.
