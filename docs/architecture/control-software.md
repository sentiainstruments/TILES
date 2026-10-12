# Control software: how TILES talks to the companion app

How the companion software (control panel, mapper, layout store) talks
to the device, and the decisions behind it. Requirements: macOS, Windows
and Linux; self-contained (nothing else to install); able to reach the
system's USB; layouts downloadable from an online store eventually.

## What exists (firmware side)

- **Settings table + flash saving.** One registry row per setting
  (`firmware/src/profiles/`), saved sparsely to a power-cut-safe two-slot
  flash store (`firmware/src/storage/`), exposed over the USB vendor
  interface as a text shell (`GET/SET/LIST/RESET/SAVE/SCHEMA/INFO`,
  `shared/protocol/README.md`). `SCHEMA` lets an app discover every
  setting (id, type, range, default) instead of hard-coding the list.
- **Software reboot, no BOOTSEL button.** Two paths:
  - pico-sdk's standard USB reset interface (`pico_usb_reset`, added by
    hand in `firmware/src/midi/usb_descriptors.c`), which
    `picotool load -f` uses to flash a running board. picotool needs the
    non-stock USB ID spelled out (`--vid/--pid`); `tools/flash.sh` does
    that (`firmware/AGENTS.md`, "Flash").
  - `REBOOT BOOTSEL` / `REBOOT APP` over the settings shell, for a
    script or the app.

  This is the plumbing a firmware-update flow will use, not the flow
  itself (that needs a UI and, eventually, A/B partitions with
  rollback).
- **Driverless on Windows:** Microsoft OS 2.0 (BOS) descriptors bind
  WinUSB to the vendor interface automatically. Not yet tried on a
  Windows machine.
- **Colour schemes and custom scales (0.2.7).** The idle note-role
  colours (root, fifth, third, note, accidental) and per-pad colours are
  settings (`color.*`); custom scales live in a second flash store, the
  content store (`firmware/src/profiles/content.h`), pushed and pulled with
  `SCALES` / `SCALE PUT|GET|DELETE`. Each pushed item records its pack,
  item and version, for selling content in packs
  ([`content-packs.md`](content-packs.md)).
- **Not built:** the binary protocol, layouts as stored objects,
  entitlements for paid modes/games, calibration streaming, a Linux udev
  rule, the app itself.

## Decisions

1. **The USB vendor interface is the control channel; MIDI stays for
   performance.** The vendor interface is separate from MIDI and CDC, so
   the app works while a DAW has the MIDI ports open (on Windows often
   exclusively), and high-rate calibration streaming (24 pads x XYZ at
   ~120 Hz) can't compete with note and expression traffic. DAW
   integration (the Ableton script, TILES DISPLAY) stays on MIDI: it is
   part of performance, and the DAW owns that path.
2. **A setting is one table row.** Adding one never changes the
   protocol; the app builds its UI from `SCHEMA`. Ids are permanent;
   keys are for humans.
3. **The device is the source of truth.** The app reads the device on
   connect. On-device edits (a scale button) and app edits both land in
   the owning module, so they can't disagree. Later: a revision counter
   and change notification, so a running app notices a button-driven
   change.
4. **Layouts are portable files; the store only distributes them.** A
   layout is a versioned, hashed file holding a subset of the settings
   schema (later: pad-to-note maps, scales, themes). The app downloads,
   validates (schema, device protocol version, setting ranges) and pushes
   it; the **device never talks to a store**. The device enforces its
   own ranges whatever a file says, so a bad or malicious layout can't
   push an unsafe value (LED power ceiling, CV range). Signing and
   metadata matter only once there is an online catalog.

## Next steps (in order)

1. **Binary protocol** for the app (the text shell stays as a debug
   tool): length-prefixed frames, sequence IDs, a version/capability
   handshake so an old app and new firmware (or the reverse) fail
   safely, chunked transfers for larger blobs (layouts, later firmware
   update), and a separate telemetry stream. Generated from one schema
   into C, TypeScript and Python (`shared/protocol/` + codegen in
   `tools/`, not built), which also ends the hand-kept copies of the DAW
   CC numbers and channel conventions in the Ableton script.
2. **Driverless on every OS:** test the Windows descriptors; Linux needs
   one udev rule for non-root access; macOS needs nothing. A WebUSB
   descriptor is only worth adding if a browser build of the app
   happens.
3. **Layout object** as a new record type in the content store (the
   region exists; see [`content-packs.md`](content-packs.md)) plus the
   revision counter.
4. The app.

## App stack

The firmware side is identical for every option (it only sees USB bulk
transfers), so the choice is reversible.

| Option | For | Against |
|---|---|---|
| **Electron** (chosen) | Chromium's WebUSB gives USB from the UI with no native module to build for three OSes; the biggest ecosystem for the store UI (React, HTTPS, JSON); mature auto-update, code signing, notarization | Large installer and RAM (~100+ MB); ships a whole browser |
| **Tauri 2** | Much smaller (single-digit MB), Rust backend | Uses each OS's own webview, and WebUSB isn't in all of them, so USB goes through Rust (`nusb`/`rusb`) with a Rust-JS bridge; three web engines to test |
| **Qt** (C++ or PySide) | Self-contained, native look, mature libusb bindings | A different UI stack from web tech, so the store UI and any web presence share no code; LGPL obligations |
| **.NET + Avalonia** | Self-contained per-OS publish, one C# codebase | Smaller ecosystem; USB via a libusb wrapper |
| **Flutter desktop** | One codebase, native-compiled | The least mature USB packages of the group |
| **Browser-only web app** | Zero install | WebUSB is Chrome/Edge only (no Safari, no Firefox); not self-contained |

**Electron for V1**, with Vite + React + TypeScript and no database or
server until the online layout store (`companion-app/BRIEF.md`, the
developer brief). The deciding factors are WebUSB in Chromium (no
per-OS native USB build) and web tech for the store. Tauri is the
credible lighter alternative if installer size becomes a priority; a
browser build of the same UI could be offered later for Chrome/Edge
users.
