# Apple Logic Pro Integration for SENTIA TILES

A native, event-driven macOS CoreMIDI bridge and Logic Pro control surface integration that brings plug-and-play transport, Live Loops launching, Smart Controls with Auto-Assign, mixer banking, MPE per-pad expression, and real-time RGB LED plus haptic feedback to SENTIA TILES—without altering firmware, companion software, or the existing Ableton integration.

## Dev Review

> [!IMPORTANT]

All deliverables will live strictly inside the dedicated Logic Pro integration folder, leaving firmware, shared contracts, companion app, and Ableton scripts untouched.

Bridge Architecture: Native macOS CoreMIDI background bridge paired with Logic Pro Controller Assignments (plus Mackie Control handshake for automatic Logic Pro surface recognition and bidirectional mixer/Smart Control feedback).

Add files exclusively within the new Logic Pro integration folder, including dedicated `l_README.md` and `l_HISTORY.md` documentation.

Preserve untouched per-pad MPE expression (touch, strike, press, tilt on channels 2–16) while offering an Auto-Assign Smart Controls bridge mode that translates per-pad strike, press, and tilt into discrete, learnable CCs and MCU V-Pot/Fader parameters.

Mirror the familiar Ableton Scene Launch 5×4 Live Loops grid, Diamond transport, Circle+pad gestures, and melodic note echo (`TILES DISPLAY`), and add toggle-able Smart Controls and Mixer Banking pages on the bridge.

> [!NOTE]
> Because firmware remains unmodified, hardware haptic motors and pad LEDs in Scene Launch mode are driven through the existing DAW port SysEx feedback protocol (`has_clip`, `is_playing`, `is_triggered` + RGB), while melodic-mode per-pad haptics (touch tick, strike kick, pressure sustain) and per-pad expression (velocity, aftertouch, pitch bend) remain firmware-native at hardware scan speed.

---

## 1. Overview & Core Concept

> What It Does: Bridges the physical SENTIA TILES USB-MIDI ports (`MIDI` and `DAW`) to virtual macOS CoreMIDI control ports understood natively by Apple Logic Pro. It maps TILES's Diamond transport, 4×6 pad grid, Minus/Plus banking keys, and Circle shift combos to Logic Pro's Transport, Live Loops grid, Smart Controls, and Track Mixer—while translating Logic Pro feedback back into TILES SysEx LED colors and haptic readiness/playing states.

> Key Value: Sub-millisecond, zero-polling event routing using Apple's native CoreMIDI C/Swift runtime, negligible idle CPU/battery draw, automatic Mackie Control / Smart Controls Auto-Assign support, and seamless switching between MPE performance, Live Loops, Smart Controls, and Mixer pages.

## 2. User Experience & Hardware Control Flows

### Key User Flows

1. Zero-Config Background Startup:
   - The lightweight CoreMIDI bridge runs on-demand or via a macOS LaunchAgent.
   - It sleeps with zero CPU usage until CoreMIDI notifies it that a `SENTIA TILES` unit has connected or Logic Pro sends a control surface query.
