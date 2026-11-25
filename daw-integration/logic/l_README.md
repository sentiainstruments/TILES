# daw-integration/logic/

Software that integrates **SENTIA TILES** with **Apple Logic Pro** for
both **Live Loops** and **Tracks / DAW** control, with ultra-low latency,
zero-polling idle power efficiency, bidirectional LED and haptic
feedback, MPE expression, and Mackie Control Auto-Assign.

| Path | What it is |
|---|---|
| `bridge/CMakeLists.txt` | CMake build configuration defining `tiles_logic_core`, `tiles_studio_link`, `tiles_logic_manager`, `tiles_midi_logger`, `tiles_midi_monitor`, `tiles-logic-bridge`, and CTest targets. |
| `bridge/tiles_logic_core.{h,c}` | Portable C11 state machine for Live Loops, Smart Controls, Mixer banking, per-track Logic LED colors, Mackie Control Auto-Assign handshake, and per-pad MPE expression splitting. |
| `bridge/StudioLink.{hpp,cpp}` | Bidirectional Controller Assignment coordinator driving beat-quantized cues, Smart Control center-detent bumps, and timed SysEx haptic feedback. |
| `bridge/tiles_logic_bridge.cpp` | Native C++17 macOS CoreMIDI / CoreFoundation daemon (`tiles-logic-bridge`) exposing `TILES Logic MCU`, `TILES Logic Grid`, `TILES Logic Display`, and `TILES Logic MPE`. |
| `bridge/LogicManager.{hpp,cpp}` | C++17 macOS AppKit menu bar UI controller (`NSStatusBar`) for toggling between `Live Loops` and `Mixer` view states and opening the MPE `MIDIMonitor` window. |
| `bridge/MIDILogger.{hpp,cpp}` | Lock-free ring buffer and semantic decoder logging real-time hardware MPE, haptic SysEx (`0x10`/`0x11`), `CC 115–117`, and Scripter/Grid MIDI traffic. |
| `bridge/MIDIMonitor.{hpp,cpp}` | Lock-free graphical macOS AppKit debug window (`NSWindow` + `NSTextView`) inspecting real-time incoming/outgoing MPE, SysEx, Grid, and MCU streams. |
| `bridge/MIDI_Map.txt` | Bridge mapping reference for Touch/Tilt/Strike/Press -> Smart Controls, MPE compatibility, and haptic intensity scaling. |
| `bridge/build.sh` | Builds `tiles-logic-bridge` via CMake (or `clang++ -std=c++17 -O2` fallback) against `CoreMIDI`, `CoreFoundation`, and `AppKit`. |
| `scripter/TILES_DISPLAY.js` | Logic Pro Scripter MIDI FX preset (`TILES DISPLAY`) that passes MPE untouched while mirroring track notes onto TILES pads in melodic mode. |
| `scripter/TILES_Controller.js` | Logic Pro Scripter MIDI FX plugin for per-pad Auto-Assign Smart Controls, Live Loops `5×4` feedback, Mixer channel banking (`Previous/Next`, per-track Logic LED colors), and `MIDILogger` console toggle. |
| `scripter/LogicScripterUI.js` | Logic Pro Scripter MIDI FX plugin & HTML/CSS HUD (`Exo 2` / `Sora`, Sentia palette) with an interactive `6×4` 24-pad CSS mapper, Live Loops bank status, and Mixer page pad mapping. |
| `midi-device-script/TILES.lua` | Logic Pro MIDI Device Script descriptor listing ports, transport, Live Loops CCs, and per-pad expression CCs. |
| `MIDI_Map.txt` | Reference table of Smart Controls, per-pad expression, haptic cues/intensity, and mixer bank CCs. |
| `install/` | macOS installer (`install_logic.sh`), LaunchAgent (`com.sentia.tiles.logic-bridge.plist`), and standalone test kit builder (`build_standalone_installer.sh`). |
| `tests/` | C11, C++17, and Node.js verification suites (`run_tests.sh`) covering core state, StudioLink, LogicManager, MIDILogger, MIDIMonitor, LogicScripterUI, and `TILES_Controller.js`. |

Read `l_HISTORY.md` for the architectural rationale behind the CoreMIDI
bridge, dual-port isolation, and per-pad expression routing.

## Integration Architecture

