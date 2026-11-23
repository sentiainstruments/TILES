# daw-integration/logic/ history

Design notes, architectural decisions, and implementation milestones for the
**SENTIA TILES** integration with **Apple Logic Pro**, matching the structure
and conventions of `daw-integration/HISTORY.md` and `daw-integration/ableton/`.

---

## 1. Why a Native CoreMIDI Bridge + Mackie Control + Grid Port

1. **No Python Remote Script runtime in Logic Pro**:
   Unlike host environments that load a third-party Python `ControlSurface`
   package directly over raw USB ports, Logic Pro integrates deeply with
   hardware controllers through two built-in mechanisms:
   - **Mackie Control Universal (MCU)** for zero-map transport, track
     record-arm, mute, solo, select, 8-channel track banking, Cycle, and
     bidirectional Smart Controls / Plug-In V-Pot ring feedback.
   - **Controller Assignments (`Cmd+L` Learn)** plus **Live Loops MIDI
     Triggers** and **MIDI Device Scripts** for custom matrix grids and
     discrete parameters.
2. **Firmware protocol untouched (`firmware/` isolation)**:
   `firmware/src/services/op_mode.c` already transmits standardized
   channel-1 CCs (`102..111`) on `SENTIA TILES N DAW` and accepts compact
   7-bit RGB SysEx (`0xF0 0x7D 0x01 0x10/0x11/0x12 ... 0xF7`) for per-pad
   LED state and touch/sustain haptic feedback. Keeping the exact same wire
   contract lets one board switch between DAWs without reflashing or
   forking firmware.
