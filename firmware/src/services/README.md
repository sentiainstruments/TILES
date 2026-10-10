# services/

The behavior layer: turns raw sensor data into notes, expression, light
and haptics, and turns player gestures into modes and menus. Depends on
`drivers/`, `board/` and `midi/`. Every module has an `init()` called
once from `main.c` in boot order, and a `scan()` (or `service()`)
called once per main-loop pass. Nothing blocks.

`HISTORY.md` holds the full development log (every change, the
hardware findings behind it and the feedback that prompted it). Read it
before re-tuning a constant or reviving an approach that was dropped.

## Modules

**Input**

| Module | Role |
|---|---|
| `touch` | Both MPR121s → per-pad touched state. |
| `hall` | 24 TMAG5273s through the muxes → per-pad X/Y/Z and depth. Touched pads are read every pass; the rest round-robin. Rest baseline with slow drift correction. |
| `buttons` | 6 function buttons (debounced) and their LEDs. Owns both PCA9685s (shared with haptics). |
| `pedal` | 1/4" jack: sustain (CC64) or expression (CC11); sustain style SYNTH (CC64) or HOLD (TILES holds notes). |
| `power` | Power mode from the TPS2121 status pin + USB → budgets, LED ceiling, haptic voices, CV permission. |

**Playing**

| Module | Role |
|---|---|
| `expression` | Touch + Hall fusion: strike detection, velocity, pressure, per-note pitch bend from tilt, vibrato, release velocity, melodic harmonics, MPE channel allocation. |
| `note_map` | Pad → note: scales (picker order), key, octave, bass guitar frets, chord strip + melody grid. |
| `mpe_alloc` | Pure MPE channel choice / steal order (MMA RP-053 3.2). Tested. |
| `midi_channels` | The 16-channel map: fixed parts (drums 10, game 11, chord 12, sequencer 13-16), pool 2-9 shared by Song and the live MPE zone. Tested. |
| `midi_clock` | External MIDI clock or tap tempo → one pulse counter. |
| `cv_gate` | Monophonic MIDI-to-CV (1 V/oct, pressure, gate). Off unless external power and explicitly enabled. |
| `haptics` | Per-pad motor envelopes: kick, gap, sustain; touch pulse; voice ceiling. |

**Modes, menus, display**

| Module | Role |
|---|---|
| `op_mode` | Mode menu and the modes: melodic, sequencer, bass guitar, chord, Song (beta), Ableton, drums. Scale picker, pattern bank, captures, Live transport. The button map is in `op_mode.h`. |
| `drum_seq` | Drum mode: 16 or 32 steps (two pages, diamond flips) on columns 1-4, 8 drums on columns 5-6 (tap selects, push plays, circle + push rolls), banks of 8 in Drum Rack order, MIDI channel 10, saved through `storage/log_store`. The step engine, layout and save format are `drum_pattern` (no hardware; tested). |
| `octave_control` | "-"/"+" octave shift and transpose mode. |
| `expression_control` | Square: pitch bend toggle, expression menu; circle + square: MPE on/off. |
| `lighting` | Pad LEDs and underglow; power-derived brightness ceiling; idle colors by note role; indicator priority on the underglow. |
| `standby` | Screensaver animations and deep sleep; circle holds. |
| `boot_sequence` | Power-on animation (+ a Hall baseline recapture). |
| `game_mode` | Snake, Tile Breaker, Tile Drop, Paddle, Echo. |
| `pixel_font` | 4x4 font for the marquee and the transpose display. |
| `debug_mode`, `crash_indicator` | Crash recorder and watchdog; red underglow after a crash reboot. |

## Rules the modules share

- **Who owns the LEDs.** Normal play draws through `lighting.c`
  (`set_pad_press`). Anything that takes over the display (standby,
  boot animation, menus, games, sequencer/Song/Ableton views) claims
  it with `tiles_lighting_set_standby_active()` /
  `tiles_buttons_set_standby_active()` and draws with the standby
  setters. Crash, debug, pattern-confirmation and capture indicators
  write the underglow directly through a fixed priority chain in
  `lighting.c`.
- **Who owns the pads and buttons.** A module that uses the grid or
  "-"/"+" for its own purpose says so (`*_owns_pad_grid()`,
  `*_owns_octave_buttons()`, `tiles_game_mode_is_active()`), and the
  others stand down: `expression.c` starts no new strike,
  `octave_control.c` ignores "-"/"+", `main.c` skips standby. A module
  that stands down keeps its edge tracking current, so a held button
  isn't read as a new press when control returns.
- **Gestures are exact.** Combos check their complete button set (the
  game-mode 4-button hold, debug mode's 3-button hold and circle +
  square must never trigger each other). Clicks resolve on release.
  Menu picks take effect only after the finger lifts.
- **MIDI.** Channel map: `midi_channels.h`. MPE rules (zone declared
  only when idle, RPN 0 after every RPN 6, per-note setup before
  Note-On, Note-Off always sent, pedals on the Master Channel only):
  `expression.c` and `../midi/README.md`. No `printf()` right before a
  MIDI send, and no periodic printing in the loop.
- **Power and safety.** LED brightness and haptic voices come from
  `power.h` and never exceed it; CV/gate needs external power and an
  explicit enable; PCA9685 outputs stay disabled until configured.
- **Persistence.** User-tunable values are settings (`../profiles/`).
  Sequencer patterns and the Song library are saved to flash by
  `op_mode.c` (`../storage/flash_map.h`). Scale/key/octave, the active
  mode and running sequencer lanes survive a crash reboot
  (`__uninitialized_ram`), not a power cycle.
- **Unmeasured constants.** Most timing and feel constants are marked
  as first guesses in their comments; the measured ones say what
  capture they came from. Re-measure before retuning.

## Status

- **Song mode is beta**: built but not yet played on hardware.
- CV/gate and the DAC80502 driver are not yet verified on hardware.
- Per-pad Hall calibration curves, and Hall-depth wake from standby,
  are not built (see `../diagnostics/README.md`).
