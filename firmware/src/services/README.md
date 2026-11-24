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
| `touch` | Both MPR121s → per-pad touched state. Sets a controller up again if it resets. |
| `hall` | 24 TMAG5273s through the muxes → per-pad X/Y/Z and depth. Touched pads are read every pass (X/Y/Z); the rest round-robin (Z only). One mux channel left open between reads. Rest baseline kept true by `rest_guard`; sensors that fail or reset are set up again while no pad is touched. |
| `buttons` | 6 function buttons (debounced) and their LEDs. Owns both PCA9685s (shared with haptics), and checks them every 250 ms: one found asleep (it lost power on its own) is set up again and the motors rewritten. |
| `pedal` | 1/4" jack: sustain (CC64) or expression (CC11); sustain style SYNTH (CC64) or HOLD (TILES holds notes). |
| `power` | Power mode from the TPS2121 status pin + USB → budgets, LED ceiling, haptic voices, CV permission. |

**Playing**

| Module | Role |
|---|---|
| `expression` | Touch + Hall fusion: strike detection, velocity, pressure, release velocity, melodic harmonics, MPE channel allocation; feeds `bend_core`. |
| `tune` | The feel constants (sensor averaging, the automatic per-pad depth gain, strike, velocity, pressure, key and touch timing, bend, vibrato, press path learner) as one live table every mode reads. `TUNE` over USB changes them until power-off; the calibration studio (`tools/studio/`) drives it. Defaults and why: `tune.h`, and `bend_core.c` for bend. |
| `bend_core` | Per-note pitch bend from sideways tilt, with its smoothing, baseline, deadzone and vibrato, in three modes (`expression.pitch_bend_mode`: `legacy`, `cosine`, `path`, the default), and each pad's press path learned from its strikes. No hardware; tested, and recorded captures replay through it in all three modes. |
| `note_map` | Pad → note: scales (picker order), key, octave, bass guitar frets, chord strip + melody grid. |
| `mpe_alloc` | Pure MPE channel choice / steal order (MMA RP-053 3.2). Tested. |
| `midi_channels` | The 16-channel map: fixed parts (drums 10, game 11, chord 12, sequencer 13-16), pool 2-9 shared by a claiming part (none now) and the live MPE zone. Tested. |
| `midi_clock` | External MIDI clock or tap tempo → one pulse counter. |
| `cv_gate` | Monophonic MIDI-to-CV (1 V/oct, pressure, gate). Off unless external power and explicitly enabled. |
| `haptics` | Per-pad motor envelopes: kick, gap, sustain; a click (kick only, for picks and signals no note-off ends); touch pulse; voice ceiling; kick starts 15 ms apart, never more than 30 ms late. |

**Modes, menus, display**