2. MIDI / MPE Performance & Auto-Assign Flow:
   - MPE Mode (Default): `SENTIA TILES N (MIDI)` passes directly into Logic Pro tracks with MIDI Mono Mode enabled. Each pad independently delivers capacitive touch (firmware haptic tick), strike velocity (Note-On), hold pressure (Channel Pressure `0xD0`), and sideways tilt (14-bit Pitch Bend `0xE0`) on its own MPE member channel (2–9), with zero added latency.
   - TILES DISPLAY (Melodic Echo): Placing the lightweight Logic Pro Scripter MIDI FX preset before an instrument passes all MPE data through untouched while tapping active notes back to TILES's DAW port so pads glow green (primary track) or soft red (secondary track).
   - Auto-Assign / Smart Controls Mode: Pressing `B` in Logic opens Smart Controls. When switched to Smart Controls page (or via Logic's Controller Assignments / Auto-Assign), pad strike, continuous press, and tilt map cleanly to discrete assignable CCs / MCU V-Pots so Logic can Auto-Assign or `Cmd+L` learn any pad's gesture independently without MPE channel collisions.
3. Live Loops Grid Flow (Familiar Ableton Parity):
   - Enter Scene Launch (`Triangle -> Pad 6`) on TILES.
   - Columns 1–5, Rows 1–4: Live Loops tracks × scenes (`5×4` window). Bare touch gives a crisp firmware haptic click on populated cells and a steady buzz on playing cells; a pressure click past half depth fires/queues a cell or stops a playing cell.
   - Column 6: Scene/Column triggers (Sentia magenta `#FF00FF` when populated, blinking when queued).
   - Minus / Plus (`−` / `+`): Pans the 5-track window (`CC 106` track offset) across up to 64 tracks, shifting Logic Pro's control surface bank in lockstep.
   - Circle Combos: `Circle + Empty Cell` arms & records a new MIDI cell and switches TILES to Melodic mode; `Circle + Diamond` ends capture or triggers master stop; `Circle + Hold 3s` clears/deletes a cell.
4. Expanded Logic Pro Pages (Smart Controls & Mixer Banking):
   - Accessible from the bridge's virtual mode switcher (triggered via DAW page combo or bridge hot-key/menu bar status without modifying firmware):
     - Page 1 — Live Loops: 5×4 cell/scene matrix with real-time Logic cell color & play-state feedback.
     - Page 2 — Smart Controls: Maps pads/rows to 8 bidirectional Smart Control encoders/toggles (bankable across pages) with visual LED bar/color intensity feedback on the 4×6 grid and MCU Auto-Assign support.
     - Page 3 — Mixer Banking: 5 visible channel strips (columns 1–5) × 4 function rows per track: Row 1 = Mute, Row 2 = Solo, Row 3 = Record Arm, Row 4 = Select/Volume step; Column 6 = Bank Left/Right, Channel Left/Right, Undo/Cycle, Save/Automation toggle.

### Visual & Tactile Identity on Hardware

- Palette Adherence: Sentia Magenta (`#FF00FF`) marks active scene column triggers and selection highlights; Sentia Periwinkle (`#94ABF9`) and Ink (`#171D3A`) cues guide Smart Control parameter feedback; track/cell colors mirror Logic Pro's mixer/cell colors halved to 7-bit RGB SysEx (`0xF0 0x7D 0x01 ... 0xF7`).
- Haptic Language: Touching populated Live Loops cells or active mixer toggles triggers a crisp ready kick; touching playing cells produces a continuous subtle sustain buzz; Smart Control and mixer bank changes confirm with crisp state transitions.

---

## 3. Key Product Decisions & Trade-Offs

- Decision 1 — Compiled Native CoreMIDI Binary (Swift/C) vs. Python/Node Daemon:
  - Chosen Approach: Single lightweight native macOS executable (Swift + C `CoreMIDI.framework` interop, built cleanly with `clang` / `swiftc` with zero external package dependencies).
  - Why: Meets the strict low-resource, low-power, and ultra-low-latency goals. Native CoreMIDI callbacks execute on Apple's high-priority real-time MIDI I/O thread with zero interpreter overhead, zero garbage-collection pauses, < 5 MB memory footprint, and App Nap / event-driven sleep when idle.
  - Alternatives Considered: Python (`mido`/`rtmidi`) or Node.js—rejected due to higher idle memory, interpreter wakeups, and extra runtime installation friction on macOS.

- Decision 2 — Dual Protocol Strategy (Mackie Control Handshake + Dedicated Live Loops / Assignment Port):
  - Chosen Approach: Expose two clean virtual CoreMIDI port pairs from the bridge:
    1. TILES Logic MCU (standard Mackie Control Universal protocol for automatic Logic Pro Control Surface recognition, Diamond transport, 8/5-channel mixer banking, faders/pans/mute/solo/arm, and Smart Controls Auto-Assign).
    2. TILES Logic Grid (dedicated virtual port for Live Loops cell/scene triggers, Controller Assignments `Cmd+L` / `Option+K`, and per-pad Auto-Assign expression CCs).
  - Why: Logic Pro's built-in "Auto Assign" and bidirectional mixer/Smart Control feedback work out of the box with Mackie Control without requiring undocumented private APIs, while the separate Grid port prevents Live Loops triggers from colliding with MCU fader/transport bytes or MPE note tracks.
  - Alternatives Considered: Relying solely on reverse-engineered private Lua Device Scripts (`~/Music/Audio Music Apps/MIDI Device Scripts/`)—too brittle across Logic Pro updates and cannot handle custom multi-page translation or multi-unit routing as reliably without Logic bundling our VID/PID. (We can still ship an optional lightweight Lua identification descriptor alongside the bridge if desired for one-click Controller Auto-Recognition.)

- Decision 3 — Per-Pad Touch, Strike, Press & Tilt Independence:
  - Chosen Approach: Keep MPE passthrough raw and direct on `SENTIA TILES N (MIDI)` for expressive instruments, and implement an optional Per-Pad Expression Splitter inside the bridge on `TILES Logic Express` that converts incoming MPE channels (2–9) + pad tracking into independent per-pad CC/Note messages (Strike velocity, Press depth, Tilt bipolar CC) for Logic Pro Smart Control Auto-Assign / Modulation Matrix learning.
  - Why: Lets a player use TILES both as a true zero-latency MPE instrument and as 24 independent 4-dimensional expression controllers (touch/strike/press/tilt) inside Logic Pro without modifying firmware.

- Decision 4 — Strict Containment & Ableton Parity:
  - Chosen Approach: Place all code, install scripts, Controller Assignment helpers, Scripter JS, `l_README.md`, and `l_HISTORY.md` exclusively inside the Logic integration folder, mirroring Ableton's CC map (`102–111`), SysEx (`0x7D 0x01 0x10..0x12`), and multi-unit naming (`SENTIA TILES 1..4`).
  - Why: Guarantees zero regressions to firmware, companion app, or Ableton integration, and lets users move between Ableton Live and Logic Pro with identical physical muscle memory.

---

## 4. Technical Architecture & Data Strategy (Technical Reference)

### Architecture & Component Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          SENTIA TILES HARDWARE                          │
│  [USB Port 0: MIDI (MPE)]   [USB Port 1: DAW (CC 102-111 + SysEx)]      │
└────────────┬───────────────────────────────────────┬────────────────────┘
             │                                       │
             │ Direct MPE (Ch 1-9)                   │ Bi-directional
             │ (Touch/Strike/Press/Tilt)             │ CCs & SysEx RGB/Haptic
             ▼                                       ▼
┌────────────────────────────┐      ┌─────────────────────────────────────┐
│   Logic Pro Instrument     │      │    TILES Logic CoreMIDI Bridge      │
│   Tracks (MIDI Mono/MPE)   │      │    (Native macOS Real-Time Daemon)  │
│                            │      │                                     │
│  ┌──────────────────────┐  │      │  ┌───────────────────────────────┐  │
│  │ TILES DISPLAY        │  │      │  │ CoreMIDI Endpoint Router      │  │
│  │ (Logic Scripter JS)  ├──┼──────┼─►│ • Hotplug & Multi-Unit (1-4)  │  │
│  └──────────┬───────────┘  │ Note │  │ • Event-Driven (0% Idle CPU)  │  │
│             ▼              │ Echo │  └──────────────┬────────────────┘  │
│  [Alchemy / Sampler / AU]  │      │                 │                   │
└────────────────────────────┘      │  ┌──────────────▼────────────────┐  │
                                    │  │ Page & State Engine           │  │
                                    │  │ • Transport (102-104 <-> MCU) │  │
                                    │  │ • Page 1: Live Loops 5x4 Grid │  │
                                    │  │ • Page 2: Smart Controls      │  │
                                    │  │ • Page 3: Mixer Banking       │  │
                                    │  │ • MPE -> Per-Pad Expr Split   │  │
                                    │  └───────┬───────────────┬───────┘  │
                                    └──────────┼───────────────┼──────────┘
                                               │               │
                        Virtual Port:          │               │ Virtual Port:
                        TILES Logic MCU        │               │ TILES Logic Grid
                                               ▼               ▼
                                    ┌─────────────────────────────────────┐
                                    │          APPLE LOGIC PRO            │
                                    │ • Control Surfaces (MCU Auto-Assign)│
                                    │ • Live Loops Grid & Scene Triggers  │
                                    │ • Smart Controls & Mixer Banks      │
                                    └─────────────────────────────────────┘
```

### Data Model & State

- Multi-Unit Session Table (`1..4` units): Tracks each connected `SENTIA TILES N` device by CoreMIDI `MIDIUniqueID` and unit number so multiple boards never collide when replugged.
- Active Control Page State (`LiveLoops` | `SmartControls` | `Mixer`):
  - LiveLoops State: `64 × 4` matrix mirroring track/cell occupancy, playing/queued flags, and RGB color cache, synchronized with `trackOffset` (`0..59`) and active capture slot.
  - SmartControls State: `8` macro parameter values (`0..127`) + labels/active flags received from MCU V-Pot / LCD SysEx feedback, mapped onto rows/pads with quantized LED brightness & color encoding.
  - Mixer State: Per-bank (`5` visible tracks shifted by `−`/`+`) Mute, Solo, Record Arm, Select, Volume level, and Signal/Peak status updated from Logic's MCU feedback stream.
- Per-Pad Expression Tracker: Tracks active MPE voice channel → physical pad / MIDI note mapping so strike, pressure, and tilt can optionally be split into independent non-MPE Smart Control CCs without interrupting native MPE output.

### Interactive Component & State Mapping

- TILES → Logic Pro Translation:
  - `CC 102 / 103 / 104` (Play / Stop / Record) → MCU Transport Note/Buttons (`0x5E`, `0x5D`, `0x5F`) with `DIAMOND_IGNORED_WHILE_PLAYING` guard mirroring Ableton's performance layout.
  - `CC 106` (Track Offset) → Updates active `trackOffset` and dispatches MCU Bank Left/Right (`0x2E` / `0x2F`) or Channel Left/Right (`0x30` / `0x31`) pulses so Logic's Control Surface fader bar tracks TILES's 5-column window.
  - `CC 108 / 109 / 110 / 111` (Grid / Stop / Delete / Record) → Routed by active Page:
    - Live Loops Page: Fires Live Loops cell/scene assignments on `TILES Logic Grid`, sends MCU Record/Arm for `Circle + Empty`, or sends Cell Clear / Undo triggers.
    - Smart Controls Page: Pads adjust 8 Smart Control macros (stepped or pressure-continuous) + page bank + Smart Control Auto-Assign learn triggers.
    - Mixer Page: Pads toggle Mute (`MCU 0x10..0x17`), Solo (`MCU 0x08..0x0F`), Rec Arm (`MCU 0x00..0x07`), and Track Select (`MCU 0x18..0x1F`) for the 5 visible strips.
  - Page Switching: Switching pages (e.g., dedicated double-tap / DAW menu trigger or menu-bar toggle / assigned Logic key command) immediately repaints all 24 TILES pads via `0xF0 0x7D 0x01 0x10` SysEx so the hardware always reflects the active Logic Pro page.
- Logic Pro → TILES Feedback:
  - Incoming MCU feedback (transport LEDs, track Mute/Solo/Arm/Select LEDs, Smart Control V-Pot rings, LCD track names/colors) and Scripter/Grid updates are diffed against the cached state and pushed as minimal `0x10` / `0x11` SysEx frames only when a cell changes—minimizing USB traffic and power draw.
  - Scripter MIDI FX (`TILES DISPLAY`) mirrors primary (Ch 1 green) and secondary (Ch 2 soft red) track notes onto `SENTIA TILES N (DAW)` in Melodic mode while leaving the audio/MIDI track signal chain bit-exact.

### Key Deliverables & Documentation

- `l_README.md`: Complete user & technical guide in Sentia's clear, concise style (no comparison to outside products, exact port routing, one-time Mackie Control + Controller Assignments + Scripter setup, multi-unit support, page guide, troubleshooting).
- `l_HISTORY.md`: Architectural rationale, protocol decisions, Logic Pro constraints (MCU vs. Lua Device Scripts vs. CoreMIDI virtual ports), and design log matching `daw-integration/HISTORY.md`.
- Native macOS CoreMIDI Bridge (Swift/C source + zero-dependency build script + LaunchAgent plist + unit tests for protocol/state translation).
- Logic Pro Assets: TILES DISPLAY Scripter JavaScript preset (MPE-safe passthrough + dual-channel note echo), Mackie Control / Auto-Assign setup preset & Lua identification helper, and one-command installer/uninstaller script.

# Apple Logic Pro Integration for SENTIA TILES

A native, event-driven macOS CoreMIDI bridge and Logic Pro control surface integration that brings plug-and-play transport, Live Loops launching, Smart Controls with Auto-Assign, mixer banking, MPE per-pad expression, and real-time RGB LED plus haptic feedback to SENTIA TILES—without altering firmware, companion software, or the existing Ableton integration.

## Dev Review

> [!IMPORTANT]

All deliverables will live strictly inside the dedicated Logic Pro integration folder, leaving firmware, shared contracts, companion app, and Ableton scripts untouched.

Bridge Architecture: Native macOS CoreMIDI background bridge paired with Logic Pro Controller Assignments (plus Mackie Control handshake for automatic Logic Pro surface recognition and bidirectional mixer/Smart Control feedback).

Add files exclusively within the new Logic Pro integration folder, including dedicated `l_README.md` and `l_HISTORY.md` documentation.

Preserve untouched per-pad MPE expression (touch, strike, press, tilt on channels 2–16) while offering an Auto-Assign Smart Controls bridge mode that translates per-pad strike, press, and tilt into discrete, learnable CCs and MCU V-Pot/Fader parameters.

Mirror the familiar Ableton Scene Launch 5×4 Live Loops grid, Diamond transport, Circle+pad gestures, and melodic note echo (`TILES DISPLAY`), and add toggle-able Smart Controls and Mixer Banking pages on the bridge.

> [!NOTE]
> Because firmware remains unmodified, hardware haptic motors and pad LEDs in Scene Launch mode are driven through the existing DAW port SysEx feedback protocol (`has_clip`, `is_playing`, `is_triggered` + RGB), while melodic-mode per-pad haptics (touch tick, strike kick, pressure sustain) and per-pad expression (velocity, aftertouch, pitch bend) remain firmware-native at hardware scan speed.

---

## 1. Overview & Core Concept

> What It Does: Bridges the physical SENTIA TILES USB-MIDI ports (`MIDI` and `DAW`) to virtual macOS CoreMIDI control ports understood natively by Apple Logic Pro. It maps TILES's Diamond transport, 4×6 pad grid, Minus/Plus banking keys, and Circle shift combos to Logic Pro's Transport, Live Loops grid, Smart Controls, and Track Mixer—while translating Logic Pro feedback back into TILES SysEx LED colors and haptic readiness/playing states.

> Key Value: Sub-millisecond, zero-polling event routing using Apple's native CoreMIDI C/Swift runtime, negligible idle CPU/battery draw, automatic Mackie Control / Smart Controls Auto-Assign support, and seamless switching between MPE performance, Live Loops, Smart Controls, and Mixer pages.

## 2. User Experience & Hardware Control Flows

### Key User Flows

1. Zero-Config Background Startup:
   - The lightweight CoreMIDI bridge runs on-demand or via a macOS LaunchAgent.
   - It sleeps with zero CPU usage until CoreMIDI notifies it that a `SENTIA TILES` unit has connected or Logic Pro sends a control surface query.
2. MIDI / MPE Performance & Auto-Assign Flow:
   - MPE Mode (Default): `SENTIA TILES N (MIDI)` passes directly into Logic Pro tracks with MIDI Mono Mode enabled. Each pad independently delivers capacitive touch (firmware haptic tick), strike velocity (Note-On), hold pressure (Channel Pressure `0xD0`), and sideways tilt (14-bit Pitch Bend `0xE0`) on its own MPE member channel (2–9), with zero added latency.
   - TILES DISPLAY (Melodic Echo): Placing the lightweight Logic Pro Scripter MIDI FX preset before an instrument passes all MPE data through untouched while tapping active notes back to TILES's DAW port so pads glow green (primary track) or soft red (secondary track).
   - Auto-Assign / Smart Controls Mode: Pressing `B` in Logic opens Smart Controls. When switched to Smart Controls page (or via Logic's Controller Assignments / Auto-Assign), pad strike, continuous press, and tilt map cleanly to discrete assignable CCs / MCU V-Pots so Logic can Auto-Assign or `Cmd+L` learn any pad's gesture independently without MPE channel collisions.
3. Live Loops Grid Flow (Familiar Ableton Parity):
   - Enter Scene Launch (`Triangle -> Pad 6`) on TILES.
   - Columns 1–5, Rows 1–4: Live Loops tracks × scenes (`5×4` window). Bare touch gives a crisp firmware haptic click on populated cells and a steady buzz on playing cells; a pressure click past half depth fires/queues a cell or stops a playing cell.
   - Column 6: Scene/Column triggers (Sentia magenta `#FF00FF` when populated, blinking when queued).
   - Minus / Plus (`−` / `+`): Pans the 5-track window (`CC 106` track offset) across up to 64 tracks, shifting Logic Pro's control surface bank in lockstep.
   - Circle Combos: `Circle + Empty Cell` arms & records a new MIDI cell and switches TILES to Melodic mode; `Circle + Diamond` ends capture or triggers master stop; `Circle + Hold 3s` clears/deletes a cell.
4. Expanded Logic Pro Pages (Smart Controls & Mixer Banking):
   - Accessible from the bridge's virtual mode switcher (triggered via DAW page combo or bridge hot-key/menu bar status without modifying firmware):
     - Page 1 — Live Loops: 5×4 cell/scene matrix with real-time Logic cell color & play-state feedback.
     - Page 2 — Smart Controls: Maps pads/rows to 8 bidirectional Smart Control encoders/toggles (bankable across pages) with visual LED bar/color intensity feedback on the 4×6 grid and MCU Auto-Assign support.
     - Page 3 — Mixer Banking: 5 visible channel strips (columns 1–5) × 4 function rows per track: Row 1 = Mute, Row 2 = Solo, Row 3 = Record Arm, Row 4 = Select/Volume step; Column 6 = Bank Left/Right, Channel Left/Right, Undo/Cycle, Save/Automation toggle.

### Visual & Tactile Identity on Hardware

- Palette Adherence: Sentia Magenta (`#FF00FF`) marks active scene column triggers and selection highlights; Sentia Periwinkle (`#94ABF9`) and Ink (`#171D3A`) cues guide Smart Control parameter feedback; track/cell colors mirror Logic Pro's mixer/cell colors halved to 7-bit RGB SysEx (`0xF0 0x7D 0x01 ... 0xF7`).
- Haptic Language: Touching populated Live Loops cells or active mixer toggles triggers a crisp ready kick; touching playing cells produces a continuous subtle sustain buzz; Smart Control and mixer bank changes confirm with crisp state transitions.

---

## 3. Key Product Decisions & Trade-Offs

- Decision 1 — Compiled Native CoreMIDI Binary (Swift/C) vs. Python/Node Daemon:
  - Chosen Approach: Single lightweight native macOS executable (Swift + C `CoreMIDI.framework` interop, built cleanly with `clang` / `swiftc` with zero external package dependencies).
  - Why: Meets the strict low-resource, low-power, and ultra-low-latency goals. Native CoreMIDI callbacks execute on Apple's high-priority real-time MIDI I/O thread with zero interpreter overhead, zero garbage-collection pauses, < 5 MB memory footprint, and App Nap / event-driven sleep when idle.
  - Alternatives Considered: Python (`mido`/`rtmidi`) or Node.js—rejected due to higher idle memory, interpreter wakeups, and extra runtime installation friction on macOS.

- Decision 2 — Dual Protocol Strategy (Mackie Control Handshake + Dedicated Live Loops / Assignment Port):
  - Chosen Approach: Expose two clean virtual CoreMIDI port pairs from the bridge:
    1. TILES Logic MCU (standard Mackie Control Universal protocol for automatic Logic Pro Control Surface recognition, Diamond transport, 8/5-channel mixer banking, faders/pans/mute/solo/arm, and Smart Controls Auto-Assign).
    2. TILES Logic Grid (dedicated virtual port for Live Loops cell/scene triggers, Controller Assignments `Cmd+L` / `Option+K`, and per-pad Auto-Assign expression CCs).
  - Why: Logic Pro's built-in "Auto Assign" and bidirectional mixer/Smart Control feedback work out of the box with Mackie Control without requiring undocumented private APIs, while the separate Grid port prevents Live Loops triggers from colliding with MCU fader/transport bytes or MPE note tracks.
  - Alternatives Considered: Relying solely on reverse-engineered private Lua Device Scripts (`~/Music/Audio Music Apps/MIDI Device Scripts/`)—too brittle across Logic Pro updates and cannot handle custom multi-page translation or multi-unit routing as reliably without Logic bundling our VID/PID. (We can still ship an optional lightweight Lua identification descriptor alongside the bridge if desired for one-click Controller Auto-Recognition.)

- Decision 3 — Per-Pad Touch, Strike, Press & Tilt Independence:
  - Chosen Approach: Keep MPE passthrough raw and direct on `SENTIA TILES N (MIDI)` for expressive instruments, and implement an optional Per-Pad Expression Splitter inside the bridge on `TILES Logic Express` that converts incoming MPE channels (2–9) + pad tracking into independent per-pad CC/Note messages (Strike velocity, Press depth, Tilt bipolar CC) for Logic Pro Smart Control Auto-Assign / Modulation Matrix learning.
  - Why: Lets a player use TILES both as a true zero-latency MPE instrument and as 24 independent 4-dimensional expression controllers (touch/strike/press/tilt) inside Logic Pro without modifying firmware.

- Decision 4 — Strict Containment & Ableton Parity:
  - Chosen Approach: Place all code, install scripts, Controller Assignment helpers, Scripter JS, `l_README.md`, and `l_HISTORY.md` exclusively inside the Logic integration folder, mirroring Ableton's CC map (`102–111`), SysEx (`0x7D 0x01 0x10..0x12`), and multi-unit naming (`SENTIA TILES 1..4`).
  - Why: Guarantees zero regressions to firmware, companion app, or Ableton integration, and lets users move between Ableton Live and Logic Pro with identical physical muscle memory.

---

## 4. Technical Architecture & Data Strategy (Technical Reference)

### Architecture & Component Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          SENTIA TILES HARDWARE                          │
│  [USB Port 0: MIDI (MPE)]   [USB Port 1: DAW (CC 102-111 + SysEx)]      │
└────────────┬───────────────────────────────────────┬────────────────────┘
             │                                       │
             │ Direct MPE (Ch 1-9)                   │ Bi-directional
             │ (Touch/Strike/Press/Tilt)             │ CCs & SysEx RGB/Haptic
             ▼                                       ▼
┌────────────────────────────┐      ┌─────────────────────────────────────┐
│   Logic Pro Instrument     │      │    TILES Logic CoreMIDI Bridge      │
│   Tracks (MIDI Mono/MPE)   │      │    (Native macOS Real-Time Daemon)  │
│                            │      │                                     │
│  ┌──────────────────────┐  │      │  ┌───────────────────────────────┐  │
│  │ TILES DISPLAY        │  │      │  │ CoreMIDI Endpoint Router      │  │
│  │ (Logic Scripter JS)  ├──┼──────┼─►│ • Hotplug & Multi-Unit (1-4)  │  │
│  └──────────┬───────────┘  │ Note │  │ • Event-Driven (0% Idle CPU)  │  │
│             ▼              │ Echo │  └──────────────┬────────────────┘  │
│  [Alchemy / Sampler / AU]  │      │                 │                   │
└────────────────────────────┘      │  ┌──────────────▼────────────────┐  │
                                    │  │ Page & State Engine           │  │
                                    │  │ • Transport (102-104 <-> MCU) │  │
                                    │  │ • Page 1: Live Loops 5x4 Grid │  │
                                    │  │ • Page 2: Smart Controls      │  │
                                    │  │ • Page 3: Mixer Banking       │  │
                                    │  │ • MPE -> Per-Pad Expr Split   │  │
                                    │  └───────┬───────────────┬───────┘  │
                                    └──────────┼───────────────┼──────────┘
                                               │               │
                        Virtual Port:          │               │ Virtual Port:
                        TILES Logic MCU        │               │ TILES Logic Grid
                                               ▼               ▼
                                    ┌─────────────────────────────────────┐
                                    │          APPLE LOGIC PRO            │
                                    │ • Control Surfaces (MCU Auto-Assign)│
                                    │ • Live Loops Grid & Scene Triggers  │
                                    │ • Smart Controls & Mixer Banks      │
                                    └─────────────────────────────────────┘
```

### Data Model & State

- Multi-Unit Session Table (`1..4` units): Tracks each connected `SENTIA TILES N` device by CoreMIDI `MIDIUniqueID` and unit number so multiple boards never collide when replugged.
- Active Control Page State (`LiveLoops` | `SmartControls` | `Mixer`):
  - LiveLoops State: `64 × 4` matrix mirroring track/cell occupancy, playing/queued flags, and RGB color cache, synchronized with `trackOffset` (`0..59`) and active capture slot.
  - SmartControls State: `8` macro parameter values (`0..127`) + labels/active flags received from MCU V-Pot / LCD SysEx feedback, mapped onto rows/pads with quantized LED brightness & color encoding.
  - Mixer State: Per-bank (`5` visible tracks shifted by `−`/`+`) Mute, Solo, Record Arm, Select, Volume level, and Signal/Peak status updated from Logic's MCU feedback stream.
- Per-Pad Expression Tracker: Tracks active MPE voice channel → physical pad / MIDI note mapping so strike, pressure, and tilt can optionally be split into independent non-MPE Smart Control CCs without interrupting native MPE output.

### Interactive Component & State Mapping

- TILES → Logic Pro Translation:
  - `CC 102 / 103 / 104` (Play / Stop / Record) → MCU Transport Note/Buttons (`0x5E`, `0x5D`, `0x5F`) with `DIAMOND_IGNORED_WHILE_PLAYING` guard mirroring Ableton's performance layout.
  - `CC 106` (Track Offset) → Updates active `trackOffset` and dispatches MCU Bank Left/Right (`0x2E` / `0x2F`) or Channel Left/Right (`0x30` / `0x31`) pulses so Logic's Control Surface fader bar tracks TILES's 5-column window.
  - `CC 108 / 109 / 110 / 111` (Grid / Stop / Delete / Record) → Routed by active Page:
    - Live Loops Page: Fires Live Loops cell/scene assignments on `TILES Logic Grid`, sends MCU Record/Arm for `Circle + Empty`, or sends Cell Clear / Undo triggers.
    - Smart Controls Page: Pads adjust 8 Smart Control macros (stepped or pressure-continuous) + page bank + Smart Control Auto-Assign learn triggers.
    - Mixer Page: Pads toggle Mute (`MCU 0x10..0x17`), Solo (`MCU 0x08..0x0F`), Rec Arm (`MCU 0x00..0x07`), and Track Select (`MCU 0x18..0x1F`) for the 5 visible strips.
  - Page Switching: Switching pages (e.g., dedicated double-tap / DAW menu trigger or menu-bar toggle / assigned Logic key command) immediately repaints all 24 TILES pads via `0xF0 0x7D 0x01 0x10` SysEx so the hardware always reflects the active Logic Pro page.
- Logic Pro → TILES Feedback:
  - Incoming MCU feedback (transport LEDs, track Mute/Solo/Arm/Select LEDs, Smart Control V-Pot rings, LCD track names/colors) and Scripter/Grid updates are diffed against the cached state and pushed as minimal `0x10` / `0x11` SysEx frames only when a cell changes—minimizing USB traffic and power draw.
  - Scripter MIDI FX (`TILES DISPLAY`) mirrors primary (Ch 1 green) and secondary (Ch 2 soft red) track notes onto `SENTIA TILES N (DAW)` in Melodic mode while leaving the audio/MIDI track signal chain bit-exact.

### Key Deliverables & Documentation

- `l_README.md`: Complete user & technical guide in Sentia's clear, concise style (no comparison to outside products, exact port routing, one-time Mackie Control + Controller Assignments + Scripter setup, multi-unit support, page guide, troubleshooting).
- `l_HISTORY.md`: Architectural rationale, protocol decisions, Logic Pro constraints (MCU vs. Lua Device Scripts vs. CoreMIDI virtual ports), and design log matching `daw-integration/HISTORY.md`.
- Native macOS CoreMIDI Bridge (Swift/C source + zero-dependency build script + LaunchAgent plist + unit tests for protocol/state translation).
- Logic Pro Assets: TILES DISPLAY Scripter JavaScript preset (MPE-safe passthrough + dual-channel note echo), Mackie Control / Auto-Assign setup preset & Lua identification helper, and one-command installer/uninstaller script.
