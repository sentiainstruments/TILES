# src/renderer/

The UI. React/TypeScript. Talks to `../main/` only through IPC — pad grid,
calibration wizard + live monitor, scale/note editor, scene/profile
manager, diagnostics view, firmware update flow. Visual language should
follow the SENTIA identity (clean, geometric, precise — see the product
debrief) rather than a generic dev-tool look, since this is a
user/artist-facing app, not an internal tool.
