# companion-app/

Self-contained desktop configurator (Electron + TypeScript), in the spirit
of ROLI Dashboard / Roland's hardware editors — installed, branded,
offline-capable, not a browser tab. Talks to the device over the USB
vendor interface defined in `../shared/protocol/`.

## Structure

- `src/main/` — Electron main process. Owns the USB device connection
  (device discovery, connect/disconnect, the protocol client), file I/O
  for saved presets, and anything needing OS/native access. The renderer
  never touches USB directly — it only talks to main over IPC.
- `src/renderer/` — The UI (React/TS): pad-mapping grid, per-pad
  calibration wizard + live sensor view, scale/note layout editor, scene
  and profile management, power/haptic budget indicators, firmware update
  flow.
- `src/shared/` — TypeScript types generated from (or hand-kept in sync
  with) `../shared/protocol/` and `../shared/board-map/`. Anything the
  main and renderer processes both need to agree on about wire format or
  pad identity lives here, not duplicated in each.

## Planned feature areas

- Pad remap: note/scale assignment, MPE channel behavior, per-axis
  gesture-to-CC/parameter mapping.
- Calibration: guided per-pad rest/half/bottom/tilt capture, live raw XYZ
  monitor, saturation warnings.
- Profiles/scenes: edit and switch the same profile concept the device
  itself uses (see `firmware/src/profiles/`), so on-device button toggles
  and app edits stay consistent.
- Diagnostics: I2C enumeration, per-key test, power-state readout —
  surfaces the same commands `firmware/src/diagnostics/` exposes.
- Firmware update.
- **DAW integration installer**: automate the manual install step
  `../daw-integration/README.md` currently documents for TILES's
  transport remote (diamond play/stop/record) -- detect installed DAWs
  and copy the matching script (e.g. `../daw-integration/ableton/TILES/`)
  into the right Remote Scripts folder with one click, instead of the
  user navigating there by hand. Real feedback that prompted the whole
  DAW-integration feature: "i dont want to map manually, this should
  just work like it does for launchkey out of the box" -- this app-level
  installer is the next step toward that, not full parity with it: it
  can safely automate the file-copy, but NOT Ableton's own Preferences
  dropdown selection (that setting lives in Ableton's private,
  undocumented preferences file -- writing to it directly would mean
  reverse-engineering a format that can change between Ableton versions,
  with real risk of corrupting unrelated settings if gotten wrong). Gets
  users from "copy a folder to a hidden path" down to "click one button,
  then pick TILES from a dropdown once," not to zero steps.

## Status

Not yet scaffolded as a buildable Electron project. `package.json` below
is a placeholder pending confirming the exact stack (Electron + Vite +
React + TypeScript is the working assumption).