3. **Splitting MCU and Grid onto dedicated virtual ports**:
   Mixing Mackie Control note IDs (`0x00..0x7F` for strip LEDs and buttons)
   with user-learnable Live Loops cell notes/CCs on one port creates immediate
   collisions when Logic Pro Auto-Assign scans incoming MIDI. Separating
   `TILES Logic MCU` (dedicated to Logic's Mackie Control surface) from
   `TILES Logic Grid` (clean Channel 16 CCs + discrete notes `36..55` for
   Live Loops and Smart Control Learn) prevents every collision.

## 2. Independent Touch, Strike, Press, Tilt & Auto-Assign

- **Zero-hop MPE path**:
  Logic Pro Software Instruments in **MIDI Mono Mode (Common Base Channel 1)**
  consume `SENTIA TILES N MIDI` directly from USB CoreMIDI (`SENTIA TILES N
  MIDI` -> Instrument Track). Nothing bridges or re-packetizes that path,
  preserving full 14-bit Tilt pitch bend (`0xEn`), continuous Press (`0xDn`),
  Strike velocity (`0x9n`), and Touch gate per MPE member channel (`2..9`).
- **Passive Auto-Assign Expression Splitter**:
  Logic Pro's Controller Assignments (`Cmd+L`) only learn isolated CC/Note
  messages rather than polyphonic MPE channel pressure or 14-bit pitch bend
  streams. Tapping `SENTIA TILES N MIDI` passively inside `tiles-logic-bridge`
  mirrors each MPE voice onto `TILES Logic Grid` as independent CCs:
  - **Channels 2–9** (`CC 14` Strike, `CC 15` Press, `CC 16` Tilt, `CC 17`
    Touch) preserve polyphonic independence across up to 8 pads.
  - **Channel 16** (`CC 20` Strike, `CC 21` Press, `CC 22` Tilt, `CC 23`
    Touch) tracks the most recently touched pad so binding any Smart Control
    or Logic Plug-In parameter with `Cmd+L` takes one tap.

## 3. Page Cycling Without Firmware Modification

- Inspection of `scene_handle_shift_click()` in
  `firmware/src/services/op_mode.c` showed that `Circle + Column 6` is
  explicitly dropped on-device so scene pads never trigger accidental shifts.
- To support **Live Loops**, **Smart Controls**, and **Mixer Banking**
  pages cleanly without modifying firmware:
  1. Rapidly double-tapping `Circle + Diamond` (`CC 105` within `450 ms`)
     cycles pages anywhere (`Live Loops -> Smart Controls -> Mixer`), while
     a single `CC 105` still fires `Master Stop` (`CC 29`).
  2. When on Smart Controls or Mixer pages, tapping `Pad 6` (top-right
     column) steps cleanly to the next page in one gesture.
  3. External Logic key commands can send `CC 119` (`0/1/2` or `127`) to
     `TILES Logic Grid` to jump directly to any page.

## 4. Haptic & LED State Parity

- Firmware `op_mode.c` derives tactile pad haptics directly from the
  `TILES_FLAG_HAS_CLIP` (`0x01`) and `TILES_FLAG_IS_PLAYING` (`0x02`) bits
  inside `0x10` SysEx clip-state messages (`56 ms` crisp touch kick on loaded
  pads; `level 90` steady buzz on actively playing pads).
- Because Smart Controls and Mixer pages also render their pad states through
  the same `emit_sysex_clip()` diff engine (`tiles_logic_core.c`), active
  tracks (Muted, Soloed, Armed, Selected) and stepped Smart Control macros
  inherit crisp haptic feedback automatically, while `pad_cache_valid` /
  `sent_pads[][]` diffing eliminates redundant USB SysEx traffic.

## 5. Exclusive Track Arm & Cross-Bank Mixer Alignment Fix

- During design review of `tiles_logic_core.c`, two subtle multi-bank edge
  cases were resolved:
  1. **Cross-bank Mixer offsets**: Because TILES pans tracks one column at a
     time (`track_offset 0..59`) while Mackie Control banks in groups of 8
     channels (`0..7`, `8..15`, ...), a visible window spanning a bank
     boundary (for example `track_offset = 5`, column 4 = Track 9) could
     target the wrong MCU bank if alignment only tracked `track_offset / 8`.
     `sync_mcu_bank_to_index()` now aligns the exact target bank (`abs_track /
     8`) immediately before any Mute, Solo, Arm, or Select press.
  2. **Cross-bank Exclusive Record Arm**: When `Circle + Empty Pad` (`CC 111`)
     triggers new-clip recording in Live Loops, armed tracks in other MCU
     banks are banked cleanly and disarmed before arming the target track and
     emitting `SysEx 0x12` (`Open Melodic`).

## 6. Low Resource & Zero-Polling Execution Model

- Rather than an Electron/Node runtime or polling timer loop, the bridge
  runs as a lightweight C11 / C++17 native CoreMIDI binary
  (`tiles-logic-bridge`) inside a zero-CPU-idle `CFRunLoopRun()`.
- Every hardware/Logic packet runs inline on the CoreMIDI notification thread
  under a priority-inherited `pthread_mutex_t` (`PTHREAD_PRIO_INHERIT`)
  RAII lock, while `TILES Logic MPE` forwards incoming MPE `MIDIPacketList`
  frames directly before Auto-Assign parsing for sub-millisecond MPE
  delivery.

## 7. CoreMIDI Bridge Reliability & Apple Silicon Hardening

- **4-byte aligned `MIDIPacketList` unions**: Virtual and hardware sends
  place `MIDIPacketList` at the front of an aligned `union` so packet
  construction satisfies strict alignment on Apple Silicon (`arm64`).
- **Stale cached endpoint filtering**: `scanAndConnectHardware()` skips
  endpoints where `kMIDIPropertyOffline` is non-zero, avoiding cached
  greyed-out entries in Audio MIDI Setup after firmware updates or replugs.
- **Persistent virtual endpoint Unique IDs & MPE Zone Config**: `TILES
  Logic MCU`, `TILES Logic Grid`, `TILES Logic Display`, and `TILES Logic
  MPE` register deterministic `kMIDIPropertyUniqueID` values (`0x544C4Dxx`,
  `0x544C47xx`, `0x544C44xx`, `0x544C45xx`) with zero schedule advance
  (`kMIDIPropertyAdvanceScheduleTimeMuSec = 0`) and emit MPE Lower Zone
  MCM (`RPN 0x0006` = 8 member channels) on connect.
- **Async-signal-safe termination**: `SIGINT` and `SIGTERM` use libdispatch
  signal sources on the main queue to stop `CFRunLoopGetMain()` cleanly.

## 8. Menu Bar View Toggle (`LogicManager`)

- `bridge/LogicManager.{hpp,cpp}` provides a dedicated C++17 UI controller
  using macOS `AppKit` (`NSStatusBar` + `NSMenu` via `<objc/message.h>`) so
  `tiles-logic-bridge` stays pure C++17 with zero Objective-C++ build split.
- Users can toggle directly between **Live Loops** and **Mixer** view states
  from the macOS menu bar while hardware page switches (`Circle + Diamond`
  double-tap, `Pad 6`, or `CC 119`) keep the menu bar title and checkmarks
  synchronized on the main run loop (`--headless` disables the menu bar item).

## 9. Lock-Free Diagnostics (`MIDILogger` & Graphical `MIDIMonitor`)

- Printing or updating AppKit text views inside CoreMIDI read callbacks would
  risk blocking high-priority MPE delivery under `PTHREAD_PRIO_INHERIT`.
- `bridge/MIDILogger.{hpp,cpp}` and `bridge/MIDIMonitor.{hpp,cpp}` isolate
  observation behind fixed-capacity lock-free single-producer/single-consumer
  ring buffers with off-thread `dispatch_async_f` flushing to the main queue.
- `MIDILogger` decodes hardware MPE, haptic SysEx (`0x10`/`0x11`), `CC 115–117`,
  Live Loops Grid, and MCU traffic to `stderr`/LaunchAgent logs and can be
  toggled live from Scripter via `CC 85` (enable) and `CC 86` (category mask).
- `MIDIMonitor` opens an AppKit dark diagnostic window (`--monitor` or from the
  `LogicManager` menu) showing timestamped incoming/outgoing MPE, SysEx, Grid,
  and MCU events with filter toggles without touching Objective-C++ `.mm` builds.

## 10. Interactive 24-Pad Hardware Mapper & Bank HUD (`LogicScripterUI`)

- `scripter/LogicScripterUI.js` combines a native Logic Pro Scripter MIDI FX
  script (`PluginParameters`, `HandleMIDI` passthrough, `Cmd+L` Learn pulses)
  with a self-contained HTML/CSS interface (`Exo 2` and `Sora` typography,
  Sentia Ink/Periwinkle/Magenta palette).
- Mirrors the physical 6×4 TILES pad matrix and top navigation keys alongside
  live `5×4` track window status, `8`-channel MCU bank alignment, and per-pad
  CC/channel/expression-axis inspection.

## 11. Mixer Channel Banking & Per-Track Logic LED Colors

- `scripter/TILES_Controller.js` preserves parameter indices `0..16` for preset
  compatibility while adding `Previous (-5 Ch)` / `Next (+5 Ch)` bank stepping
  (`CC 114`), re-entrancy-safe `SetParameter` UI synchronization, **Bank LED
  Color Source** switching, and per-channel **Bank Ch 1..5 Logic Color** controls
  backed by a persistent 64-track color map (`trackColorOverrides`).
- In `bridge/tiles_logic_core.{h,c}`, `tiles_strip_state_t` stores per-track
  `has_custom_color` and `color_code` updated via `Ch 16 CC 32..36`, so
  `render_mixer_cell()` renders each track's Logic color independently via SysEx
  while keeping the default bank-group palette intact when custom colors are unset.

## 12. Smart Control Presets & Standalone Test Installer

- In `companion-app/src/project/`, `logicSmartPresets.ts` and
  `ProjectWorkspacePresets.tsx` integrate with `ProjectWorkspace` (`TilesProject`
  / `TilesPreset` state) so players can list, save, and recall Logic Pro Smart
  Control / MPE expression configurations directly from a workspace project.
- `install/build_standalone_installer.sh` builds a self-contained, SHA-256
  verified installer (`dist/SENTIA-TILES-Logic-Installer.sh`) and portable
  `.zip`/`.tar.gz` test kit with double-clickable `.command` launchers, `--stage`
  sandboxed installs, and `--verify-only` test execution without requiring a
  repository checkout.