```
SENTIA TILES Hardware (USB MIDI)
  ├── SENTIA TILES N MIDI (MPE Lower Zone: Ch 1 Master, Ch 2..9 Members)
  │     ├── Direct / TILES Logic MPE ──> Logic Instrument Track (MIDI Mono Mode)
  │     └── Passive Expression Tap ─────> tiles_logic_core ──> TILES Logic Grid (Ch 16 CC 20..23, Ch 2..9 CC 14..17)
  │
  └── SENTIA TILES N DAW (Control CCs 102..111 <-> LED/Haptic SysEx 0x10..0x12 + Display Notes)
        ├── TILES Logic MCU <──────────> Logic Control Surfaces (Mackie Control: Transport, Mixer, Smart Controls, Auto-Assign)
        ├── TILES Logic Grid <─────────> Live Loops Triggers & Scripter TILES_Controller.js (CC 24..30, CC 118..119, Notes 36..55)
        └── TILES Logic Display <──────> Scripter TILES_DISPLAY.js (Ch 1 Green Primary, Ch 2 Soft Red Secondary)
```

All routing inside `tiles-logic-bridge` runs event-driven on macOS CoreMIDI callbacks (`CFRunLoopRun()`) with zero polling timers, zero queuing overhead on MPE playback, and SysEx LED/haptic diff caching (`tiles_logic_core.c`) so USB traffic only fires on state transitions.

## USB MIDI Ports and Virtual Logic Endpoints

TILES exposes three USB MIDI ports (`firmware/src/midi/midi_ports.h`, unit
number `N` = `1..4` from `firmware/src/board/unit_id.h`):

- **SENTIA TILES N MIDI**: playable MPE Lower Zone (Channel 1 Master,
  Channels 2–9 Members), sustain pedal (CC 64), and clock/Start/Stop.
  Route this port directly to MPE instruments in Logic Pro with **MIDI
  Mono Mode (Common Base Channel 1)** enabled so per-pad pitch bend and
  channel pressure pass cleanly.
- **SENTIA TILES N Parts**: sequencer lanes (13–16), chords (12), bounce
  and games (11), drums (10), and Song tracks (2–9). Output only.
- **SENTIA TILES N DAW**: dedicated control-surface port for diamond
  transport, 5×4 pad grid events, LED SysEx feedback, haptic clip/playing
  state, and `TILES DISPLAY` note mirroring. Never present on DIN.

`tiles-logic-bridge` connects automatically to `SENTIA TILES N DAW` (for
control + SysEx LEDs/haptics) and `SENTIA TILES N MIDI` (for MPE
transmission, bidirectional sync, and optional Auto-Assign expression
splitting) while exposing four virtual CoreMIDI ports inside Logic Pro:

1. **TILES Logic MCU**: speaks Logic Pro's native Mackie Control Universal
   protocol (transport, Exclusive Track Record Arm, Mute, Solo, Select,
   8-channel Bank/Channel stepping, Smart Controls / Plug-In V-Pot LED
   rings, and Auto-Assign handshake).
2. **TILES Logic Grid**: dedicated port for Live Loops Cell/Scene
   Controller Assignments and per-pad Touch, Strike, Press, and Tilt CCs.
3. **TILES Logic Display**: virtual MIDI destination for routing `TILES
   DISPLAY` notes back onto TILES melodic pads (Channel 1 green primary,
   Channel 2 soft red secondary).
4. **TILES Logic MPE**: persistent bidirectional virtual MPE port
   forwarding the untouched MPE Lower Zone (`RPN 0x0006` = 8 member
   channels `2..9`) between `SENTIA TILES N MIDI` and Logic Pro so MPE
   tracks survive hardware replugs without losing port bindings.

## Setup: Bridge Application on macOS

The bridge is a native Apple-silicon (arm64) binary. `bridge/build.sh` pins
the architecture, and both the builder and installer refuse anything else.

### Option A: One-Command Install (Recommended)

