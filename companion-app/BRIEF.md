# Companion app brief (for Sam)

Written 2026-09-29, as the starting point for designing and building the
TILES companion app. It covers what the app is for, what the hardware can
do today, how the app talks to it, and how we suggest structuring the
code. The deeper background (why Electron, why USB instead of MIDI, how
layouts will work) is in `../docs/architecture/control-software.md`.

**Where things stand:** the firmware side of settings works on real
hardware. The app doesn't exist yet. Nothing in this brief needs a
server.

---

## 1. What TILES is

A 24-pad expressive MIDI controller. Each pad senses touch, how hard and
how fast it's struck (velocity), how hard it's held afterwards (pressure),
and sideways tilt (pitch bend). It plays MPE synths (Equator, Serum,
Vital…) with per-note expression, has built-in modes (melodic, chord,
guitar, sequencer, song/looper, Ableton Scene Launch, games), a sustain/
expression pedal jack, 5-pin/TRS DIN MIDI, and CV/gate out.

**Hardware the UI will talk about:**

- **24 pads** in 4 rows × 6 columns, each with an RGB LED and a haptic
  motor underneath.
- **6 buttons**: minus, plus, triangle, diamond (transport: play/stop/
  record), square ("sentia": pitch-bend toggle, expression menu), circle
  (shift/power).
- **Underglow** LEDs, a **pedal jack**, **DIN MIDI** in/out, **CV/gate**
  out (only active on external power).
- On USB it appears as: two MIDI ports ("SENTIA TILES MIDI" for playing,
  "SENTIA TILES DAW" for the Ableton script), a serial console (debug
  only), and the **control interface** the app uses (section 5).

## 2. What the app is (and isn't)

- **A desktop app** for macOS, Windows and Linux: Electron + Vite + React +
  TypeScript. Electron gives us USB access to the device on every OS;
  a plain browser tab can't do that reliably.
- **TILES is the source of truth.** Settings live in the device's own
  flash memory and survive reboots and firmware updates. The app reads
  them when TILES is plugged in and writes changes back. It never keeps
  its own copy that could disagree with the device.
- **No database or server for V1.** Presets/layouts are files on the
  user's computer. A server (accounts, sharing layouts online, hosting
  firmware updates) is a later, separate project -- phase 3 below.
- **No microservices.** When the online side happens, one backend plus
  file storage is plenty.
- **Not a MIDI app.** The app does not use the MIDI ports -- a DAW
  usually has those open. It uses its own USB control interface, which
  works while Ableton is running.

## 3. Phases

| Phase | Deliverable | Needs the real board? | Needs a server? |
|---|---|---|---|
| 0 | Clickable prototype with a **fake TILES** (`fixtures/tiles-settings.json`) | No | No |
| 1 | Electron app talking to the real board: every setting, live | Yes | No |
| 2 | Layout files (save/load/share by file), calibration view, firmware update from a file | Yes (+ new firmware features) | No |
| 3 | Online layout store, accounts, update hosting | Yes | Yes |

The step before phase 0 is design: agree on the user tasks (section 4),
wireframe the top few, then prototype them.

## 4. User tasks (draft -- to prioritize together)

What a TILES owner opens the app to do. "Firmware today" says whether the
device already supports it (✅), partly (⚠️), or not yet (❌). Design all
of them; only the ✅ ones can be wired to the real board in phase 1.

| # | Task | Firmware today |
|---|---|---|
| 1 | **Tune the feel**: pitch-bend and pressure sensitivity, MPE on/off | ✅ settings |
| 2 | **Set up the pedal**: sustain vs expression, pedal polarity, sustain style | ✅ settings |
| 3 | **Change the look**: LED brightness levels for idle/root/fifth pads, echo colours | ✅ settings |
| 4 | **Tune harmonics** (light touches pluck overtones of a held note) | ✅ settings |
| 5 | **Set up CV/gate** for modular gear: volts per semitone, trims | ✅ settings |
| 6 | **DIN MIDI jack type** (TRS A/B) | ✅ settings |
| 7 | **Update firmware** | ⚠️ reboot-to-bootloader works (`REBOOT BOOTSEL`); the app must then copy the `.uf2` -- flow not designed |
| 8 | **Set up Ableton**: install the TILES script, explain the two ports | ⚠️ app-side only (copy a folder); see `../daw-integration/README.md` |
| 9 | **Pick scale / key / layout** | ⚠️ 18 scales + 9 custom slots exist on the device, chosen with buttons; not exposed to the app yet |
| 10 | **Save / load / share a setup** (layout file) | ❌ layout object not built on the device |
| 11 | **Calibrate pads** / see live pad sensors | ❌ no streaming yet |
| 12 | **Remap pads** (notes/CCs per pad) | ❌ not built |
| 13 | **Diagnose** ("is pad 7 broken?") | ⚠️ `INFO` exists; per-pad test not exposed |
| 14 | See a change made on the device (button press) reflected in the app | ❌ no change notification yet -- the app must re-read |

