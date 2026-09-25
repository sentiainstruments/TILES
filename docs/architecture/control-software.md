# Control software: how TILES talks to the mapper / layout store

Decisions and direction for the companion software (mapper, control panel,
layout store). Written 2026-09 after real feedback: "it's worth streamlining
stuff and figuring out how we are going to communicate with the mapper/control/
layout store software" -> "yes start with the settings table and flash saving.
the app should be compatible in mac and windows and linux hopefully. the main
thing is that its self contained and can interface with the system... layout
store means downloading from online eventually."

## What exists (firmware side)

- **Settings table + flash saving** -- built. One registry row per setting
  (`firmware/src/profiles/`), saved sparsely to a power-cut-safe two-slot flash
  store (`firmware/src/storage/`), exposed over the USB vendor interface as a
  text shell (`GET/SET/LIST/RESET/SAVE/SCHEMA/INFO`, `shared/protocol/README.md`).
  `SCHEMA` lets an app discover every setting (id, type, range, default) instead
  of hard-coding the list.
- **Not built:** the binary protocol, layouts/profiles as stored objects,
  calibration streaming, the app itself.

## Decisions

1. **The USB vendor interface is THE control channel; MIDI stays for
   performance.** The vendor interface is separate from MIDI and CDC, so the app
   works while Ableton has the MIDI port open (a DAW owns the port; on Windows,
   often exclusively), and high-rate calibration streaming (24 pads x XYZ at
   ~120 Hz) can't compete with note/expression traffic. DAW integration (Remote
   Script, TILES DISPLAY) stays on MIDI -- it is performance-adjacent and the DAW
   owns that path.
2. **A setting is one table row.** Adding one never changes the protocol; the app
   builds its UI from `SCHEMA`. Ids are permanent; keys are for humans.
3. **The device is the source of truth.** The app reads the device on connect.
   On-device edits (a scale button) and app edits both land in the owning module,
   so they can't disagree. Later: a revision counter + change notification so a
   running app notices a button-driven change.
4. **Layouts are portable files, and the store is only distribution.** A layout
   = a versioned, hashed file holding a subset of the settings/profile schema
   (later: pad->note maps, scales, themes). The app downloads, validates (schema +
   device protocol version + the setting ranges) and pushes it; the **device never
   talks to a store**. Firmware ranges are enforced on the device regardless of
   what a file says, so a bad or malicious layout can't push an unsafe value
   (LED power ceiling, CV output range). Signing/metadata become relevant only
   once there is an online catalog.

## Next steps (in order)

1. **Binary protocol** replacing the text shell for the app (the shell stays as a
   debug tool): length-prefixed frames, sequence IDs, a version/capability
   handshake so an old app and new firmware (or the reverse) fail safely, chunked
   transfers for larger blobs (layouts, later firmware update), a separate stream
   for telemetry. Generated from ONE schema into C, TypeScript and Python
   (`shared/protocol/` + `tools/` -- the planned, still unbuilt codegen), which
   also removes today's "must match op_mode.c" duplication of DAW CC numbers and
   channel conventions.
2. **Driverless on every OS**: add the Microsoft OS 2.0 + WebUSB (BOS)
   descriptors so Windows binds WinUSB to the vendor interface automatically; macOS
   needs nothing; Linux needs one udev rule for non-root access. Firmware change
   only, independent of the app stack.
3. **Layout/profile object** in flash (own region via `storage/`) plus the
   revision counter.
4. The app.

## App stack options

Requirements: macOS + Windows + Linux, self-contained (no separate runtime to
install), talks to a USB vendor interface, fetches JSON from the web, updates
itself. The firmware side is identical for all of them -- it only sees USB bulk
transfers -- so the choice is reversible.

| Option | For | Against |
|---|---|---|
| **Electron** (current plan) | Chromium's WebUSB means USB from the UI process with no native module to build for three OSes; biggest ecosystem for the store UI (React, HTTPS, JSON); mature auto-update, code signing, notarization | Large installer/RAM (~100+ MB); ships a whole browser |
| **Tauri 2** | Much smaller (single-digit-MB class), Rust backend | Uses each OS's own webview, and WebUSB isn't available in all of them -- so USB goes through Rust (`nusb`/`rusb`) with a Rust<->JS bridge; three different web engines to test |
| **Qt** (C++ or PySide) | Self-contained, native look, mature libusb bindings | A different UI stack from web tech, so the store UI and any future web presence don't share code; LGPL obligations |
| **.NET + Avalonia** | Self-contained per-OS publish, one C# codebase | Smaller ecosystem; USB via a libusb wrapper |
| **Flutter desktop** | One codebase, native-compiled | USB packages are the least mature of the group |
| **Browser-only web app** | Zero install | WebUSB is Chrome/Edge only (no Safari, no Firefox); not "self-contained" |

**Recommendation: stay with Electron for V1.** The deciding factors are
WebUSB-in-Chromium (no per-OS native USB build) and web tech for the online layout
store. Tauri is the credible lighter alternative if installer size becomes a
priority; a browser-only build of the same UI can be offered later for Chrome/Edge
users.