1. Build and install the binary, LaunchAgent, Scripter presets, and MIDI
   Device Script from Terminal:

   ```bash
   chmod +x daw-integration/logic/bridge/build.sh \
            daw-integration/logic/tests/run_tests.sh \
            daw-integration/logic/install/install_logic.sh
   ./daw-integration/logic/install/install_logic.sh
   ```

   This compiles `tiles-logic-bridge` to `~/.local/bin/tiles-logic-bridge`,
   registers the LaunchAgent (`com.sentia.tiles.logic-bridge`, logs under
   `~/Library/Logs/SENTIA/`), copies `TILES_Controller.js`,
   `TILES_DISPLAY.js` and `LogicScripterUI.js` into
   `~/Music/Audio Music Apps/`, and installs the MIDI Device Script as
   `~/Music/Audio Music Apps/MIDI Device Scripts/Sentia/TILES/TILES.lua`.
   The LaunchAgent runs the bridge with its menu-bar item; pass `--headless`
   to install it without one, `--no-load` to install without starting it now,
   or `--stage DIR` to sandbox the whole install. `--uninstall` unloads the
   agent and removes the installed files.

### Option B: Standalone Test Installer Bundle

To package the entire Logic Pro integration (`bridge/`, `scripter/`,
`midi-device-script/`, `install/`, `tests/`, and `MIDI_Map.txt`) into a
checksum-verified standalone installer and macOS test kit under `dist/`:

```bash
chmod +x daw-integration/logic/install/build_standalone_installer.sh
./daw-integration/logic/install/build_standalone_installer.sh
```

This generates `dist/SENTIA-TILES-Logic-Installer.sh`,
`dist/SENTIA-TILES-Logic-Test-Kit/`, `dist/SENTIA-TILES-Logic-Test-Kit.zip`,
and `dist/SENTIA-TILES-Logic-Test-Kit.tar.gz`. Testers can run
`bash SENTIA-TILES-Logic-Installer.sh --verify-only`, `--stage <dir>`,
`--extract-only <dir>`, or double-click `Install_SENTIA_TILES_Logic.command`
on macOS without cloning the repository.

### Option C: Manual CMake Build & CLI Flags