| Module | Role |
|---|---|
| `op_mode` | The mode menu (one column per family, each with its own colours; each mode's underglow its colour) and the modes: melodic, Duet (chord + melody), sequencer, bass guitar, Ableton, plus the switch into drums / Volca (`drum_seq`), Prism (`harmony`), Bouncing notes (`bounce`) and the physics modes (`physics`). Scale picker, pattern bank, captures, Live transport. Modes and keys: below. |
| `physics`, `physics_core` | The physics modes: Ripples (stones that drop every bar, rings playing on the sixteenths), Sand (a melody poured in sand: pile height = pitch, a triplet playhead), Orbits (planets round a sun in whole-beat periods), Newton's cradle (knocks on the beat for ever). One set of moves (what you place loops and stays lit, strike it again to remove it; circle + hold clears; square = the mode's other physics), the shared tempo pulled onto the clock's grid, MIDI channel 11. The simulations are `physics_core` (no hardware; tested). |
| `bounce`, `bounce_core` | Bouncing notes: 12 lanes (6 shown, circle + "-"/"+" scroll), a ball bouncing in each from a height of 1-4 pads, playing the lane's scale note when it lands; on the shared clock, MIDI channel 11. The physics is `bounce_core` (no hardware; tested). |
| `harmony`, `harmony_core` | Prism mode: a chord machine; a chord type and extensions on the right shape the root played on the left. Chromatic roots (columns 1-3); voicing (column 4), quality (column 5) and extensions (column 6), pressed by depth and latched onto the sounding chord; root bass; pedal strum; key lock from the scale picker (roots outside it dark and silent, chords unchanged). The chord maths is `harmony_core` (no hardware; tested). |
| `light_effects` | The light effects while playing (the Feel menu's Light effects menu): each note lights its pad and the pads around it, mechanical-keyboard style, drawn by `lighting` over the normal look. No hardware; tested. |
| `rest_guard` | A pad's rest position: slow drift, a re-zero when an untouched key reads pressed for 0.6 s, and silence for a pad touched while its rest was taken until it's seen at rest. No hardware; tested. |
| `strike_settle` | When a struck pad's note can go: as soon as its velocity is known (the key stopped going down, or the depth score is full), at most the 20 ms follow-through after the strike depth. Used by `expression` and `harmony`. No hardware; tested. |
| `play_state` | Mode, scale, key and octave saved to flash (through `storage/log_store`, never erasing while the clock runs) and restored at power-up. Tested natively. |
| `drum_seq` | Drum mode (and Volca mode: the same, each drum a C on its own MIDI channel 1-16, purple): 16 or 32 steps (two pages, diamond flips) on columns 1-4, 8 drums on columns 5-6 (tap selects, push plays, circle + push rolls), banks of 8 in Drum Rack order, MIDI channel 10, saved through `storage/log_store`. The first bank's 8 drums also play the sampler's 8 slots in TILES's own sound (`../audio/README.md`). The step engine, layout and save format are `drum_pattern` (no hardware; tested). |
| `octave_control` | "-"/"+" octave shift and transpose mode. |
| `expression_control` | Square: pitch bend toggle, expression menu; circle + square: MPE on/off. |
| `lighting` | Pad LEDs and underglow; power-derived brightness ceiling; idle colors by note role; indicator priority on the underglow; on touched pads, every mode: hue turns toward Sentia blue with pressure, brightness swells down with bend; LED intensity (Feel menu row 4) scales pads, underglow and button LEDs. |
| `standby` | Screensaver animations and deep sleep; circle holds. |
| `sound_menu` | Standalone mode's sound and effects menus on the diamond (see "Sound menus"). |
| `show_link` | Show takeover: a computer's light show (Show Control) streams the music as SysEx and, while it does, owns every LED (see "Show link"). |
| `boot_sequence` | Power-on animation (+ a Hall baseline recapture); its last pulse shows the device mode, and triangle during it switches it. |
| `device_mode` | Controller or standalone mode (below): read before USB starts; a switch saves and restarts. |
| `game_mode` | Snake, Tile Breaker, Tile Drop, Paddle, Echo. |
| `pixel_font` | 4x4 font for the marquee and the transpose display. |
| `debug_mode`, `crash_indicator` | Crash recorder and watchdog; red underglow after a crash reboot. |

## Modes and keys

Triangle opens the mode menu: one column per family, the first of each on
row 1, each mode in its menu colour (`MENU_SLOTS` in `op_mode.c`).

| Column | Modes |
|---|---|
| 1 | Melodic (magenta, default; its underglow is the device mode's colour), Duet (pink: chords on columns 1-2, melody on 3-6), Bass guitar (lilac: rows = strings, columns = frets, "-"/"+" shift the frets) |
| 2 | Sequencer (scarlet), Drums (orange steps, amber drum pads: 16/32 steps on channel 10, Drum Rack order) |
| 3 | Prism (blue: chord machine, `harmony`) |
| 4 | Ableton (teal: Session View clip grid), Volca (cyan: a drum per channel) |
| 5 | Bouncing notes (lime, `bounce`), Newton's cradle (steel), Sand (sand) |
| 6 | Ripples (water: soft crests with a fading wake; the stone, each ripple's root, ultramarine and breathing with the bar), Orbits (night sky) |

Every mode but melodic can be taken off the menu (settings `modes.*`);
removing the one in use returns to melodic. The current mode pulses white.
A pick takes effect when the finger lifts, so it can't play a note in the
new mode. Melodic, bass guitar and chord play through `expression`; the
sequencer, Ableton and drums draw the whole grid themselves. Song mode
(a 24-slot looper, beta) was removed on 2026-11-23: the Deck takes its job
(`docs/architecture/standalone-recorder.md`); a saved Song mode comes up as
melodic, and its setting `modes.song` stays, inert.

| Keys | Does |
|---|---|
| triangle | mode menu (toggle) |
| circle + triangle | scale picker (drums: the other step page); cancels a step edit or capture |
| diamond | Live transport: click = play/stop, hold 2 s = record. Standalone mode: the sound menu instead (below). Sequencer: pattern bank |
| circle + diamond | capture (sequencer); stop all clips in Ableton; end a Live recording. Standalone mode outside the sequencer: the effects menu |
| "-" / "+" | sequencer and drums: stop/start the viewed lane ("-" twice rewinds; "+" restarts the clock when none comes in); circle first = length (drums: previous/next 8 drums). Bass guitar: frets. Ableton: pan the 5-track window |
| circle | tap tempo when no external clock; flashes the beat |

**Sequencer.** 4 lanes at once, each on its own channel (`midi_channels.h`),
each playing one of 6 patterns from the pattern bank (row = lane, column =
pattern; circle + tap saves to flash, circle + hold 3 s deletes). Step view:
tap = arm, hold = pitch, keep holding = probability, circle + step =
ratchet. Up to 4 notes per step; a step is a sixteenth (6 clocks) from
`midi_clock`; starts land on the beat. Lanes keep playing in every mode.
Notes are fitted to the current scale as they play; storage never changes.
Capture records into the viewed lane at the running tempo, additively.

**Persistence.** Patterns are in flash
(`storage/flash_map.h`); mode, lane patterns and running lanes survive a
crash reboot (`__uninitialized_ram`, `tiles_op_mode_init()`).

## Rules the modules share

- **Who owns the LEDs.** Normal play draws through `lighting.c`
  (`set_pad_press`). Anything that takes over the display (standby,
  boot animation, menus, games, sequencer/Ableton views) claims
  it with `tiles_lighting_set_standby_active()` /
  `tiles_buttons_set_standby_active()` and draws with the standby
  setters. Crash, debug, pattern-confirmation and capture indicators
  write the underglow directly through a fixed priority chain in
  `lighting.c`. Above all of them (but the crash indicator) is the show
  takeover (`tiles_lighting_set_show_active()`): its own colours and
  written record, so the modes switching the standby path underneath
  can't take the LEDs back.
- **Who owns the pads and buttons.** A module that uses the grid or
  "-"/"+" for its own purpose says so (`*_owns_pad_grid()`,
  `*_owns_octave_buttons()`, `tiles_game_mode_is_active()`), and the
  others stand down: `expression.c` starts no new strike,
  `octave_control.c` ignores "-"/"+", `main.c` holds standby off (its
  idle minute starts when the owner lets go, not at the last touch before
  it took over). A module
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
  Sequencer patterns are saved to flash by
  `op_mode.c` (`../storage/flash_map.h`). Scale/key/octave and the
  active mode survive a power cycle (`play_state`, saved ~2 s after a
  change) and a crash reboot (`__uninitialized_ram`); running sequencer
  lanes survive only a crash reboot. No save erases flash while the clock
  runs or TILES's own sound rings (`tiles_usb_audio_is_quiet()`); saves
  while playing go out a page per pass (`../audio/README.md`).
- **Unmeasured constants.** Most timing and feel constants are marked
  as first guesses in their comments; the measured ones say what
  capture they came from. Re-measure before retuning.

## Controller and standalone mode

| | Controller (default) | Standalone |
|---|---|---|
| For | playing a computer or synth over MIDI/MPE | TILES's own sound (synths, drum sampler) |
| USB | MIDI, settings, console | the same plus a stereo 48 kHz audio input |
| Core 1 | off | the sound, and USB (`../audio/README.md`) |
| Colour (boot pulse, melodic underglow) | deep blue | Sentia magenta |

Tap or hold triangle during the boot animation to switch; the pulse turns
the new mode's colour at once. While playing, hold triangle alone for 10 s:
the pads and underglow flash the new mode's colour and TILES restarts into
it. Or the setting `device.mode`
(`controller` / `standalone`; `tools/tiles_control.py mode standalone`).
The USB descriptor differs, so a switch saves and restarts TILES; that
restart skips the animation (a flag in RAM a power cut clears). The mode
is read from the settings in flash before USB starts
(`tiles_settings_persist_peek()`), since the settings load later.

## Sound menus

Standalone mode, every mode but the sequencer (whose diamond stays the
pattern bank and capture): diamond opens the
**sound menu**, circle + diamond the **effects menu**; the same keys close
them, the other key switches. They set one of TILES's four synth tracks,
first the one the mode plays (melodic, bass guitar, Ableton: 2;
Duet, Prism: 3; bouncing notes and the physics modes: 4; drums: 1);
"-"/"+" step through the tracks, the underglow in the track's sequencer
lane colour. No DAW transport here.

- **Sounds:** the 96 preset slots (`../audio/presets.h`), 24 a page,
  row by row, a role a row, top to bottom: basses, plucks and leads,
  pads, bells and keys. Page 1: acid, reese, wobble, sub, solid, rumble /
  pluck, lead, stab, hoover, tine, super / haze, air, choir, cloud, dust,
  scatter / glass, pure, chime, shimmer, throb. Page 2: yoi, growl,
  riddim, pound, deep, punch / future, organ, dub, rave, screech, brass /
  drift, warm, tunnel, whisper, riser, halo / mallet, metal, bleep, drops,
  ember, keys. FM: solid, tine, chime, punch, brass, mallet; granular:
  cloud, dust, scatter, shimmer, whisper (scatter plays drum slot 8, or
  the last slot loaded; the delay rests while it is chosen). Circle +
  "-"/"+" steps through the pages holding a sound, the top row showing
  the page's number (a pad a page) for half a second; the menu opens on
  the page of the track's sound. A slot written from the computer shows
  its own sound; an empty one stays dark. The track's sound bright, the
  rest dim; a press past half travel picks and plays a short preview on
  the track (a sound whose wavetable is being built previews once it
  plays, ~0.2 s). A new sound brings its own effect amounts.
- **Effects:** a column each: reverb, delay, chorus, drive, crush, pump.
  Bottom row off, then a third, two thirds, full at the top, lit as a
  meter; a press sets that level, pressing deeper adds up to a third more.
- Planned: the First Edition's encoders (two, maybe three) assignable to
  any of these parameters while playing
  (`../../../docs/architecture/synth-engines.md`).

Colours and the preview notes are first guesses. Built 2026-11-15, not yet
tried on the board.

## Show link

SysEx from the computer, on the MIDI or the DAW port, manufacturer
`0x7D`, sub-ID `0x02` (the DAW script's scene messages are `0x01`); every
value 7-bit:

| Message | Bytes after `F0` | Meaning |
|---|---|---|
| Mode | `7D 02 01 on anim aR aG aB bR bG bB` | takeover on (1) or off (0); animation (below); the main and second colour |
| Look | `7D 02 03 glow len bright tail bed gain speed meter cR cG cB` | underglow: 0 white, 1 main, 2 second, 3 off; strobe length (20-400 ms); pad brightness (15-100%); tail, the pads' decays (0.25-4x, 64 = 1x); bed under the hits (0-3x); meter reach (0.5-2x, 64 = 1x); speed of ripple, rain and chase (0.4-2.5x, 64 = 1x); meter in the colours (1) or white/yellow/red (0); the accent colour |
| Frame | `7D 02 02 kick accent level b1 b2 b3 b4 b5 b6` | sent ~30 times a second and at once on a kick: kick strength (0 none), snare/hat accent (0 none), loudness, six bands low to high (one per pad column) |

While it's on and a message came within 2 s, the underglow strobes on
each kick (white falling toward the main colour, or in a colour) and the
pads play the animation: **0 meter** (each column a bar from its band,
like the standby equalizer), **1 pulse** (the grid in the main colour on
each kick over a dim bed of the second), **2 ripple** (a ring from the
centre on each kick), **3 sparkle** (pads sparking in the accent colour
on snares and hats, a wash on kicks), **4 rain** (drops fall from the top
row: kicks in the main colour, accents in the accent colour), **5 chase**
(a lit column steps across on each kick) or **6 spectrum** (the meter's
bars coloured main to second across the grid, peaks in the accent
colour). Without messages for 2 s it lets go and everything is as it
was. From the app's "on" until its "off" (or 30 min without a message)
standby and deep sleep wait, the pauses between songs included. Pads still play notes
underneath. The decays are first guesses.

## Status

- CV/gate and the DAC80502 driver are not yet verified on hardware.
- Per-pad Hall calibration curves, and Hall-depth wake from standby,
  are not built (see `../diagnostics/README.md`).
- **Next for expression:** first make pitch bend solid (bend logic in its
  own host-tested module, replaying recorded sensor captures). Only then
  add MPE slide (CC74) on the forward/back tilt axis: it follows the tilt
  while held and springs back to centre (64) when eased off; a setting,
  off by default; bend becomes left/right only.
