# Architecture notes

Design notes that span several parts of the project rather than
belonging to one module.

- [`defaults-and-safeguards.md`](defaults-and-safeguards.md): what the
  sensors drive, the power safeguards and per-mode budgets, LED
  brightness, CV/gate, DIN and pedal polarity, and Hall baseline drift
  compensation. Firmware headers link to its sections.
- [`control-software.md`](control-software.md): how the companion app
  talks to the device (USB vendor interface, the settings table,
  layouts as portable files), what's built, the next steps, and the app
  stack choice.
- [`content-packs.md`](content-packs.md): how sold packs reach a device --
  data content (scales, later layouts) pushed into the content store, code
  content (modes, games) unlocked per unit by signed entitlements -- and
  what's built.

Earlier versions of these notes (the pre-implementation plans) are in
git history: `git log -p docs/architecture/`.