```bash
cmake -S daw-integration/logic/bridge -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Supported CLI flags when launching `tiles-logic-bridge` directly:

- `--unit 1..4`: bind specifically to `SENTIA TILES N` and create numbered virtual ports.
- `--toggle-transport`: use toggle Play/Stop instead of Performance mode lockout while playing.
- `--no-expr-split`: disable passive MPE -> Auto-Assign CC generation on `TILES Logic Grid`.
- `--headless`: disable the macOS menu bar status item (`LogicManager`) for background-only LaunchAgent operation.
- `--monitor`: open the graphical real-time MPE/MIDI inspector window (`MIDIMonitor`) on launch (also toggleable from the `LogicManager` menu bar item).
- `--log-midi`: log real-time hardware MPE, haptic SysEx, and Scripter/Grid traffic off-thread via `MIDILogger`.
- `--log-filter <all|mpe|haptic|grid|mcu>`: enable `MIDILogger` filtered to a specific traffic category.
- `--verbose` (`-v`): print connected endpoints and diagnostics to `stderr`.

### Logic Pro Port & Control Surface Configuration (Once)

2. Open **Logic Pro -> Settings -> MIDI -> Inputs**:
   - Enable **TILES Logic MPE** *or* **SENTIA TILES N MIDI** (enable one of
     them, not both, so MPE notes never double; **TILES Logic MPE**
     persists across USB replugs and advertises Lower Zone width).
   - Enable **SENTIA TILES N Parts** (for multi-channel drum/sequencer recording).
   - **Uncheck** **SENTIA TILES N DAW** in MIDI Inputs so DAW control CCs
     never record into regions.
   - Enable **TILES Logic Grid** so Live Loops and Learn assignments see
     the grid and expression-split CCs.

3. Open **Logic Pro -> Control Surfaces -> Setup**:
   - Click **New -> Install...** and add **Mackie Designs -> Mackie Control**.
   - Set both **Output Port** and **Input Port** to **TILES Logic MCU**.
   - Logic Pro sends a Universal Device Inquiry + Host Connection Query;
     the bridge completes the Mackie handshake automatically and activates
     transport, mixer banking, Smart Controls, and Auto-Assign.

4. For **Live Loops** (optional one-time `Cmd+L` Controller Assignments):
   - Assign **TILES Logic Grid** Channel 16 CCs (`24..30`) or discrete
     notes (`36..55`, Channel 1) in Logic's Controller Assignments window
     to Live Loops cell trigger/stop/record/delete and scene trigger.

## Two Units at Once

Launch a dedicated bridge instance per connected TILES unit (`--unit 1..4`):

```bash
~/.local/bin/tiles-logic-bridge --unit 2 &
~/.local/bin/tiles-logic-bridge --unit 4 &
```

Each instance creates numbered virtual ports (`TILES Logic MCU 2`,
`TILES Logic Grid 2`, `TILES Logic Display 2`) so both boards control the
same Logic Pro project independently with zero channel collision.

## Transport (Diamond)

Mirrors `handle_diamond_transport()` in `firmware/src/services/op_mode.c`
using **Performance layout** by default (`diamond_ignored_while_playing =
true`; pass `--toggle-transport` for Toggle layout):

| Gesture | TILES DAW CC | Logic Pro MCU Action |
|---|---|---|
| Diamond click | CC 102 | Play (`0x5E`), ignored if Logic is already playing |
| Diamond hold 2 s, release | CC 104 | Record (`0x5F`), ignored if Logic is already playing |
| Circle + Diamond | CC 103 | Disarm Record + Stop (`0x5D`) |

## Three Control Pages (Live Loops, Smart Controls, Mixer)

Enter DAW Scene Launch mode on TILES (`Triangle -> Pad 6`, teal).
To avoid firmware conflicts (`Circle + Column 6` is reserved in
`op_mode.c`), switch pages using any of three methods:

- **Double-tap `Circle + Diamond`** (`CC 105` within `450 ms`) to cycle
  `Live Loops -> Smart Controls -> Mixer -> Live Loops`.
- **Single tap `Pad 6` (Column 6, Row 1)** when on Smart Controls or Mixer.
- **Send `CC 119` (`0/1/2` or `127` next)** to `TILES Logic Grid`.

### Page 1: Live Loops (`5×4` Grid + Scene Column)

- **Columns 1–5, Rows 1–4**: `5×4` window onto Logic Pro Live Loops cells;
  `-` / `+` pan the 5-track window (`CC 106` `0..59`) and automatically keep
  Logic's 8-channel Mackie bank aligned.
- **Column 6**: launches Scene Rows 1–4 (`TILES Logic Grid` Ch 16, `CC 28`).
- **Touch vs. Pressure Click** (firmware `op_mode.c`):
  - Bare capacitive **touch** gives tactile haptic confirmation only: a
    sharp click (`56 ms`) when a cell contains a loop (`HAS_CLIP`), and a
    continuous sustain buzz (`level 90`) while that clip is playing
    (`IS_PLAYING`).
  - **Pressure click** (press past actuation threshold): fires/stops the cell
    (`CC 24` / Note `36..55`) or stops a playing cell (`CC 109 -> CC 25`).
  - **Circle + Pressure click** on an empty slot (`CC 111`): exclusively
    record-arms that track via Mackie Control (`MCU_NOTE_REC_ARM_BASE`),
    emits `CC 26`, and opens TILES melodic mode (`SysEx 0x12`) so you play
    straight into the new Live Loops cell. `Circle + Diamond` (`CC 107`)
    finishes capture (`CC 30`).
  - **Circle + 3 s touch** (`CC 110`): emits `CC 27` to delete the cell and
    clears the pad LED/haptics.
- **Lighting**: uses Sentia colors (`#FF00FF` magenta scene launch,
  `#171D3A` ink dim base, per-clip 7-bit RGB SysEx `0x10` / `0x11`).

### Page 2: Smart Controls & Auto-Assign

- **Columns 1–4, Rows 1–2**: step Smart Controls / Plug-In V-Pots `1..8` up
  (`+6`) and emit assignable Ch 16 `CC 102..109` on `TILES Logic Grid`.
- **Columns 1–4, Rows 3–4**: step Smart Controls / Plug-In V-Pots `1..8`
  down (`-6`). Pad brightness reflects live Mackie V-Pot ring feedback
  (`0x30..0x37`).