Whatever the top-priority tasks need that's ❌ goes onto the firmware
list; tell us early.

## 5. Talking to TILES

**Finding it:** USB vendor ID `0x1209`, product ID `0x0001` (a
*temporary* test ID -- see the note below), interface named **"SENTIA
TILES Control"**. Match on the interface name, not only the ID. Older
boards may still report `0x2E8A:0x100A`.

**Protocol (V1, text):** one command per line, one or more reply lines.
Full spec: `../shared/protocol/README.md`. Working reference client:
`../tools/tiles_control.py` (~200 lines of Python -- read this first).

```
SCHEMA            -> one line per setting: id, key, type, range/values, default; then OK
LIST              -> key=value for every setting; then OK
GET <key>         -> the value, or ERR ...
SET <key> <value> -> OK, or ERR unknown-key / bad-value / out-of-range
RESET <key>|ALL   -> back to default
SAVE              -> write to flash now (it also auto-saves ~2 s after the last change)
INFO              -> unit=2/4, firmware=0.2.0, flash-store status...
REBOOT BOOTSEL    -> reboots into the bootloader for a firmware update (no reply)
REBOOT APP        -> plain restart
```

Things the app must respect:

- **Build the settings UI from `SCHEMA`**, don't hard-code the list.
  Adding a setting to the firmware must not need an app release. `id` is
  permanent; `key` is for humans; ranges are enforced by the device (an
  out-of-range SET is refused, not clamped).
- **Read on connect, re-read after reconnect.** Values can change on the
  device itself (buttons, a firmware update).
- **Drain before each command** -- a previous, interrupted session can
  leave reply lines queued; `tiles_control.py`'s `drain()` shows how.
- **Debounce sliders** (send on release, or at most every ~100 ms). Each
  SET is cheap, but the device saves to flash ~2 s after the last change
  (and only while no pad is touched), and a flash write pauses it for tens
  of milliseconds -- a stream of SETs just keeps postponing that.
- **`persist=0` settings** (currently only `cv_gate.enabled`) reset every
  boot by design -- show that in the UI.
- **LED `look.*` values are % of a fixed safe ceiling**, not raw
  brightness; no setting can exceed it.
- **CV/gate only runs on external power**, even when enabled -- the UI
  should say so rather than imply it's broken.

**OS notes:** macOS needs nothing. Windows gets Microsoft's WinUSB driver
automatically (device interface GUID `{8cfdc7ad-aecd-411d-9610-
df8c9929713c}`) -- implemented but **never tested on a Windows machine**.
Linux will need one udev rule for non-root access (not written yet).

**Coming later (phase 2):** a binary protocol with version handshake,
streaming (live pad sensors), and larger transfers (layouts). Keep the
transport behind an interface (section 6) so swapping it is local.

**Temporary USB ID:** `1209:0001` is a shared test ID that may only be
used in-house. Before any unit is given out it changes to a real ID
(from Raspberry Pi). Keep VID/PID in one constant.

## 6. Suggested code structure

```
companion-app/
  src/main/        Electron main process: USB (the only code that touches it),
                   file dialogs/storage, firmware-update orchestration
  src/renderer/    React UI -- talks to main only through a small IPC API
  src/shared/      types both sides use (Setting, DeviceInfo, ...)
  fixtures/        tiles-settings.json -- real SCHEMA + values for the fake device
```

Three layers, so the prototype becomes the real app without a rewrite:

1. **`DeviceTransport`** -- `connect()`, `send(line) -> replyLines`,
   `onDisconnect`. Two implementations: `FakeTilesTransport` (answers
   `SCHEMA/LIST/GET/SET/...` from the fixture, enforces ranges, can
   simulate unplugging) and `UsbTransport` (the real one, phase 1).
2. **`TilesClient`** -- typed API over the transport: `getSchema()`,
   `getAll()`, `set(key, value)`, `reset(key)`, `info()`. Parses the text
   protocol. Later swapped to the binary protocol behind the same API.
3. **UI state** -- mirrors the device: loaded from `getAll()`, updated by
   successful `set()`s, reloaded on reconnect.

A generic control per setting type (bool → toggle, enum → segmented/
select, int/uint/float with min/max → slider + number field) covers every
setting today. Friendly labels, grouping and help text can live in an app
side table keyed by `key` -- anything missing there still shows up from
`SCHEMA` with a generic label.

