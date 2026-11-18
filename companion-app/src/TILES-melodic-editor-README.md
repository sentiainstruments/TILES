# TILES — Melodic Mode Editor

## Install

Copy these files into `TILES/companion-app/src/`:

- `TilesVisualizer.tsx` — replaces the earlier mode preview
- `TilesVisualizer.css` — replaces the earlier visualizer styles
- `MelodicEditor.tsx` — new contextual device settings editor
- `MelodicEditor.css` — new editor styles
- `melodicNoteMap.ts` — melodic note layout preview calculations
- `SchemaSettings.tsx` — optional update to the previously supplied global schema screen; keeps its displayed values synchronized with edits elsewhere

No `App.tsx` changes are needed if it already imports and renders `<TilesVisualizer />`.

## Run the mock board

```bash
cd ~/TILES/companion-app
TILES_MOCK=1 npm run dev
```

Open **Device Settings**. In the instrument, click **△** to open the mode menu and click **Melodic** in the first pad (or close the menu if Melodic is already selected). The Melodic editor appears beside the visualizer on wide windows, below it on narrower windows.

## Available controls

Controls are discovered from firmware `SCHEMA` and initialized with `LIST`. The editor displays only keys actually returned by the connected firmware. In particular:

- `layout.melodic`: choice of note arrangement
- `color.root`, `color.third`, `color.fifth`, `color.note`, `color.accidental`: role colors
- `color.custom_pads` and `color.pad.01`–`color.pad.24`: per-pad override colors
- `look.*`: lighting intensity and lighting effects
- `expression.*`: MPE, pressure, and bend parameters (some affect multiple modes)
- `features.melodic_harmonics`, `features.harmonics.*`: harmonics

Click a pad on the visualizer to select it, then edit that pad's override color in the panel. Enable **Custom Pads** first for overrides to show; `none` inherits the role color. **Animate selected pad** is an optional local preview, disabled by default so you can see the actual edited idle color.

Number inputs commit on blur or Enter; sliders commit on release. Color input changes are debounced (~300ms). Every edit uses `SET`, reads back via `GET`, and broadcasts a `tiles:setting-changed` event to update the preview. **Reset** uses firmware `RESET`, then `GET`. **Refresh** rereads `SCHEMA`, `LIST`, and `INFO`. There is no extra save button: firmware handles flash persistence. The mock board stores edits only in memory for the current session.

## Notes on accuracy and scope

- The melodic grid note arrangement preview is derived from the official `firmware/src/services/note_map.c` for Rows, Columns, Fourths, Keys, Fifths, Tonnetz, and Quarter Tones.
- Firmware does *not* currently expose the full active key/scale/octave state through the settings protocol. The preview defaults to C and uses its own octave controls and preview scale selector; these do **not** program the instrument. Custom scales can still be uploaded using the existing CustomScales feature.
- The mode selector and top-button interactions remain **local preview only**. They do not switch actual hardware modes through the normal USB settings protocol.
- Pad lights are visual approximations. Full physical LED dynamics, haptics, pitch bend gestures, and performance behavior still need hardware/firmware integration.
- The included `SchemaSettings.tsx` has only a small change from the previous supplied version: it listens for setting-change events so its displayed values don't become stale when Melodic editor changes them.

## Verify

From `companion-app`:

```bash
npm run build
```

With the mock board running, try switching `layout.melodic`, turning on `color.custom_pads`, choosing pad 19 and painting it blue, toggling MPE, and clicking Refresh. Switch simulated firmware versions and confirm only supported settings show.

The generated TSX was statically checked with TypeScript 5.8 and lightweight React declarations, and the melodic mapping received direct note tests. A full project build and physical board test were **not** run here.
