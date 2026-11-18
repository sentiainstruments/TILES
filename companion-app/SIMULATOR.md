# Companion app simulator

Start from `companion-app/` with `TILES_MOCK=1 npm run dev` (macOS/Linux).
The simulator runs without any physical instrument.

The mock USB command handler is in `src/main/mockUsb.ts`. The fixture
(`fixtures/tiles-settings.json`) contains 54 settings from firmware 0.3.6,
plus 21 settings from the official `firmware/board-bringup` protocol
at commit `a52f515fffa0c025c4d7900096920cbb309bc8ea` (Alpha 6.5 on 2026-10-28).
This is a **simulation**, not a physical instrument or a real firmware build.

- The default firmware is Alpha 6.5, exposing 75 settings, including
  layout selection, 12 lighting effects, MIDI routing, 13 mode-menu toggles,
  and 9 editable custom scales. 0.3.6 remains selectable with its original
  54 settings and the same custom-scale commands.
- RGB schemes and 24 per-pad overrides are supported in both 0.3.6 and Alpha.
- The header's simulator-only firmware selector can also switch to 0.3.6
  or legacy 0.2.0; this resets the mock device. `SIM VERSION` is
  intentionally *not* a real firmware command.
- `GET`, `SET`, `LIST`, `SCHEMA`, `INFO`, `RESET`, `SAVE`,
  `SCALES`, `SCALE GET/PUT/DELETE`, and `CONTENT LIST/CLEAR` are
  interpreted locally. Responses use the same format as the renderer's USB
  adapter: multiline terminal `OK` is removed.
- The simulator validates documented setting ranges, values, RGB colors,
  and custom scale slot/interval syntax.
- `TEST` mode is only acknowledged for a subset of commands; no real
  electrical/haptic/MIDI effect can be reproduced.
- Simulator writes remain in memory until the Electron main process ends.
  Physical-device persistence and USB transfers are **not** tested.
- The visualizer approximates how role and pad colors change with a chosen
  scale. Its scale dropdown is **preview only**: the current USB protocol
  uploads scales but does not offer a command to select an active scale.

The real USB path (`src/main/usb.ts`) remains separate from this simulator.
For verification on an actual board, compare `INFO`, `SCHEMA`,
`LIST`, `SCALES`, and a `SCALE PUT/GET/DELETE` round trip.

## Updating from upstream

The companion app remains on `sam-branch`; the source-of-truth firmware
branch is `sentiainstruments/TILES:firmware/board-bringup`. Do **not**
merge the full firmware branch into the UI branch to access its protocol.
Instead, compare `shared/protocol/README.md` and
`firmware/src/profiles/settings_table.c`, update this fixture and the UI,
and retain compatibility checks against 0.3.6.

Alpha 6.5 additions are controlled by a SCHEMA-discovered page, so the
same UI hides unsupported controls on older hardware. The preview can
show changes in total LED intensity and labels for layout/effect; it
**does not yet faithfully render** other note layouts, animated effects,
pressure-based lighting or real haptics.

The firmware's newer optional request-tag protocol (since 0.3.7) is
not yet implemented in the physical USB adapter; the current adapter
serializes commands without tags. Hardware testing should verify this
before shipping.