- **Column 5**: switches Logic's Mackie assignment mode: `Plug-In / Smart
  Control` (Row 1), `Pan` (Row 2), `Send` (Row 3), `Track` (Row 4).
- **Column 6**: Row 1 advances to Mixer; Rows 2–3 step parameter banks;
  Row 4 toggles the MPE Per-Pad Auto-Assign Expression Splitter.

### Page 3: Mixer Banking

- **Columns 1–5**: five tracks aligned to the current window (`Mute` amber on
  Row 1, `Solo` gold on Row 2, `Rec Arm` coral on Row 3, `Track Select`
  Sentia magenta on Row 4), synchronized bidirectionally with Logic Pro's
  Mackie Control feedback LEDs.
- **Column 6**: Row 1 returns to Live Loops; Rows 2–3 step tracks left/right;
  Row 4 toggles Logic Cycle mode (`0x56`).

## Independent Touch, Strike, Press, and Tilt + Auto-Assign

Every TILES pad produces four independent physical gestures per voice:

1. **Touch** (capacitive pad contact & release)
2. **Strike** (piezo/force impact velocity on Note-On)
3. **Press** (continuous Z-axis channel pressure, `0xDn`)
4. **Tilt** (continuous Y-axis 14-bit per-note pitch bend, `0xEn`)

In **MPE mode** (Logic track Inspector -> `MIDI Mono Mode: On (Common Base
Channel 1)`), `SENTIA TILES N MIDI` delivers all four gestures polyphonically
per finger across Channels 2–9 with **zero bridge hops** in the audio path.

When **Auto-Assign Expression Split** is active (default on; toggle with
Smart Controls Col 6 Row 4 or `--no-expr-split`), the bridge passively taps
`SENTIA TILES N MIDI` without blocking or modifying the MPE instrument stream
and emits clean, Logic `Cmd+L` Learn-friendly CCs on **TILES Logic Grid**:

| Target Channel | Parameter | CC | Source MPE Gesture |
|---|---|---|---|
| Channel 16 (Focused Pad) | Touch Gate | CC 23 | Member Note-On (`127`) / Note-Off (`0`) |
| Channel 16 (Focused Pad) | Strike | CC 20 | Member Note-On velocity (`1..127`) |
| Channel 16 (Focused Pad) | Press | CC 21 | Member Channel Pressure (`0..127`) |
| Channel 16 (Focused Pad) | Tilt | CC 22 | Member Pitch Bend MSB (`0..127`) |
| Channels 2–9 (Per-Voice) | Voice Strike | CC 14 | Independent per-finger Strike |
| Channels 2–9 (Per-Voice) | Voice Press | CC 15 | Independent per-finger Press |
| Channels 2–9 (Per-Voice) | Voice Tilt | CC 16 | Independent per-finger Tilt |
| Channels 2–9 (Per-Voice) | Voice Touch | CC 17 | Independent per-finger Touch Gate |

Use Channel 16 CCs `20..23` when binding a global Smart Control or Plug-In
knob via Logic Pro's `Cmd+L` Learn, or Channels 2–9 CCs `14..17` when
automating up to 8 independent voices concurrently.

## Using the Scripter MIDI FX Plugins

### 1. `TILES_Controller.js` (Per-Pad Auto-Assign Smart Controls & Live Loops Feedback)

`TILES_Controller.js` runs inline inside Logic Pro's **Scripter** MIDI FX
slot (or on an External Instrument track routed to **TILES Logic Grid**)
and provides real-time control without modifying your MPE audio performance:

1. Add **MIDI FX -> Scripter** before your Software Instrument (or on a
   Summing Stack External Instrument leg pointed at **TILES Logic Grid**).
2. Click **Open Script in Editor** in Scripter, paste or load
   `scripter/TILES_Controller.js`, and click **Run Script**. Save it as a
   Scripter preset for one-click recall.
3. Choose an **Auto-Assign Expression** mode:
   - `MPE + Focused & Per-Voice`: forwards untouched MPE (`event.send()`)
     while emitting both Channel 16 focused-pad CCs (`20..23`) and per-voice
     Channels 2–9 CCs (`14..17`).
   - `MPE + Focused (Ch 16 Only)` / `MPE + Per-Voice (Ch 2-9 Only)` / `MPE Passthrough Only`.
   - Toggle individual **Touch**, **Strike**, **Press**, **Tilt**, and
     **Invert Tilt Axis** switches as needed during `Cmd+L` Learn so one
     gesture never steals another assignment.
4. Use **Bridge Page (CC 119)** (`Live Loops`, `Smart Controls`, `Mixer`) to
   switch active TILES pages directly from Logic Pro Smart Controls or
   track automation.
5. Enable **Live Loops Cell Feedback** so cell fire/stop/record/delete CCs
   (`Ch 16 CC 24..27`, `CC 29`) update the `5×4` pad matrix (`Notes 36..55`,
   `vel 127` playing pulse + sustain haptic buzz, `vel 32` queued blink,
   `vel 0` off), or use the **Feedback Cell Track / Scene / State** controls
   to test individual pads and haptics manually.
6. Enable **Auto-Sync Mixer Bank LEDs** and switch between 5-track mixer
   channel banks using **Active Mixer Bank (Fader Group)**, **Mixer Bank
   Previous (-5 Ch)** / **Mixer Bank Next (+5 Ch)**, or Mackie Control
   `0x2E`–`0x31` / `CC 113` / `CC 114`. Set **Bank LED Color Source** to
   `Logic Track Colors (Per-Channel)` and customize **Bank Ch 1..5 Logic
   Color** (or send `Ch 16 CC 32..36`) so TILES pad LEDs reflect each
   active bank track's color persistently across bank switches.
7. Toggle **MIDILogger System Console (CC 85)** and **MIDILogger Filter
   (CC 86)** to stream live decoded MPE and haptic events from
   `tiles-logic-bridge` to the system console (`stderr` / LaunchAgent log)
   without restarting the bridge.
8. Browse, save, and recall Logic Pro Smart Control presets in the companion
   app's **Project Workspace** tab (`companion-app/src/project/ProjectWorkspacePresets.tsx`).

### 2. `TILES_DISPLAY.js` (Melodic Notes Mirrored on TILES Pads)

1. Create a **Summing Track Stack** containing your Software Instrument
   track and a parallel **External Instrument** track whose MIDI Destination
   is **TILES Logic Display** (Channel: All).
2. Insert **Scripter** on the External Instrument track, load
   `scripter/TILES_DISPLAY.js`, and click **Run Script**.
3. Enable **VIEW** in Scripter and pick **Primary (Green - Ch 1)** or
   **Secondary (Soft Red - Ch 2)**.
4. Every note passes through `event.send()` untouched while lighting matching
   pads on TILES in melodic mode, with automatic Note-Off and CC 123
   cleanup when transport stops or VIEW disarms.

### 3. `LogicScripterUI.js` (24-Pad CSS Mapper, Live Loops Bank & Mixer HUD)

`scripter/LogicScripterUI.js` runs directly inside Logic Pro **Scripter**
(zero-latency `event.send()` passthrough + per-pad `Cmd+L` Learn pulse
emission) and renders an interactive HTML/CSS interface (`LogicScripterUI.renderHTML()`,
`LogicScripterUI.mount(el)`) featuring a `6×4` 24-pad CSS grid matching
the TILES hardware face alongside the active `5×4` Live Loops track bank,
`8`-channel Mackie bank alignment, and Mixer page pad mappings.

## Latency, CPU, and Power Design

- **Zero-latency MPE & instrument path**: Logic instruments either read
  directly from `SENTIA TILES N MIDI` or from `TILES Logic MPE` (which
  forwards the unmodified CoreMIDI `MIDIPacketList` before expression-split
  parsing with zero extra queuing hops).
- **0% idle CPU**: `tiles_logic_bridge.cpp` blocks inside macOS
  `CFRunLoopRun()` and uses CoreMIDI C callbacks (`MIDIClientCreate`,
  `MIDIInputPortCreate`), waking exclusively on USB MIDI interrupts or
  device hot-plug events with zero timers or polling loops.
- **SysEx diff cache**: `tiles_logic_core.c` diffs every visible pad against
  `sent_pads[][]` and `sent_scenes[]`, sending `0x10`/`0x11` SysEx frames
  only when cell color, clip presence, or play state actually changes.

## Troubleshooting

- Verify the bridge is running: `launchctl list | grep sentia`
- Check logs: `~/Library/Logs/SENTIA/tiles-logic-bridge.err.log`
- Run diagnostic verbose mode in Terminal:
  ```bash
  launchctl bootout "gui/$(id -u)" ~/Library/LaunchAgents/com.sentia.tiles.logic-bridge.plist || true
  ~/.local/bin/tiles-logic-bridge --verbose
  ```
- Stale port cache after firmware updates: open **Audio MIDI Setup ->
  Window -> Show MIDI Studio**, delete greyed-out **SENTIA TILES** entries,
  and reconnect USB.