## 7. Current settings (from a real board)

Grouped the way the ids are grouped. "Basic" vs "Advanced" is our
suggestion for the UI.

| Key | Type | Range | Default | Suggest |
|---|---|---|---|---|
| `pedal.mode` | enum | sustain, expression | sustain | Basic |
| `pedal.polarity` | enum | normally_open, normally_closed | normally_open | Basic |
| `pedal.sustain_style` | enum | synth, hold | synth | Advanced |
| `expression.mpe_enabled` | bool | | on | Basic |
| `expression.pitch_bend_sensitivity` | float | 0.001 – 1.0 | 0.065 | Basic |
| `expression.aftertouch_sensitivity` | uint | 1 – 65535 | 900 | Basic |
| `cv_gate.enabled` | bool (not saved) | | off | Basic (CV page) |
| `cv_gate.pitch.volts_per_semitone` | float | 0.001 – 1.0 | 0.0833 (1 V/oct) | Advanced |
| `cv_gate.pitch.reference_note` | int | 0 – 127 | 0 | Advanced |
| `cv_gate.pitch.zero_trim_volts` | float | -2.5 – 2.5 | 0 | Advanced |
| `cv_gate.pitch.gain_trim` | float | 0.5 – 2.0 | 1.0 | Advanced |
| `cv_gate.pressure.full_scale_volts` | float | 0.1 – 10 | 10 | Advanced |
| `cv_gate.pressure.zero_trim_volts` | float | -2.5 – 2.5 | 0 | Advanced |
| `cv_gate.pressure.gain_trim` | float | 0.5 – 2.0 | 1.0 | Advanced |
| `look.idle_baseline_percent` | uint | 0 – 100 | 50 | Basic |
| `look.natural_pad_percent` | uint | 0 – 100 | 21 | Basic |
| `look.root_pad_percent` | uint | 0 – 100 | 40 | Basic |
| `look.fifth_pad_percent` | uint | 0 – 100 | 40 | Basic |
| `look.fifth_red_tint_percent` | uint | 0 – 100 | 35 | Advanced |
| `look.echo_sustain_tint_percent` | uint | 0 – 100 | 35 | Advanced |
| `look.echo_secondary_g_percent` | uint | 0 – 100 | 18 | Advanced |
| `look.echo_secondary_b_percent` | uint | 0 – 100 | 12 | Advanced |
| `look.echo_flash_ms` | uint | 0 – 2000 | 180 | Advanced |
| `midi.din_trs_type` | enum | a, b | a | Basic (MIDI page) |
| `features.melodic_harmonics` | bool | | on | Basic |
| `features.harmonics.arm_ms` | uint | 0 – 1000 | 150 | Advanced |
| `features.harmonics.confirm_ms` | uint | 0 – 500 | 40 | Advanced |
| `features.harmonics.press_depth` | uint | 1 – 1000 | 128 | Advanced |

What each one does, in detail: `../shared/protocol/README.md` ("Key
catalog"). Some raw units (pitch-bend sensitivity as a "cosine
deviation", aftertouch as a depth number) aren't meaningful to players --
the UI should present them as "less ↔ more sensitive". The device's own
expression menu maps them onto 6 steps; matching that is a good default.

## 8. Design notes

- **SENTIA look:** clean, geometric, precise; Sentia magenta `#FF00FF` is
  the accent the device itself uses. A player-facing app, not a dev tool.
- **Show the pad grid** (4×6) as the app's anchor visual -- most future
  features (layouts, remapping, calibration, diagnostics) are per-pad.
- **Connection state is always visible**: not connected / connected
  (with unit + firmware) / needs a firmware update.
- **Changes apply live** (no "Apply" button) -- the device saves them
  itself a moment later.
- **Reset to default** per setting and per page.

## 9. Open questions for us

1. Which tasks in section 4 are V1?
2. Firmware update: will the app ship firmware files inside itself, or
   download them (which pulls in a server)?
3. Should the app also be the Ableton-setup helper (task 8)?
4. Scales/layouts on the device: what should the app be able to change?
   (Needs firmware work either way.)

## 10. Where to look

| Topic | File |
|---|---|
| Architecture decisions, stack comparison | `../docs/architecture/control-software.md` |
| Protocol + every setting explained | `../shared/protocol/README.md` |
| Reference client (Python) | `../tools/tiles_control.py`, `../tools/README.md` |
| Real settings data for the fake device | `fixtures/tiles-settings.json` |
| USB IDs, firmware version | `../firmware/src/midi/product_identity.h` |
| Ableton setup / two MIDI ports | `../daw-integration/README.md` |
| Pad wiring, buttons | `../docs/hardware/`, `../firmware/src/board/board_layout.h` |
