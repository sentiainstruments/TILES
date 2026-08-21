# Architecture notes

Cross-cutting design notes that span firmware and companion-app rather
than belonging to one side — e.g. how on-device button-driven config
changes (a scale switch from a function button) and companion-app edits to
the same profile stay consistent, or how calibration data ownership is
split between device flash and app-side backups.

## Contents

- [`defaults-and-safeguards.md`](defaults-and-safeguards.md) — V1 sensing
  scope (pressure/depth axis only), CV/gate/MIDI-polarity/pedal-polarity/
  LED-brightness defaults, and the pad baseline drift-compensation
  trigger. The reference `firmware/src/board/` and `firmware/src/services/`
  get built against.

More notes will be added here as cross-cutting decisions get made.
