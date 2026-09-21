# services/

The behavioral layer: turns raw driver data into musical/expressive
events, and turns high-level intent into driver commands. Depends on
`drivers/` and `board/`; knows nothing about USB/MIDI transport.

Built: Hall scan, touch, lighting, buttons, pedal, note mapping (with
octave shift), the SW1/SW2 octave-shift button controller, SW5/square
("sentia")'s pitch-bend toggle + haptic-intensity shift and its
circle+square expression sub-menu + mute, real player-controlled
minigames (snake, brick breaker), touch+Hall expression fusion
(velocity/aftertouch/pitch bend), power source state, standby idle
animations (plus a deep sleep state after 20 minutes), a power-on boot
animation, and per-pad haptic feedback -- see Status below. Still
planned: per-pad Hall calibration, X/Y tilt -> pitch/timbre, storage
glue.

This is also where the legacy prototype's *behaviors* (scale modes, voice
stealing, standby animation, haptic confirm clicks — see
`../../../docs/reference/legacy-prototype-v1/`) get redesigned for 24 pads,
not its code.

## Status

- `lighting.h`/`.c` — done for the V1 default behavior in
  `docs/architecture/defaults-and-safeguards.md` ("LED color and
  brightness"): underglow solid white at its own fixed high brightness
  (230/255, deliberately independent of the power ceiling below -- only
  4 LEDs on that chain, negligible current impact even at full
  brightness, and it was reading as "basically not glowing" when it
  rode down with the USB-only ceiling); pads brighten to plain white
  toward the ceiling when `touch.c` reports that pad touched -- written
  immediately on a press-value change rather than waiting for the
  round-robin, so touch reads as responsive rather than laggy. Pad
  brightness ceiling reads live from `power.h`'s
  `tiles_power_get_state().led_brightness_ceiling_percent` (37% on
  USB-only, 90% once external power is confirmed -- see that entry
  further below for the fuller current-budget accounting behind those
  numbers) instead of a hardcoded constant -- underglow does not use
  this ceiling at all.
  **Idle (untouched) coloring by note role, added after real feedback
  that a uniform white grid was hard to read** -- "root should be blue
  and black keys shouldnt have led this in rest non pressed moment...
  when pressed the regular white illumination is fine." `write_pad()`'s
  non-standby branch now checks `services/note_map.h`'s new
  `tiles_note_map_is_root_pad()`/`_is_natural_pad()` for whichever pad
  it's about to render, but ONLY while that pad is untouched -- a touch
  always still collapses straight to plain white at `pad_level_for_press()`
  regardless of note role, unchanged. A natural (white) key keeps exactly
  the previous idle-white behavior at `idle_baseline_level()`
  (`TILES_LIGHTING_IDLE_BASELINE_PERCENT`, 25% -- raised from 10%, real
  feedback: "make all led brighter its hard to see"); a sharp (black) key
  goes to TRUE black -- a deliberate, narrow exception to this file's
  usual "pads never go fully dark" floor, scoped specifically to this
  readability distinction. Root checked first: a root pad can itself be
  a sharp/black key depending on the current key offset, and root's own
  color always wins over that (see `tiles_note_map_is_root_pad()`'s own
  comment on why root is exactly 2 fixed physical pads regardless of
  key, while natural/sharp classification genuinely does shift with the
  key). No new wiring needed for a live key change to repaint idle pads
  correctly -- `tiles_lighting_service()`'s existing round-robin already
  revisits every pad continuously regardless of whether its press value
  changed.
  **Root recolored to Sentia purple, and dimmed, after a first hardware
  pass** -- real feedback: "make the blue sentia purple for root notes
  but dim it a bit more than standard non pressed pads." Root now lights
  Sentia Instruments Magenta (#FF00FF, R and B channels only -- the same
  brand color `expression_control.c`'s sub-menu and `boot_sequence.c`'s
  final pulse already use) instead of plain blue, at
  `root_baseline_level()` (`TILES_LIGHTING_ROOT_BASELINE_PERCENT`, now
  15% of ceiling, raised from 6% -- real feedback: "make root note led
  also brighter," part of the same "make all led brighter" pass as the
  natural-key baseline above) -- deliberately kept LOWER than the
  natural-key baseline's 25%, a reversal of this feature's first pass
  (which had root brighter, reasoning that a single-channel color reads
  dimmer than three-channel white at the same level and wanted a clear
  landmark). Real feedback called for the opposite: a subtler root
  indicator, dimmer than the surrounding naturals rather than a bright
  highlight -- still true after this brightness pass, just a less
  extreme gap now that both are brighter in absolute terms. Unmeasured --
  a first attempt at "visibly dimmer, not so dim it disappears," not
  calibrated against real LED brightness/diffusion.
  Unmeasured -- root's brighter percentage and the natural/sharp
  distinction reading clearly at actual LED brightness/diffusion are
  both first attempts. Raised again, 15 -> 20, real feedback: "root note
  as well slightly brighter" -- still kept below
  `TILES_LIGHTING_IDLE_BASELINE_PERCENT` (25) so root stays visibly
  dimmer than a natural key at rest, same reasoning as every earlier
  round of this same constant.
  **Ceiling made dynamic/load-aware, then reverted -- a real dead end,
  worth keeping the history of.** Real feedback: "could we push the led
  celing a bit more safely?" First attempt: `pad_dynamic_scale()` summed
  every pad's CURRENT desired brightness (`pad_desired_rgb()`, the same
  standby/press/idle-root/natural/sharp branching `write_pad()` always
  had, pulled out so the budget estimate and the actual render could
  never drift apart) into a real projected mA figure and only scaled
  down if that would exceed the budget `led_brightness_ceiling_percent`
  (power.c) establishes for the current power mode -- mathematically
  current-safe, a couple of pressed pads could run close to full
  brightness while the rest of the grid sat idle. Real feedback after
  seeing it on hardware: "this led solution might look glitchy like we
  have unstable power. lets find a solution that doesnt include shifting
  brightness." Correct call -- a ceiling that reacts every frame to how
  many OTHER pads happen to be lit makes the WHOLE board's brightness
  visibly shift as notes are struck/released or an animation's lit-pixel
  count changes, which reads exactly like a brownout regardless of
  whether the underlying math is safe. Removed entirely; `write_pad()`
  is back to a single flat ceiling (`static_ceiling_level()`) that only
  changes when the power MODE itself changes, never per-frame.
  **Then, a fuller current-budget accounting**, real feedback: "calculate
  the safe range again to make sure, acountign for ics lights and
  haptics and sensors." LEDs are solid (16mA/pixel at full white
  including ~1mA controller overhead, per the board map's own
  `current_model` -- 448mA worst case across 28 pixels, matching the
  number this file was already built on). MCU + 24x TMAG5273 + 2x MPR121
  + I2C mux/expander ICs + the 6 function-button LEDs add up to roughly
  220mA of overhead this file's budget math never subtracted before
  (reasonable datasheet-typical estimates, not measured for this board).
  Haptic motor current is the real unknown -- both hardware docs flag it
  explicitly as unmeasured; a pessimistic small-ERM-motor estimate (~60-
  100mA each, up to 5 simultaneous voices on USB) could be 300-400mA
  alone, potentially the single largest term in the whole budget. On
  USB-only (500mA), that leaves little to no confirmed headroom beyond
  the EXISTING ceiling, so USB_ONLY/FAULT's `led_brightness_ceiling_percent`
  (power.c) was left at 37%, not raised. External power (2500mA) keeps
  ~1.8A of margin even under the same pessimistic haptics assumption, so
  EXTERNAL_ONLY/USB_AND_EXTERNAL's was raised 75 -> 90 there instead --
  see power.c's own comment for the full numbers. Haptic motor current
  measurement is the actual highest-priority unknown here now, not
  anything about LED brightness specifically.
  **Not done:** standby animations (needs its own design pass — see the
  defaults doc), Hall-driven (as opposed to touch-driven) brightness.
  **Idle brightness standardized, real feedback: "lets standardise led
  brightnbess, resting led should be brigher always. en its too dim."**
  `TILES_LIGHTING_IDLE_BASELINE_PERCENT` 25 -> 50, `TILES_LIGHTING_ROOT_
  BASELINE_PERCENT` 20 -> 40 (kept at roughly the same ~80% ratio to the
  natural-key baseline, not a fresh number). 50 specifically matches
  `services/op_mode.c`'s own `OP_SCALE_AVAILABLE_LEVEL` -- an already-
  validated "readable secondary brightness" constant from an earlier
  round of the identical complaint -- rather than picking a third,
  different "resting" percentage; genuine standardization on one shared
  value for "visible but not the active thing," not just another
  brightness bump.
  **Guitar/bass fret mode's own idle coloring added**, real feedback:
  "the lights should light up as frets for whatever marking make the
  most sence" -- see `note_map.c`'s own entry for the standard inlay-dot
  fret-marker convention this renders (`tiles_note_map_is_guitar_fret_
  marker_pad()`), checked in `pad_desired_rgb()` before the melodic root/
  natural logic (mutually exclusive with it). Unmarked frets reuse the
  SAME `TILES_LIGHTING_IDLE_BASELINE_PERCENT` the melodic idle state uses
  (this round's own standardization, above), tinted amber instead of
  white so guitar mode still reads as visually distinct; single-dot
  markers step up to 75%, octave (double-dot) markers to 100% -- mirroring
  how a real neck's double dots stand out more than the single ones.
  Pads STILL show plain white when actually touched, in guitar mode or
  out of it -- that branch is untouched, shared by both modes.
- `buttons.h`/`.c` — done for V1: reads all 6 function buttons
  (debounced, 10ms), lights each one's PCA9685-driven LED while (and
  only while) it's held -- the default for any button without a
  persistent function assigned. Owns both physical PCA9685 chips --
  `tiles_buttons_pca9685_for_addr()` is the accessor `haptics.c` (below)
  uses to reach the same two already-initialized instances rather than
  re-running `tiles_pca9685_init()` itself, resolving the ownership
  question this file used to flag as open.
  Also exposes a per-button LED override
  (`tiles_buttons_set_override_active()`/`_set_override_led()`), for a
  button whose default "LED follows press" has been replaced by some
  other persistent function -- `octave_control.c` (below) is the first
  user, for SW1/SW2. Distinct from the standby hooks (which apply to all
  6 buttons at once, only while idle): this is per-button, at any time.
  A button under an active override still gets its press/release
  tracked normally (`tiles_button_is_pressed()` keeps working), only the
  LED write is suspended; `tiles_buttons_set_standby_active(false)`
  (standby ending) skips re-asserting an overridden button's LED rather
  than clobbering it, and the override's own next scan repaints it.
  Fixed a real bug found from octave_control.c's real-hardware
  feedback: `set_button_led_level()`'s two exact endpoints (0.0/1.0,
  going through `tiles_pca9685_set_channel_full()`) had the active-low
  boolean backwards relative to `set_button_led()`'s own established
  `!lit` convention two lines above it -- level 0.0 ("off") was actually
  driving the pin low (lit), and level 1.0 ("solid") was actually
  driving it high (dark). The intermediate PWM path was correct the
  whole time (its own comment already worked through the inversion
  carefully); only the two full-on/full-off special cases were wrong.
  This is exactly why octave_control.c's buttons looked inverted
  (lit by default, dark when they should show "solid") while standby's
  own button dimming mostly didn't visibly show it -- standby's
  luminance values are rarely exactly 0.0 or 1.0.
  **New `tiles_buttons_resync_pca9685()`** -- real feedback: "pulling
  power plug killed haptics tho. power managment is not ready yet." Half
  of the fix (see `haptics.c`'s own entry for the other half): re-runs
  `tiles_pca9685_init()` for both chips (recovering from a possible
  rail-glitch-induced config reset during an actual power-source switch,
  without resetting this file's own button debounce/override state) then
  `refresh_all_button_leds()` (already existed) to repaint every button
  currently in the default "follows press" mode -- standby/override-
  governed LEDs don't need explicit repainting, they're redrawn
  continuously by their own owning module every frame regardless. Called
  from a `main.c` power-change callback, chips-first, before
  `haptics.c`'s own resync repaints motor state on top.
- `touch.h`/`.c` — done: reads both MPR121 controllers, derives each
  pad's touched state from its board-map touch route, pushes that into
  `lighting.c`'s per-pad brightness (touched = full ceiling, untouched
  = idle baseline). Touch state + lighting only -- MIDI now lives in
  `expression.c`, which reads `tiles_touch_is_touched()` itself rather
  than touch.c reaching into MIDI.
  Now prints an edge-triggered `[touch] pad N: touched`/`released`
  diagnostic on every state change -- real feedback that touch doesn't
  reliably wake `standby.c` from idle; this makes it directly observable
  whether the MPR121 ever registers the touch in question at all
  (hardware/EMI-level question) as opposed to registering it but
  something downstream not acting on it (a logic bug), pairing with
  `standby.c`'s new wake-source print. Temporary bring-up visibility,
  same as the other periodic prints in `main.c` -- replace with a real
  `usb_vendor/` diagnostics stream once that exists.
- `hall.h`/`.c` — done for V1: scans all 24 pads' TMAG5273 sensors
  through their Hall mux channels, storing raw XYZ plus a per-pad rest-Z
  baseline (captured once at init, re-capturable on demand via
  `tiles_hall_recapture_baseline()` -- see `diagnostics/README.md`'s
  `calibration.h`/`.c` for the serial-driven flow that calls it once the
  boot-time capture is known stale) and a derived depth magnitude
  (`tiles_hall_get_depth()`, `|z - baseline|`, sign-agnostic since the
  actual sensor polarity per pad is still unknown). Scan priority: a
  touched pad is read every `tiles_hall_scan()` call; untouched pads
  round-robin in the background -- a pure round-robin only reaches a
  given pad every ~24 calls, nowhere near fast enough to catch a 30-80ms
  strike, which `expression.c` needs. Structurally enforces "only one
  Hall mux channel across all three TCA9548A devices at a time." A pad
  whose sensor fails identify/init at boot is skipped by future scans
  rather than blocking the other 23.
  Also implements the gated slow drift tracker
  `docs/architecture/defaults-and-safeguards.md`'s "Pad baseline
  calibration and drift compensation" section already specced but never
  built (`update_drift_tracker()`, fed one pad per call from
  `tiles_hall_scan()`'s background round-robin pass only -- touched pads
  never reach it). Nudges a pad's baseline toward its current reading by
  ~1/128 of the gap (`DRIFT_SLEW_DENOMINATOR`) once that reading has
  stayed within `DRIFT_NOISE_THRESHOLD` (8 raw counts) of the *previous*
  background read -- not a fixed anchor -- for `DRIFT_DWELL_MS` (400ms).
  Comparing against the previous reading rather than a fixed one is
  deliberate: it's what lets genuine slow drift accumulate over many
  readings without any single step ever looking "unstable." One
  simplification from the spec's 3-condition gate: "no active MIDI note"
  isn't checked separately, since it's already implied by `touched ==
  false` given `expression.c`'s tight touch/note coupling (a note is
  never active on a pad this codebase reads as untouched). Reset (via
  `reset_drift_tracker()`) alongside both `tiles_hall_init()` and
  `tiles_hall_recapture_baseline()`, so stale pre-reset stability state
  never immediately nudges away from a freshly forced baseline.
  **Not hardware-verified:** `DRIFT_NOISE_THRESHOLD`/`_DWELL_MS`/
  `_SLEW_DENOMINATOR` are first-guess constants, not measured against
  real thermal drift over a session.
  **Not done:** deciding which raw axis is actually vertical press depth
  per pad (Z is assumed for all pads, unverified), per-pad calibration
  curve (offsets, dead zones, saturation margin), any use of X/Y
  (tilt/lateral). Actual achieved scan rate for touched pads is
  unmeasured -- see `firmware/README.md`'s known gaps.
- `note_map.h`/`.c` — done: maps a logical pad to a MIDI note. Physical
  layout confirmed against real hardware: pad 19 (bottom-left) is the
  lowest note, ascending left-to-right along the bottom row, then
  continuing (not restarting) up each row above -- see the file header
  for the full walk-through. Scale-mode architecture in place
  (`tiles_note_map_set_scale()`) with only chromatic implemented; adding
  a scale later means adding an interval table here, not touching
  `pad_config.c` or this layout logic. Verified by
  `firmware/test/test_note_map.c` against the exact examples given when
  the layout was specified.
  Also owns the octave shift (`tiles_note_map_set_octave_shift()`/
  `_get_octave_shift()`, +/-`TILES_NOTE_MAP_MAX_OCTAVE_SHIFT` (3), applied
  as +/-12 semitones on top of the scale-derived note) -- lives here
  rather than in `octave_control.c` (below) since it's a note-mapping
  parameter exactly like the scale, one owner for "how a pad's position
  becomes a MIDI note." The +/-3 bound was picked to match
  `octave_control.c`'s highest LED pattern and because it keeps the full
  24-pad chromatic span safely inside 0-127 with real margin either
  side -- it's a deliberate UX limit, not something the 0-127 note clamp
  ever actually has to catch.
  Also owns the key-center transpose (`tiles_note_map_set_key_offset()`/
  `_get_key_offset()`, 0-11 semitones, 0 = C, *wraps* rather than clamps
  since it's a position on the 12-note wheel not a magnitude) applied on
  top of octave shift, added for `octave_control.c`'s transpose mode
  (below) -- same "one owner for note-mapping parameters" reasoning as
  octave shift.
  Two new accessors added for `lighting.c`'s idle pad coloring (above):
  `tiles_note_map_is_root_pad()` and `_is_natural_pad()`. Root is purely
  positional -- `pad_degree(cfg) % 12 == 0`, exactly 2 of the 24 pads,
  always the SAME 2 pads regardless of the current key offset, since
  transposing shifts every pad's note by the same amount (the offset
  cancels out algebraically comparing a pad's pitch class against the
  root's). Natural-vs-sharp is the opposite -- genuinely key-dependent,
  looked up from a new 12-entry `s_pitch_class_is_natural[]` table
  indexed by the pad's CURRENT absolute pitch class
  (`tiles_note_map_get_note(pad) % 12`). `pad_degree()` itself was
  pulled out of `tiles_note_map_get_note()` into its own small helper so
  `is_root_pad()` could reuse the exact same row/col-to-degree math
  rather than duplicating it.
  **18 real scales added**, real feedback: "for melodic it toggles
  different scale modes... ionian, dorian, phrigian, lydian, mixo,
  aeolian, locrian, bluse major and minor, arabian, dim, combination
  dim, pentatonic major and minor, egyptian, whole tone, japanese
  miyakobushi, raga todi, the remaining ones are spaces for costume
  scales." Exactly 18 named + 6 reserved `CUSTOM_*` placeholders = 24,
  filling `services/op_mode.h`'s new melodic-mode scale-picker grid (one
  pad per scale) exactly. `tiles_note_map_get_note()`/`is_root_pad()`
  generalized from the old chromatic-only `degree % 12` math to a real
  per-scale interval table + octave-fold (`degree / table.count`,
  `degree % table.count`) that works for any table length 5-12 -- root
  pad count now genuinely varies by scale (verified: pentatonic major 5,
  whole tone 4, diminished 3, vs chromatic's fixed 2) rather than being
  hardcoded to the chromatic case. Interval tables are standard,
  documented definitions (not invented) -- see note_map.c's own comments
  for each. `TILES_SCALE_CHROMATIC` stays the boot default and is
  deliberately NOT one of the 24 grid slots (the named list already
  fills all 24 exactly); the 6 `CUSTOM_*` values are real enum values
  with no table yet -- selecting one is a UI no-op in
  `services/op_mode.c`, and `tiles_note_map_get_note()` falls back to
  chromatic internally if one somehow got selected anyway.
  `firmware/test/test_note_map.c` still passes unmodified (chromatic's
  table is `{0..11}`, count 12 -- the new generic fold reduces to the
  exact old formula for that specific case, verified no regression).
  **Guitar/bass fret mode added**, real feedback: "lets imoplenment for
  note mode a guitar fret mode for 4 stings with the structure of bass
  shapes, -+ change frets up and down. each row is a string each colum is
  a fret." A completely different note-mapping shape from the scale
  system above -- absolute string+fret, not scale-degree-relative --
  gated by a new `s_guitar_mode_active` flag `tiles_note_map_get_note()`
  checks FIRST, before the scale logic. Standard 4-string bass tuning
  (E1=28, A1=33, D2=38, G2=43 in MIDI note numbers -- each string a
  perfect 4th, 5 semitones, above the last, the real standard interval,
  not invented here). Row-to-string order follows standard TAB notation
  -- researched as the closest existing convention to this exact
  row=string/column=fret-or-time shape: a real fretboard/tab diagram
  always draws the HIGHEST string on the top line and the LOWEST on the
  bottom, mirroring the strings' physical arrangement viewed from above
  in normal playing position. Row 1 (closest to the function buttons) =
  G2, row 4 (farthest) = E1. Column 1-6 shows a sliding 6-fret window
  into a modeled 24-fret neck (`GUITAR_MAX_FRET`), controlled by
  `tiles_note_map_set_guitar_fret_offset()` -- `services/op_mode.c`'s
  guitar mode steps this with "-"/"+" (real feedback: "-+ change frets up
  and down").
  **Fret markers**: `tiles_note_map_is_guitar_fret_marker_pad()` reports
  the standard inlay-dot marker positions every real guitar/bass neck
  uses to help a player find their place without counting frets one by
  one -- single dots at 3/5/7/9 (and their next-octave repeats
  15/17/19/21), double dots at the octave points 12/24, not invented
  here. Marks the WHOLE column (all 4 strings) regardless of row, since a
  real neck's inlay dot spans the fretboard's width rather than sitting
  under one specific string -- `services/lighting.c`'s idle pad coloring
  is the one caller, see that file's own entry for the rendering.
  **Deliberately does NOT touch `services/expression.c`** -- that file's
  entire touch/velocity/pitch-bend/aftertouch/haptics pipeline already
  funnels through exactly one call to `tiles_note_map_get_note()` per
  strike (captured once into `active_note` and reused for both the
  note-on and its later matching note-off), so making note_map.c itself
  guitar-aware was enough to make the WHOLE existing pipeline play
  guitar-mapped notes with zero changes elsewhere -- confirmed by reading
  through expression.c's actual call site before relying on this design,
  not assumed. `services/op_mode.c`'s guitar mode is architecturally much
  closer to "melodic mode with a different note-mapping function" than to
  a custom instrument because of this -- see that file's own "Guitar/bass
  fret mode" section for the rest.
- `octave_control.h`/`.c` — done for V1: the default function of SW1
  ("-") and SW2 ("+") is octave shift down/up, one octave per press
  (fires on release, not the press itself -- see this entry's own
  "solo-step race" fix further below for why), driving `note_map.c`'s
  shift above.
  Claims both buttons permanently via `buttons.c`'s new per-button
  override -- their LEDs stop following "lit while held" and instead
  show the active direction's shift magnitude via a distinct pattern,
  all three built from one shared "pulse" shape (a raised-cosine bump,
  smooth rise and fall, no square-wave edge anywhere) so the family
  reads as coherent rather than three unrelated effects: magnitude 1 is
  that pulse repeating evenly forever with no rest; magnitude 2 is two
  of that pulse back to back then a dim (not fully dark) rest, then
  repeat; magnitude 3 is exactly magnitude 2's shape with one more pulse
  appended before the same rest. The inactive direction (and both, at
  shift 0) stay dark.
  Reworked three times from real hardware feedback: first pass slowed
  magnitude 2's period and replaced magnitude 3's original hard on/off
  blink-then-solid-hold with smooth pulses, but left the three
  magnitudes as separately-shaped animations that "didn't pulse evenly
  with each other"; second pass rebuilt all three from the single shared
  `pulse_unit_level()` building block above so magnitude 2 and 3 are
  literally the same burst shape (just one more repeat before the rest)
  and magnitude 1 uses that same shape continuously instead of being
  flat solid; third pass slowed both periods again after that rebuild
  still read as too fast overall (magnitude 1 especially -- a pulse with
  no rest between repeats reads as noticeably faster than the same
  period would in a burst, so it now runs on its own longer
  `OCTAVE_PULSE1_PERIOD_MS` rather than sharing the burst unit's period).
  Meant to become a general-purpose modifier eventually (held for other
  menus/combos, per the product's own direction) -- this module only
  implements the V1 default function, not a generic modifier framework;
  that's real future work, not built speculatively now.
  **Not hardware-verified:** every pattern timing constant
  (`OCTAVE_PULSE1_PERIOD_MS`/`OCTAVE_PULSE_UNIT_MS`/`OCTAVE_PULSE_REST_MS`/
  `OCTAVE_PULSE_REST_LEVEL`) is a first guess -- this rework is reasoned
  through against the *previous* version's real feedback, not itself
  seen on real hardware yet.
  **Transpose mode** is the first real instance of that "general-purpose
  modifier" direction: holding SW1+SW2 together (a quick "click them
  together," ~`TRANSPOSE_COMBO_HOLD_MS` 120ms, not a long hold) toggles
  it. While active both LEDs pulse together (magnitude 1's continuous
  pulse shape, same phase on both) instead of showing the octave
  pattern, and "-"/"+" step `note_map.c`'s key-center offset (wraps
  0-11/C-B) instead of the octave shift. The pad grid is claimed (same
  standby-active rendering-ownership pattern `standby.c`'s own
  animations and `game_mode.c` use) and shows the current key's
  natural-note letter in caps, centered, via the shared font in
  `pixel_font.h`/`.c` (below). A sharp key alternates the letter with a
  plain amber "+"-shaped cross as a second flash, since a 4x4 glyph has
  no room to draw "#" -- a proper plus contained in its own 4x4 box
  (2-column vertical arm, full 4-row height; 1-row horizontal arm, 4
  columns wide, centered), reworked from an initial version whose
  horizontal arm spanned the full 6-wide grid (real feedback: "the
  horizontal line is too long"). The letter always shows first for a
  moment (re-anchored on mode entry and on every key change) so the
  flash is never caught mid-cross. Underglow goes dark while this is
  showing. `tiles_octave_control_is_transpose_active()` lets `main.c`
  skip `standby.c`'s idle scan while this owns the pad grid, mirroring
  the existing `game_mode.c` gate.
  **Defers to game mode, manual screensaver scrolling, and the
  expression sub-menu:** this module's scan runs unconditionally every
  tick with no gate of its own, and `game_mode.h`'s minigames (below),
  `standby.h`'s manually-entered screensaver (hold SW6/circle for 6s),
  and `expression_control.h`'s expression sub-menu (visible whenever
  square is held alone, or left open sticky, see that entry below) all
  need SW1/SW2 left alone -- without a check here, every in-game,
  scroll, intensity-adjustment, or sub-menu-dismiss press would *also*
  silently fire an octave or transpose step underneath.
  `tiles_game_mode_is_active()`, `tiles_standby_owns_octave_buttons()`,
  and `tiles_expression_control_owns_pad_grid()` are all checked at the
  top of the scan: while any is true, this module only keeps its
  press-edge tracking current and does nothing else (button-LED writes
  were already a no-op during game mode, see `game_mode.h`'s entry below
  for why; during manual screensaver, `standby.c` itself claims the same
  standby-active flag for the same reason; the expression sub-menu
  doesn't touch SW1/SW2's LEDs at all, so they simply keep their normal
  default "lit while pressed" behavior throughout).
  **Solo-step race fixed after a real-hardware pass** -- real feedback:
  "the transpose menu erases the previous selected transpose setting...
  transpose menu enter and exit accidentally triggers octave up or down
  on enter or exit because button [presses land] before entering menu
  because simultaneous press is impossible." True: a human can never
  press SW1/SW2 in exactly the same tick, so whichever button registered
  first used to read as a genuine solo press (`both_held` still false at
  that exact instant) and fire a real octave-shift or key-offset step an
  instant before the second button joined and the combo took over -- on
  BOTH entering and exiting transpose mode, since `s_transpose_mode`'s
  current value at that instant decided which one fired. Exiting was the
  more damaging case: a stray key-offset step landing right as the combo
  closed, invisible until the next time transpose mode reopened and the
  letter had silently drifted -- read by real feedback as the mode
  "erasing" the setting, though it was really an undetected off-by-one
  each cycle, not a reset. Fixed by moving the solo step from the PRESS
  edge to the RELEASE edge, gated on whether that specific press ever
  became part of `both_held` at any point during its hold
  (`s_minus_became_combo`/`s_plus_became_combo`, reset on that button's
  own fresh press, set true the instant `both_held` is seen regardless of
  which button led) -- the same "click vs. long action" shape
  `services/expression_control.c`'s own square-button handling already
  established. Whichever button happens to land first no longer matters:
  its release, once the combo has formed, is already flagged and fires
  nothing.
  **Now claims the pad grid from `expression.c` too** -- real feedback:
  "playing the grid in transpose menu exits the menu." Diagnosis:
  `services/expression.c`'s per-pad strike state machine had no idea
  transpose mode existed, so touching pads while adjusting the key still
  fired real notes/haptics underneath the letter display -- unlike the
  expression sub-menu, which was already correctly suppressing new
  strikes via `tiles_expression_control_owns_pad_grid()`.
  `tiles_octave_control_is_transpose_active()` is now ALSO checked
  there, alongside that existing check, so a tap meant only to read/set
  the key never also plays a note (a pad already mid-strike or held when
  transpose mode opens is still left alone to finish normally, same as
  the sub-menu's own rule).
  **Not hardware-verified:** the combo-hold threshold, both flash
  durations, the cross's row/column placement, and the amber accent
  color are all first-pass judgment calls, not measurements. The
  solo-step-race and pad-grid-ownership fixes above are untested on real
  hardware too.
  **New guitar mode also yields "-"/"+"** -- this file's own scan-gate
  now checks `services/op_mode.h`'s `tiles_op_mode_owns_octave_buttons()`
  instead of `tiles_op_mode_owns_pad_grid()` directly. Broader than the
  original check on purpose: guitar mode needs "-"/"+" ownership (its own
  fret-window shift) WITHOUT the "suppress new note strikes" side effect
  full pad-grid ownership carries elsewhere (`services/expression.c`
  checks `owns_pad_grid()` for exactly that, and guitar mode's whole
  design depends on real notes still playing normally through that same
  pipeline -- see `note_map.c`'s own entry). Sequencer mode is unaffected
  either way, since it already legitimately owns the whole grid.
- `expression_control.h`/`.c` — done for V1: SW5 (square, "sentia")'s
  function-button role, corrected from an earlier pass that built the
  same behaviors onto circle by mistake -- see `standby.h`'s entry above
  for that history. Real feedback identified the physical button
  directly: "our shift and power button is circle. sentia is square
  button. sentia acts as a secondary shift for a single feature for now,
  everything else shift is power/sleep/round."
  **Square alone:** a short click (press+release, before either
  hold-gesture below fires) toggles `services/expression.c`'s pitch bend
  on/off via `tiles_expression_toggle_pitch_bend()` -- real feedback:
  "when you press sentia button once it turns on and off the pitch
  bend." From the very first instant square is held alone (no threshold
  -- see the sub-menu below), SW1/SW2 step the expression sub-menu's
  row-1 (haptics) COLUMN one at a time
  (`handle_square_shift_input()`/`step_haptics_column()`), through the
  exact same `apply_row()` path a sub-menu pad tap uses -- real feedback:
  "holding just sentia acts like a function shift for modifiers -/+ for
  haptics," followed, after a first real-hardware pass, by: "theres no
  continuity between menu and arrow keys control for haptics... any
  changes that affect those 4 parameters should always be reflected on
  the menu." (An earlier version stepped `services/haptics.c`'s scalar
  directly via a now-removed `tiles_haptics_adjust_intensity()`, which
  could drift to a value matching no defined column -- see that file's
  own entry above.) `tiles_expression_control_owns_pad_grid()` is checked
  by `octave_control.c` (see its entry above) so a shift press -- or a
  press meant only to dismiss a sticky sub-menu, see below -- doesn't
  *also* silently step the octave/transpose key.
  **Square alone, momentary preview + 2-second sticky lock: the
  expression sub-menu.** Real feedback, after the button-identity
  correction: "it should be when you hold shift and sentia the pads
  become sliders one for each of the four rows... row one is haptics,
  row two is pitch bend, 3 is the remaining axis and 4 is aftertouch,"
  then, after a combo-based version (holding circle+square together) was
  tried on real hardware: "lets change [that] to hold square for 3
  seconds alone to toggle that menu," followed by a second hardware pass:
  "momentary press should open menu as well but after 3 seconds the
  toggle should happen," then later "hold sentia for 2 sec not 3." The
  sub-menu is now visible from the instant square is held alone
  (`s_submenu_visible`, `set_submenu_visible()`) -- a momentary preview
  that disappears again on release below
  `EXPRESSION_SUBMENU_TOGGLE_HOLD_MS` (2000ms, lowered from 3000).
  Reaching that threshold
  while still held alone (edge-latched via `s_submenu_toggle_fired`, same
  `*_fired` shape `standby.h`'s own circle-hold uses; the alone-streak
  restarts if circle ever joins mid-hold) flips `s_submenu_sticky`
  instead, locking it visible after release too -- closing again only on
  the next such 3-second hold, or via the dismiss-button shortcut below.
  Visible, it claims the pad grid (`tiles_lighting_set_standby_active()`,
  the same rendering-ownership pattern `octave_control.c`'s transpose
  mode and `standby.c`'s own animations use) and turns it into 4 rows of
  6-pad sliders: row 1 (nearest the buttons) = haptics intensity, row 2 =
  pitch bend sensitivity, row 3 = reserved for a future "remaining axis"
  (Y) feature (the selected column is stored, `SUBMENU_ROW_Y_AXIS`, but
  not yet consumed anywhere), row 4 (bottom) = aftertouch sensitivity.
  Tapping any pad in a row (a capacitive touch rising edge, read directly
  via `tiles_touch_is_touched()` rather than through
  `services/expression.c`'s own state machine, which is suppressed for
  new strikes the whole time this is visible -- see that file's entry
  above) selects that column (1-6, left to right) as the row's new
  level, applied immediately through each parameter's own setter -- the
  exact same `apply_row()` the square-alone "-"/"+" shift above uses for
  row 1, so the two controls can never disagree about what's currently
  applied.
  **Dismissing a sticky sub-menu without re-toggling it.** Real feedback:
  "any of the 4 function buttons should exit that menu it shouldnt have
  to be untoggled." `poll_dismiss_button_edge()` tracks SW1-4's press
  edges unconditionally every tick (so the tracking is never stale by
  the time it matters -- see its own comment on why that separation from
  *acting* on an edge is deliberate); a fresh edge on any of them while
  the sub-menu is sticky AND square is NOT currently held clears
  `s_submenu_sticky` immediately. Scoped to that passive-viewing case
  specifically -- while square IS actively held, SW1/SW2 are busy
  running the haptics shift above, so a press there keeps adjusting
  rather than exiting. A plain short click of either square or circle
  dismisses a sticky sub-menu too -- real feedback: "make sure we can
  exit from menu with single click of sentia or shift/power as well."
  Square's own click checks `s_submenu_sticky` first and closes it
  instead of running its normal pitch-bend toggle when it's set (the
  more likely intent while the menu is up); circle has no competing
  click action, so its click always closes a sticky sub-menu, muted or
  not. Both are guarded by their own `s_*_press_had_long_action` flag
  (mirroring the existing pitch-bend-suppression one) so the incidental
  release right after a circle+square mute-combo hold doesn't also read
  as a dismiss click.
  Every row shares one mapping function, `piecewise_column_value()`: a
  3-anchor piecewise-linear curve (column 1, column 4, column 6), column
  4 landing on exactly each parameter's previous fixed default (so a
  fresh boot behaves identically to before this feature existed) --
  real feedback: "we want defaults to be the sweet spot on column 4, and
  5 and 6 are extra strong or sensitive." Row 1 (haptics) ranges 0.0 (col
  1, a real OFF -- see `haptics.h`'s entry above) -> 0.72 (col 4) -> 1.0
  (col 6) -- 0.72 deliberately below the physical duty ceiling so columns
  5-6 have real headroom to be "extra strong" rather than column 4
  already sitting at 1.0 with nowhere to go. Row 2 (pitch bend,
  `tiles_expression_set_pitch_bend_sensitivity()`) and row 4 (aftertouch,
  `tiles_expression_set_aftertouch_sensitivity()`) both range the
  opposite direction -- smaller is *more* sensitive for both underlying
  values -- 0.40/1300 (col 1, least sensitive) -> 0.20/900 (col 4 --
  0.20 is pitch bend's current default, after real feedback pushed it
  0.15 -> 0.30 -> back down to 0.20, see `expression.h`'s entry above for
  the full history; 900 is aftertouch's original real-calibrated
  default, unchanged) -> 0.10/600 (col 6, most sensitive). All of row
  1/2/4's non-default anchors are unmeasured first attempts, not felt or
  captured on real hardware yet.
  The selected pad in every row lights Sentia Instruments Magenta
  (#FF00FF, the same brand color `boot_sequence.h`'s final pulse phase
  uses); every OTHER pad in the grid sits at `SUBMENU_UNSELECTED_LEVEL`
  (a low 0.06 fraction of full magenta, not fully dark) -- real feedback,
  after a first hardware pass showed unselected pads reading at the same
  brightness as the selection: "make the non selected default light pad
  very dim." Row 1's column-1 OFF position is the one exception: real
  feedback, also from that pass: "the lowest setting is off and should be
  blinking when active in menu to show its off" -- `row_column_is_off()`
  flags that one row/column combination specifically (no other row has a
  true "off" position at column 1, just "least sensitive"), and
  `render_submenu()` blinks that pad between full magenta and the same
  dim baseline every unselected pad sits at (`OFF_INDICATOR_BLINK_
  PERIOD_MS`, 500ms) rather than showing it solid, so the sub-menu itself
  communicates "off," not just "lowest."
  **Available during mute, but doesn't silently escape it.** Real
  feedback, after mute originally suppressed the sub-menu entirely: "when
  mute is on the menu is unavailable and we dont want that." Opening,
  viewing, and adjusting the sub-menu (pad taps or the "-"/"+" shift) now
  all work identically regardless of mute state -- merely visiting it
  never changes mute. `apply_row()` (the one funnel every real edit goes
  through) is also the one place that can tell a genuine value change
  apart from a no-op re-selection or a clamped step; when it sees a real
  change while `s_mute_active` is set, it turns mute back off right
  there (the same `tiles_haptics_set_muted()`/`tiles_expression_set_
  muted()` calls the mute combo's own toggle uses) -- real feedback:
  "changes to the menu should override expression mute and turn it off
  but if the menu is opened just to check settings and no change is made
  then mute stays on." `tiles_expression_control_owns_pad_grid()` lets
  `main.c` skip `standby.h`'s idle scan while the sub-menu is visible,
  the same way it already does for `game_mode.h` and `octave_control.c`'s
  transpose mode.
  **Circle+square held 3 seconds: expression mute.** A separate combo,
  independent of the sub-menu above -- holding SW6 (circle) and SW5
  (square) together for `EXPRESSION_MUTE_HOLD_MS` (3000ms, its own edge
  latch, `s_mute_fired`) toggles a sticky mute that persists until the
  same 3-second combo hold toggles it off again (or an in-menu change
  auto-unmutes it, above) -- real feedback: "a shortcut that disables
  everything and leaves basic midi... it acts like a mute."
  `tiles_haptics_set_muted()` and `tiles_expression_set_muted()` (see
  those files' own entries) do the actual work: pitch bend and poly
  aftertouch stop being computed/sent, every haptic effect stops firing
  with every active motor cut immediately, and note-on/off/velocity are
  completely unaffected. Square's own pitch-bend-click and the sub-menu's
  momentary-preview/sticky-lock hold (not the sub-menu's *contents*, see
  above) are suppressed while muted, since square's LED is busy showing
  the mute indicator instead. That indicator
  (`render_square_led()`/`mute_blink_level()`) is a repeating two-blink
  pattern (`MUTE_BLINK_ON_MS`/`_GAP_MS`, 120ms each) followed by a rest
  at `MUTE_REST_LEVEL` (0.5, medium brightness, `MUTE_REST_MS` 900ms) --
  real feedback: "sentia should become a blinking light with a two blink
  pattern and rest at medium brightness to indicate expression functions
  mute." Outside of mute, square's LED shows `SQUARE_LED_HELD_LEVEL`
  (1.0, matching default press feedback) while physically held (alone or
  as part of the combo), and a persistent `SQUARE_LED_TOGGLE_ON_LEVEL`
  (0.8, "not by a lot" dimmer, same reasoning the reverted circle version
  used) glow once released, while pitch bend is on; dark otherwise.
  **Defers to game mode.** `services/game_mode.h`'s Pong minigame uses
  SW5/SW6 as its own live right-paddle up/down controls -- this module's
  entire scan bails immediately (keeping only its own press-edge
  tracking current) whenever `tiles_game_mode_is_active()` is true, and
  symmetrically `game_mode.c`'s own SW3+SW4+SW5+SW6 entry combo
  (`gm_combo_held()`) refuses to fire while this module's sub-menu
  already owns the pad grid, so the two mutually-exclusive features can
  never both claim the board at once -- see `game_mode.h`'s entry below.
  **Real bug found and fixed after hardware testing:** "we have an issue
  when going into game mode the sentia and circle combo is getting
  triggered by the 4 button combo." `tiles_game_mode_is_active()` above
  only guards AFTER game mode has actually toggled on; the real gap was
  on the way IN (or back out) -- `gm_combo_held()`'s entry gesture is
  SW3+SW4+square+circle, and since square+circle are two of those four
  buttons, this module saw the exact same circle+square state a
  deliberate two-button gesture would produce for however long fingers
  take to land or lift all four (never perfectly simultaneous), reading
  it as a fresh short combo/press/release of its own mute or sub-menu
  gestures. Fixed by skipping this module's handling entirely whenever
  SW3 AND SW4 are ALSO currently held -- that specific four-button state
  is `game_mode.c`'s reserved combination and never a legitimate use of
  circle/square alone or together on their own.
  **Timing lowered, real feedback:** "hold sentia for 2 sec not 3" --
  `EXPRESSION_SUBMENU_TOGGLE_HOLD_MS` 3000 -> 2000 (see that constant's
  own comment for why `EXPRESSION_MUTE_HOLD_MS`, a different gesture,
  deliberately stayed at 3000).
  **Not yet hardware-verified at all** -- none of the above (the
  button-identity correction, the momentary/sticky sub-menu and its
  column-to-value mapping, the dismiss-button shortcut, the dim/off
  indicators, the magenta color choice, the mute-availability change, or
  the mute pattern itself) has been tried on real hardware yet.
- `pixel_font.h`/`.c` — done for V1: a shared tiny pixel font, a fixed
  4x4 grid per glyph (one pixel per pad row 1-4, 4 columns wide), used
  by both `standby.c`'s scrolling marquee animation and
  `octave_control.c`'s transpose key-letter display above -- pulled out
  of `standby.c` (where the glyphs used to live as a one-off,
  hand-guessed set) so both callers share one already-checked font
  instead of each guessing its own. Format: one byte per glyph column
  (always 4 columns except `SPACE`), bit0 = row 1 (top) ... bit3 = row 4
  (bottom). Covers exactly the letters needed -- A-G (the seven natural
  note names) plus I/L/S/T (for the "TILES -" marquee message), a dash,
  and a space -- not a full alphabet, since nothing else uses this yet.
  `tiles_pixel_font_glyph_for_note_letter()` is the runtime lookup
  `octave_control.c` needs for a variable key letter; `standby.c`'s
  marquee references the glyphs directly since its message is fixed.
  Reworked once: the original version used variable-width 3-column
  glyphs with a separate gap column between letters and had a real
  mistake (E and F were nearly indistinguishable, E was missing its
  bottom bar); this version moved to a fixed 4x4 grid styled after the
  user-supplied "FOUR BIT" reference font (bold, blocky, geometric) --
  true monospacing, no gap column needed. `N` (only ever needed for
  "SENTIA") was later deleted along with that word -- see the marquee's
  own entry above.
  **Not hardware-verified:** every glyph is hand-drawn specifically for
  4x4 (there's no off-the-shelf font at exactly this size to have copied
  instead) and hasn't been seen lit yet -- legibility at actual LED
  brightness/diffusion is unconfirmed.
- `game_mode.h`/`.c` — done for V1: real, player-controlled minigames --
  a genuinely separate feature from `standby.c`'s autonomous snake/
  brick-breaker animations (below), which stay exactly what they were
  (ambient, self-playing, no player). Hold SW3 (triangle) + SW4
  (diamond) + SW5 (square) + SW6 (circle) together for ~0.7s to toggle a
  menu on/off -- SW1 ("-")/SW2 ("+") are deliberately excluded from that
  combo since they're reserved as in-game controls, matching
  `octave_control.h`'s own note above about "-"/"+" becoming
  general-purpose modifiers eventually. The menu shows pad 1 (green) for
  Snake, pad 2 (orange) for Brick Breaker, pad 3 (cyan) for Tetris, and
  pad 4 (blue) for Pong; touch any to launch it.
  Snake: starts 2 segments long (real feedback: 3 felt cramped on a
  board this small), SW1/SW2/SW3/SW4 = left/right/up/down (absolute
  direction, not relative turning; reversing straight into the snake's
  own neck is ignored, the standard rule), eats a pulsing food dot to
  grow, wraps around the grid's edges (friendlier than instant
  wall-death on a board this small) and dies only on self-collision.
  Brick Breaker: SW1/SW2
  move the paddle -- otherwise identical physics to `standby.c`'s
  autonomous version.
  Tetris: SW1/SW2 move the falling piece left/right, SW3 rotates it (2
  states per piece, not full 4-state SRS, and no wall kicks -- see the
  `gt_` section's own comment for why), SW4 hard-drops it, gravity also
  steps it down automatically (`GT_STEP_MS`). A custom 5-piece set, NOT
  the standard 7 tetrominoes -- real feedback that full tetrominoes (up
  to 4 wide/tall) were too big for a board this size, one piece able to
  span the entire width or height. Reworked to: a 1-cell dot, a 2-cell
  domino, a 3-cell straight tromino ("long piece," capped at 3 instead
  of 4), a 3-cell corner tromino, and a compact 2x2 square (4 cells but
  small footprint, so it stays the largest). `gt_piece_def_t` grew a
  `num_cells` field (1-4) since pieces are no longer always exactly 4
  cells -- every loop over a piece's cells (`gt_fits()`, `gt_lock()`,
  `render_tetris()`) uses that field instead of a hardcoded 4.
  `gt_clear_lines()` shifts everything above a full row down (handles
  multiple simultaneous clears in one bottom-up sweep) and returns how
  many rows cleared, so `gt_lock()` can trigger a brief, fast-toggling
  white underglow strobe (`GT_LINE_CLEAR_FLASH_MS`/`_TOGGLE_MS`) only
  when something actually cleared -- real feedback: "the underglow must
  flash white dramatically when a line is cleared." Topping out (a
  freshly spawned piece already collides) ends the round with a plain
  red blink instead of the usual red/purple (real feedback: "when game
  is lost it should flash red") -- `gm_start_round_end()` grew a
  `red_only` parameter for this, since Tetris is the only game needing a
  different round-end color than snake/brick breaker.
  Pong: two players, one board -- column 1 is the left paddle (SW1 up,
  SW2 down), column 6 is the right paddle (SW5 up, SW6 down -- the
  mirror pair to SW1/SW2; **unverified** whether "square" is actually
  the button the user meant by "the other one next to circle"). Both
  paddles 2 pads tall and white; the ball is a single blue dot, checked
  before either paddle in the render order so it draws on top during a
  bounce (same precedent as brick breaker's ball). First to
  `GP_WIN_SCORE` (2) points wins -- real feedback: Pong wasn't tracking
  who was winning at all. A non-winning miss (`gp_point_scored()`)
  flashes underglow white briefly and re-serves immediately, staying in
  `GM_STATE_PLAYING_PONG`; this part deliberately still doesn't go
  through the shared round-end flow below, since a rally on a board
  this small can end in a couple of seconds and bouncing to the menu
  every point would be disruptive. The score itself renders on each
  side's own movement-control buttons via `render_pong_score_buttons()`
  -- a breathing glow, not flat-on: 0 points both dark, 1 point the "up"
  button (SW1 left / SW5 right) glows, 2 points both glow -- "one point
  one control lit, 2 points both buttons on." Reaching the winning
  score is different from an ordinary miss: real feedback was "don't
  reset the game immediately, return to the game menu," so
  `s_gp_match_over` freezes the ball/paddles (rendering just stops being
  updated, no special-case needed) for `GP_MATCH_END_DISPLAY_MS` while
  the winner's controls keep glowing, then `tiles_game_mode_scan()`
  calls `gm_enter_menu()` directly -- handled locally in the
  `GM_STATE_PLAYING_PONG` branch rather than through the shared
  `GM_STATE_ROUND_END` state, since Pong's "flash" here is on the button
  LEDs, not underglow.
  Every other game's end (snake self-collision, brick breaker won/lost)
  flashes underglow red/purple for ~2s, then returns to the menu.
  Claims the same standby-active rendering path `standby.c`'s own
  animations and `boot_sequence.c` use -- correct and sufficient for LED
  *writes*: `buttons.c`'s per-button override for SW1/SW2
  (`octave_control.c`) already goes transparently inert under that same
  flag (see `buttons.c`), so no changes were needed there for this
  module to freely drive SW1/SW2's LEDs too. That inertness only covers
  writes, though -- `octave_control.c`'s button *reads* run
  unconditionally every scan with no gate of their own, so without a
  fix every in-game left/right press would *also* silently step the
  octave or transpose key underneath the game. Fixed by having
  `octave_control.c` check `tiles_game_mode_is_active()` itself and skip
  all of its own action logic (while keeping its press-edge tracking
  current) whenever a game owns the buttons -- see its own entry above
  and file header. `main.c` skips calling `tiles_standby_scan()` entirely
  while `tiles_game_mode_is_active()` is true, so standby's own idle
  timer can't fire mid-game and fight this module over the same
  rendering path -- both being triggered by real button presses means
  standby's idle timer gets a fresh reset the moment control hands back
  either way, so there's no "immediately idle right after leaving a
  game" edge case from skipping its scan while active.
  Deliberately NOT sharing state/logic with `standby.c`'s autonomous
  versions of the same games, even though the physics/rules mostly
  overlap -- an AI-driven idle loop and a player-driven game are
  different concerns likely to evolve independently (control remapping,
  more games, difficulty tuning), and forcing them through one shared
  implementation now would couple things that don't need to be coupled.
  **Not hardware-verified at all:** the entry-gesture hold duration,
  every step-timing constant, the wrap-around-vs-wall-death choice for
  snake, Tetris's simplified 2-state rotation, Pong's SW5/SW6 mapping
  (the "square"-vs-whatever-the-user-meant guess above), and the
  round-end/line-clear/point flash timings are all first attempts, none
  seen on real hardware yet.
  **A 5th minigame, Simon Says, added at menu pad 5** -- real feedback:
  "lets implement another mini game, simon says, so a haptic and led
  patern appears on the pads and the user has to follow, start with a
  simple one pad at a time but it gets exponentially longer like the real
  simon says." Grows by exactly one step per round (linear) -- real Simon
  Says itself grows linearly, not exponentially, and "the real simon
  says" is the actual behavioral anchor in that sentence; "exponentially"
  is read as colloquial ("gets long enough to feel hard fast"), not
  literal doubling. Any of the 24 pads can appear (repeats allowed, same
  as real Simon Says), each step also gets an independently-random color
  from a 6-color palette (real feedback: "the sewqurnce has different
  colors flashing") -- pure visual variety, not encoding anything.
  Deliberately the one game in this file that reads `tiles_hall_get_depth()`
  instead of `tiles_touch_is_touched()` for its actual gameplay input --
  real feedback: "for this mini game touch pads are disabeled but push
  pads are used for pattern receation" -- using the same real-hardware
  `MIN_STRIKE_DEPTH_DELTA`-style threshold (`GSIM_PRESS_DEPTH`, 300)
  expression.c's own real note strikes are calibrated against, not a new
  guess. Playback fires both an LED flash AND a `tiles_haptics_trigger_kick()`
  pulse together per step -- real feedback: "the haptics play a big part
  on this one giving you the vibraions paired with light to inditcate the
  correct light." A correct reproduction re-flashes that exact pad's own
  pattern color (real feedback: "when player plays the colors do come
  back when pressed") plus a lighter haptic echo; a wrong pad calls the
  same shared `gm_start_round_end(now_ms, true)` plain-red flash Tetris's
  own "when game is lost it should flash red" precedent uses, then
  returns to the menu like every other game's round end -- completing a
  full pattern instead stays IN Simon Says, extends the pattern, and
  loops back to a fresh playback after a brief pause. Not
  hardware-verified -- none of the timing constants, the palette, or the
  press-depth threshold's real-hardware feel have been tried yet.
  **Real RNG-quality bug found and fixed**, real feedback: "is simon says
  generating unique patterns every time? it should do that." It wasn't
  reliably -- see `standby.c`'s own entry for the deeper fix (this
  firmware's one shared `rand()`/`srand()` stream used to seed from
  boot-time milliseconds, which could land nearly identical across boots
  given how deterministic this board's boot sequence timing is).
  `gsim_new_game()` now ALSO reseeds with fresh `get_rand_32()` hardware
  entropy right as each game starts, on top of that fix -- extra
  insurance specifically for this feature.
- `expression.h`/`.c` — done for V1: touch+Hall fusion. Touch remains
  the authoritative note on/off *timing* gate (more reliable to detect
  than inferring press/release from Hall depth alone); Hall gates
  *whether* a note fires at all (`MIN_STRIKE_DEPTH_DELTA`, real measured
  depth travel required, not touch alone), supplies velocity (elapsed
  time to reach that travel -- see below for why this isn't
  acceleration-based), and drives ongoing aftertouch (from press depth
  while held), sent as MIDI poly key pressure. Per-pad state machine:
  IDLE -> AWAITING_STRIKE (touched, not yet committed) -> NOTE_ON, with
  AWAITING_STRIKE cancelling back to IDLE (no note sent) if released
  before a real press is ever detected.
  This module went through five rounds of real-hardware feedback to get
  here, each catching a genuine bug or forcing a real architectural
  change rather than a constant tweak:
  1. **"Touch is triggering notes not press velocity"** -- the original
     safety-timeout fallback fired a note at floor velocity purely
     because touch had lasted a fixed window, with no check on whether
     the pad had moved at all. First fix (`MIN_STRIKE_DEPTH_DELTA` = 15,
     then 30) still failed on hardware: **a second bug**, the reference
     depth that threshold measured against was read immediately at
     touch-down from whatever `hall.c` had cached from its slow
     background round-robin (an untouched pad isn't scanned every call);
     comparing that stale reading against a fresh one once touch
     switched the pad to `hall.c`'s every-call priority scan could read
     as "movement" from nothing more than ordinary drift. Fixed via
     `has_touch_start_depth`: the reference (`touch_start_depth`) is now
     set from the *first fresh sample actually taken at the fast rate*,
     not a read taken before that rate even starts.
  2. **"Any touch still triggers midi... it's all the same velocity"**
     -- rather than guess a third threshold blindly, an `[expression]`
     debug print was captured across ~140 real touches. The data showed
     Hall depth reads in steps of 16 raw counts, with bare capacitive
     contact clustering overwhelmingly at depth_delta 32 (a long tail to
     96) while deliberate presses reached 192-736 (of the ~900-unit
     full-press range) -- a real, measurable gap. `MIN_STRIKE_DEPTH_DELTA`
     raised to 150, sitting in that gap with margin both sides.
  3. **"Contact has to be broken for retrigger... strong hard presses
     don't trigger anything"** -- a fresh capture showed genuine
     touch-and-release cycles (150-200+ms, not a graze) with no note-on
     between them: real hits silently dropped. Root cause: "pressed" was
     computed from the depth reading at one instant, not the peak
     reached during the touch -- a fast strike can spring back (or end
     contact) before that instant ever sees it past threshold. Fixed via
     `peak_depth_delta`, the running max since touch began, checked
     instead of the instantaneous value; `commit_on_release` added so a
     touch that ends after the peak already cleared threshold still
     fires, instead of being discarded. Separately, `RETRIGGER_ARM_DEPTH_DELTA`
     (40) added real retrigger-without-lifting: once a *held* note's
     depth eases back down within 40 of the pad's original touch-down
     reference (not just down from that note's own peak), it's treated
     as release-and-retouch -- note-off fires and the pad drops back
     into `begin_awaiting_strike()` for a genuinely fresh strike, gated
     by `RETRIGGER_GRACE_MS` (50ms) so a strike's own post-impact
     rebound doesn't immediately misfire this. Deliberately conservative
     (close to "as light as the original touch," not just "eased off
     the peak") since looser risks cutting off an ordinary sustained
     hold's natural pressure fluctuation -- the acknowledged flip side
     is a genuinely gradual fade-out could also trip this early.
  4. **"Hard fast press won't trigger... velocity curve is bad... no
     haptic preview"** -- three separate findings. First, a capacitive
     touch bounce: a hard impact can momentarily break contact for a
     couple of ms, which this module read as a real release, restarting
     strike detection right at the strike's own peak and missing it
     entirely. `TOUCH_DROPOUT_GRACE_MS` (12ms) bridges this by treating
     touch as active for a short window past the last raw `true`
     reading (`touch.c`'s own prints stay undebounced -- this tolerance
     is purely an expression-layer interpretation). Second, the velocity
     curve was a flat linear scale on acceleration, which can't make
     both "light stays quiet" and "hard reliably maxes out" true at
     once. Third, haptic feedback was reported missing -- reviewed
     against three clean debug captures and found no code-level cause
     (see `haptics.h`'s entry); a confirmation print was added at the
     actual motor-drive call instead of guessing further.
  5. **"Max sudden push does not trigger notes properly... light presses
     trigger randomly hard... the logic and measurement method is not
     working"** -- this was the real verdict on acceleration itself as
     the velocity measure, not just its constants. A double-difference
     over only 3 Hall samples is extremely sensitive to exactly which
     samples land where and how far apart, on top of depth's own coarse
     16-count quantization -- and a genuinely fast, hard strike is
     precisely the case most likely to blow past `MIN_STRIKE_DEPTH_DELTA`
     in only 1-2 samples, never reaching a stable 3-sample estimate at
     all (matching "sudden push doesn't trigger"), while a slower press
     got whatever accel its particular sample spacing produced with no
     reliable relationship to actual force (matching "randomly hard").
     **Velocity is now elapsed TIME, not acceleration** -- the same
     technique real weighted-action MIDI keyboards and drum pads use: a
     dual-contact-point timing measurement, not a differentiated
     position signal. `touch_start_sample_ms` marks the first fresh Hall
     sample after touch begins; `strike_time_ms` is set exactly once,
     the moment `peak_depth_delta` first crosses `MIN_STRIKE_DEPTH_DELTA`,
     as the gap between those two Hall-sample timestamps.
     `MIN_STRIKE_SAMPLES` and the whole 3-sample accel history
     (`update_strike_history()`, `t[]`/`d[]`/`peak_accel`) are gone
     entirely, along with `MAX_STRIKE_WINDOW_MS` -- with nothing left to
     "wait for," a real press now commits the instant it's measured,
     whether that took 3ms or 300ms. `velocity_from_strike_time()`
     replaces `velocity_from_peak_accel()`: `STRIKE_TIME_MAX_VELOCITY_MS`
     (10ms, at or below which velocity pins at 127 -- the same
     deliberate plateau below the fastest possible strike the prior
     curve also aimed for) and `STRIKE_TIME_MIN_VELOCITY_MS` (150ms, at
     or above which velocity floors at `MIN_VELOCITY`) bound a power
     curve (`VELOCITY_CURVE_EXPONENT`, still > 1, still suppressing the
     low/slow end relative to linear) between them. The `[expression]`
     print now reports `strike_time_ms` directly on every commit, since
     there's no equivalent captured timing data yet for these two new
     bounds -- unlike the depth-delta numbers above, they're first
     attempts at a feel, and this print exists specifically so the next
     real-hardware session can calibrate them from real numbers.
  6. **"Full fast presses are not even registering... if I press really
     fast and hard nothing happens"** -- round 5's `touch_start_depth`
     fix had a real bug of its own: it captured the zero reference from
     the *first fresh Hall sample after touch begins*, reasoning that
     hall.c's background round-robin could leave a stale pre-touch
     reading. But a genuinely fast, hard strike can already be well past
     `MIN_STRIKE_DEPTH_DELTA` by the time that first post-touch sample
     actually arrives -- using it as the zero reference made
     `peak_depth_delta` start near 0 with most of the real travel
     already behind it, unable to ever reach threshold again on the way
     back down. The harder and faster the strike, the more likely its
     very first sample was already deep -- so this bug hit hardest
     exactly the strikes it should have served best. Fixed by capturing
     `touch_start_depth` immediately in `begin_awaiting_strike()`, at the
     exact scan tick touch is first detected, from whatever hall.c
     already has cached -- not waiting for anything. hall.c's depth is
     already baseline-relative (drift-compensated for untouched pads by
     its own background tracker -- see `hall.c`'s entry), so a cached
     pre-touch reading is a perfectly valid zero point; the "staleness"
     concern that motivated the original design was solving a problem
     that likely didn't really exist, at the cost of one that very much
     did.
     Separately, **"when I press faster but not deep the reading is
     still strong"**: at `MIN_STRIKE_DEPTH_DELTA` = 150 (~17% of the
     ~900-unit full-press range), a light flick needs very little real
     force to cover that little distance quickly, so speed and force
     weren't well correlated at that shallow a checkpoint. Raised to 300
     (~33%) -- covering twice the distance in the same short time
     requires genuinely more force, given the pad's spring/magnet return
     works against the motion the whole way, so a confident, fast strike
     is now needed to trigger the checkpoint quickly, not just a flick.
     Still leaves ~67% of travel for aftertouch. Unmeasured against this
     specific complaint (the original 150 was validated against
     "touch vs. press," not "how much depth suppresses fast-but-light
     strikes") -- revisit with a labeled capture if light-fast still
     reads too hard, or deliberate soft presses stop registering.
     Finally, **touch-only haptic feedback added**, raised three times
     with increasingly specific wording before it was clear this meant a
     genuinely new capability, not a report that the note-strike kick
     was broken: "haptic pulse on touch without pressure is gone still
     as well." `tiles_haptics_trigger_touch_pulse()` (see `haptics.h`'s
     entry) now fires the instant capacitive touch is detected
     (IDLE -> AWAITING_STRIKE), completely independent of whether that
     touch ever clears `MIN_STRIKE_DEPTH_DELTA` -- a light "I felt you"
     tick distinct from the note-strike kick, which still requires a
     real press exactly as before.
  7. **"Sudden full force press is not triggering the notes... touch is
     detected... just no midi"** -- round 6's fix (capture the reference
     immediately, not on a later sample) was necessary but not
     sufficient. A targeted debug capture, logging depth on *every*
     cancelled (no-note) release rather than only on a successful
     commit, showed the smoking gun directly: `touch_start_depth` values
     of 880-1040 -- essentially full mechanical compression, at or past
     the ~900-1184 full-press range -- captured at the instant touch was
     first detected, for several hits that produced nothing. For a hard
     enough strike, the entire compression can complete faster than
     capacitive touch detection catches up, so by the time software
     sees "touched," the press has already finished. The bug: this
     module was tracking `peak_depth_delta`, a *delta from a per-touch
     reference* captured at touch-down -- so even when that initial
     reading was already near full compression, its own delta started
     at 0, discarding exactly the information needed to recognize "this
     already happened." There was never a real need for that second,
     per-touch reference on top of `hall.c`'s depth in the first place --
     `hall.c`'s depth is already baseline-relative (drift-compensated
     for untouched pads by its own tracker). Fixed by removing
     `touch_start_depth` entirely: `peak_depth` now tracks the raw
     depth's own running max directly, gated against
     `MIN_STRIKE_DEPTH_DELTA` with no subtraction. `begin_awaiting_strike()`
     checks the very first reading against threshold immediately -- if
     already past it, `threshold_crossed` is set true on the spot with
     `strike_time_ms = 0`, correctly registering as an instantaneous,
     max-velocity strike rather than "not pressed yet." The retrigger
     check (`RETRIGGER_ARM_DEPTH_DELTA`) updated to match: it now
     compares raw depth directly against near-true-rest, which is
     actually a cleaner, more meaningful reference than "wherever touch
     happened to start" was anyway. Every failing case in the capture
     that motivated this fix would now register correctly.
  **Pitch bend from sideways motion, added once strike detection and
  velocity felt solid** ("it all feels fine for now") -- real feedback:
  "can we implement pitch bend on sideways motion for pads? This is only
  relevant after the initial velocity and should compensate for vertical
  movement in magnet and drift from aftertouch. Make sure the math is
  solid before implementing." Only active while a note is already held
  (`PAD_STATE_NOTE_ON`) -- strike detection above never touches X/Y at
  all. The math (full derivation in expression.c's own "Pitch bend from
  sideways motion" section): a naive raw-X-minus-baseline measurement
  would fail the "compensate for vertical movement... and drift" ask
  directly, since a magnetic dipole's field strength changes with Z
  distance -- X's raw magnitude would drift every time the player simply
  pressed harder or eased off, with zero real lateral motion. Fixed by
  working with the field's *direction* instead of magnitude:
  `hall_x_direction_cosine()` computes X / |B| (|B| = sqrt(x²+y²+z²)), a
  direction cosine that depends only on angular position relative to the
  magnet's axis, not distance from it -- the same principle real 3-axis
  Hall-effect joysticks use to derive tilt independent of plunger depth.
  A per-note baseline cosine is seeded the instant a note fires
  (`claim_pitch_bend_owner()`, mirroring how aftertouch seeds
  `smoothed_depth` at note-on rather than from 0); everything sent
  afterward is the smoothed *change* in cosine from that baseline
  (`PITCH_BEND_SMOOTHING_ALPHA`), scaled to the 14-bit MIDI range by
  `s_pitch_bend_max_cosine_deviation` (unmeasured -- no captured real data
  yet for how much a deliberate sideways push actually moves this ratio
  on this board, unlike the depth-based constants above). Runtime, not a
  fixed constant, since `expression_control.h`'s sub-menu (below) needs
  to adjust it live -- `tiles_expression_set_pitch_bend_sensitivity()`.
  **Made usable after a first real-hardware pass** -- real feedback:
  "very jittery and not responding to the sideway tilt as expected...
  even with no tilt it jitters it should be not as sensitive and not
  jittery." Three changes, together: `s_pitch_bend_max_cosine_deviation`'s
  default doubled, 0.15 -> 0.30 (half as sensitive, with
  `expression_control.h`'s sub-menu row-2 anchors rescaled to match, same
  spread ratio as before); `PITCH_BEND_SMOOTHING_ALPHA` lowered, 0.35 ->
  0.15 (more EMA smoothing, unlike `AFTERTOUCH_SMOOTHING_ALPHA` which
  keeps its original 0.35 -- pitch bend needed more aggressive filtering
  specifically, not aftertouch); and a new `PITCH_BEND_DEADZONE_COSINE_
  DELTA` (0.03), applied as a "soft knee" in
  `pitch_bend_14bit_from_cosine_delta()` -- within the deadzone output is
  exactly centered, just past it output ramps continuously from 0 (not a
  hard cutoff-then-jump) and still reaches full swing at exactly
  `s_pitch_bend_max_cosine_deviation`. The deadzone directly targets "even
  with no tilt it jitters": raw Hall X/Y/Z readings are quantized (~16
  raw-count steps, same quantization affecting Z elsewhere in this file)
  and the direction-cosine ratio is sensitive to that even with zero real
  lateral motion -- smoothing alone reduces but doesn't eliminate it,
  since it's a low-pass filter, not a floor. A divide-by-zero/negative-
  range guard in that same function floors the deadzone-adjusted usable
  range to a small positive value, in case the sub-menu is ever tuned to
  a sensitivity at or below the deadzone itself. All three values are
  unmeasured first attempts, not derived from a captured real-noise
  session the way `MIN_STRIKE_DEPTH_DELTA` above was.
  **Two rounds of downstream compensation layers (depth-correlated
  deadzone widening, then a "hold to confirm" timing gate stacked with
  an acceleration ramp on top) were tried and then REMOVED** after real
  feedback on the combined result: "so jittery at rest and at the same
  time it requires too much tilt to register that it might break the
  keys." Both complaints at once, after two rounds of each fix fighting
  the previous round's fix for the other symptom, was a real signal the
  layered-workarounds approach had reached diminishing returns rather
  than something to keep tuning knobs on. Replaced with a simpler
  pipeline and one fix aimed at what was probably the actual root cause,
  not more downstream compensation:
  - **Baseline settle window** (`PITCH_BEND_SETTLE_MS`, 25ms): the real
    likely culprit for "jittery at rest" -- `claim_pitch_bend_owner()`
    used to capture the baseline cosine from ONE raw, instantaneous
    sample at the exact (often percussive) instant a note fires, just as
    susceptible to raw sensor noise as any later reading; if that one
    sample landed off from true rest, every subsequent comparison was
    against an already-wrong reference, which no amount of downstream
    deadzone/timing tuning on the LIVE signal could ever fix. Now
    ownership is claimed without capturing a baseline yet
    (`s_pitch_bend_baseline_settled = false`); the NOTE_ON loop keeps
    running the existing EMA (`PITCH_BEND_SMOOTHING_ALPHA`) and only
    captures baseline from the SETTLED value once `PITCH_BEND_SETTLE_MS`
    has passed, staying centered (no bend sent at all) during that brief
    window.
  - **Sensitivity/deadzone reset to their original values**:
    `s_pitch_bend_max_cosine_deviation` back to 0.15 (from 0.30, then
    0.20), `PITCH_BEND_DEADZONE_COSINE_DELTA` a single fixed 0.02 (the
    depth-scaled extra deadzone is gone entirely) -- direct response to
    "too much tilt... might break the keys," and no longer needing to
    also absorb a bad-baseline problem the settle window now addresses
    at the source. `expression_control.c`'s sub-menu row-2 anchors
    rescaled to match (0.30/0.15/0.075, back to their original spread).
  - **`PITCH_BEND_ARM_MS` kept, but shrunk to a brief 15ms noise-transient
    filter with NO acceleration past 1.0x** (`pitch_bend_confidence_
    multiplier()` now just ramps 0..1 and stops) -- the previous
    multi-hundred-ms "hold to arm, then keep accelerating" version could
    itself make an unsettled baseline worse: a persistent-but-wrong
    offset looks identical to real held intent to a pure time-based
    filter, so accelerating past 1.0x would have accelerated the error
    right along with any genuine tilt.
  - **Direction still flipped** (delta is `baseline_cosine -
    smoothed_cosine`) and **the note-on ordering fix still in place**
    (`claim_pitch_bend_owner()` runs before `tiles_midi_note_on()`, see
    below) -- both unrelated to the jitter/sensitivity rework, carried
    over unchanged.
  - **A real diagnostic print added** (`[expression] pad N pitch bend
    sent: bend=... delta=...`, on every actual send): unlike
    `MIN_STRIKE_DEPTH_DELTA`/`DEPTH_TO_AFTERTOUCH_FULL_SCALE` elsewhere
    in this file, no round of this feature's tuning has ever been
    calibrated from a real captured session -- every constant above is
    still an unmeasured guess. This exists so the next real-hardware
    pass can read actual numbers (how big is rest-state noise really,
    how big does a deliberate tilt actually register) instead of
    continuing to guess blind.
  **A real note-on ordering bug, unrelated to the jitter/sensitivity
  rework above, fixed the same round**: real feedback, "sometimes play
  lands in bent note." `claim_pitch_bend_owner()` used to run AFTER
  `tiles_midi_note_on()` in the commit sequence -- if a DIFFERENT pad
  still owned a non-centered bend the instant a brand-new note fired, the
  synth received `[note-on]` then `[bend-center]`, a real gap in which it
  applied the stale bend to the new note the moment it arrived. Now
  `claim_pitch_bend_owner()` (which sends the center reset when switching
  owners) runs FIRST, so every note-on is guaranteed to reach the synth
  already centered.
  Unmeasured, like every pitch-bend constant's entire history in this
  file -- not yet re-verified on real hardware after this specific
  change.
  **`PITCH_BEND_DEADZONE_COSINE_DELTA` recalibrated from a real capture,
  the first pitch-bend constant in this file's history to be** -- real
  feedback after the reset above: "regular press still is jittery." The
  new `[expression] pitch bend sent` print (see the note-on ordering fix
  above) was captured live over several seconds of an ordinary,
  no-intentional-tilt straight-down press: 1070 sent deltas, median
  0.0257, p90 0.0373, p95 0.0409, p99 0.051, max 0.0945 (likely a
  strike-impact transient, not steady-state). This is real confirmation
  that pressing straight down substantially moves the direction-cosine
  ratio on this board's actual assembly (the "invariant to depth for a
  fixed real lateral tilt" physics this feature is built on doesn't hold
  as cleanly in practice as the math assumes) -- not just quantization
  noise, and the reset-round's 0.02 deadzone sat right at the MEDIAN of
  that distribution, so over half of an ordinary press read as some
  amount of bend. Raised to 0.045 -- past p90, short of the single 0.0945
  outlier -- to actually cover the bulk of a real press. Leaves
  `expression_control.h`'s sub-menu row-2 column 6 (most sensitive,
  0.075) with only 0.03 of real usable range above the new deadzone --
  narrow but still real; may need its own revisit if that column
  specifically still reads too coarse.
  **`PITCH_BEND_SMOOTHING_ALPHA` lowered further (0.15 -> 0.08) for a
  DIFFERENT complaint the deadzone recalibration couldn't fix** -- real
  feedback: "not jittery on press anymore but jittery when pitch bend is
  triggered." The deadzone only zeroes out small deltas near center; it
  does nothing to smooth the SAME ongoing press-depth-correlated wobble
  the capture above measured once a real tilt has pushed past it -- that
  wobble doesn't disappear when bending, it just becomes a smaller
  fraction of a larger signal, and at low-to-moderate bend amounts it's
  still clearly audible as jitter riding on top of the real gesture.
  More aggressive smoothing on the live signal is the right tool for
  that specifically (unlike the deadzone, which is the right tool for
  "is this even real tilt at all"). A real, deliberately held tilt
  (practically always at least a couple hundred ms) still easily outlasts
  this filter's longer settling time; a continuous quick wobble on top of
  it doesn't. Unmeasured -- not yet re-verified on real hardware.
  **Sensitivity and deadzone recalibrated a second time, this time
  cross-referencing TWO real captures against each other** -- real
  feedback after playing on the recalibrated-deadzone build: "not pitch
  bending consistently... requires some extreme bend for it to happen."
  A second debug-console capture, this time of a real DELIBERATE sideways
  tilt (comfortable force, not extreme) held on a struck pad: 634 sent
  deltas, min 0.0323, median 0.0526, p75 0.0573, p90 0.0626, p95 0.0656,
  max 0.0851. Two findings from comparing this directly against the
  at-rest capture instead of tuning each threshold from its own
  percentiles in isolation:
  - `s_pitch_bend_max_cosine_deviation`'s old default (0.15, itself
    already reduced once from 0.30/0.20 in earlier rounds) was still more
    than double the single highest deliberate-tilt sample ever observed
    (0.0851) -- a real deliberate push on this hardware simply never gets
    close to it, directly explaining "requires extreme bend." Reset to
    0.065, so the bulk of a real tilt (median through p90) covers roughly
    half to full swing and the strongest sample clips at the top (same as
    pushing harder than needed on any control).
  - `PITCH_BEND_DEADZONE_COSINE_DELTA` (0.045 from the first
    recalibration) was checked against what fraction of EACH capture it
    actually rejects, not just its own percentile rank in the at-rest
    data: at 0.04, 94% of at-rest samples are rejected while only 0.6% of
    real deliberate-tilt samples are lost; the extra 3 points of
    rest-noise rejection 0.045 bought (97%) cost 5.6% of real tilt signal
    -- a bad trade once both sides were visible together, directly
    explaining "not pitch bending consistently" (some genuine tilt was
    being swallowed by the deadzone). Lowered to 0.04.
  `expression_control.h`'s sub-menu row-2 anchors were rescaled to match,
  but NOT with the usual symmetric 2x/0.5x-of-default spread every other
  row uses -- with the deadzone this close to the new default, that
  spread's column 6 (0.0325) would have landed BELOW the deadzone
  entirely. Column 1 (0.10) and column 6 (0.055) are instead picked
  directly from the captured tilt range itself; see that file's own
  comment for the reasoning.
  **Vertical-pressure compensation added alongside MPE** -- real
  feedback: "the pitchbend seems to lean towards down bend not up bend
  regardless of tilt... it should compensate for vertical pressure to
  get the correct tilt as well." The direction-cosine theory this
  feature is built on (this section's own physics writeup above) assumes
  a PERFECTLY on-axis magnet; comparing the current cosine against a
  FIXED baseline cosine (captured once, at note-on) still lets a real
  assembly misalignment's contribution grow as `|B|` shrinks with a
  harder press, biasing the result toward whichever direction that
  misalignment happens to point -- regardless of actual tilt, which
  matches "leans towards down bend... regardless of tilt" exactly.
  `claim_pitch_bend_owner()`'s replacement, `init_pitch_bend_for_pad()`,
  now captures baseline as raw X (`pitch_bend_baseline_x`, once settled
  -- see `PITCH_BEND_SETTLE_MS` above) rather than a baseline cosine;
  every tick, instead of comparing against that ONE fixed cosine, the
  NOTE_ON loop re-derives what the baseline X would predict the cosine to
  be AT THE CURRENT depth (`direction_cosine_from(pitch_bend_baseline_x,
  magnitude)`, using THIS tick's magnitude). If X hasn't genuinely
  changed (pure depth change, zero real tilt), the current cosine and
  this depth-adjusted prediction are mathematically IDENTICAL by
  construction, so the delta is exactly 0 regardless of press depth; a
  REAL tilt, which genuinely moves X beyond baseline, still produces a
  real nonzero delta. Needs no new calibration data -- it's the same
  physics reasoning this feature already used, applied one step further,
  not a data-fit correction. The `[expression] pitch bend sent` print
  now also reports this tick's smoothed depth alongside the delta, so a
  future capture can check whether the compensation actually decorrelated
  bend from press depth rather than just eyeballing it. Because this
  changes what signal the deadzone/sensitivity constants above are
  actually filtering, those specific numbers are flagged (see the note
  above `PITCH_BEND_CENTER`) as likely needing a fresh capture round
  rather than assumed still-correct.
  Originally a single hardware axis (X) used as "sideways" -- no hardware
  doc exists for which local Hall axis maps to which physical direction on
  a mounted pad, and MIDI pitch bend is inherently one-dimensional
  regardless, so Y was left unused rather than guessing how to blend two
  axes into one bend value.
  **Real regression after the MPE flash above, found and fixed:** "you
  broke mpe preassure, and the pitch bend is so extreme the glide in
  equator is too extreme and biased towards down it never goes up." Two
  separate bugs. Pressure: this was sending Poly Key Pressure (0xA0,
  note-addressed); MPE's actual per-note pressure convention is Channel
  Pressure (0xD0, no note field, since a Member Channel already is one
  note) -- see `midi/midi_out.h`'s `tiles_midi_send_channel_pressure()`.
  Bend: the vertical-pressure compensation above was comparing an
  unsmoothed "predicted baseline" cosine against a SMOOTHED (lagging)
  "current" cosine -- during any depth change (most of a note's hold),
  that lag mismatch alone produced a nonzero, depth-correlated delta even
  for zero real tilt, reintroducing exactly the bias the compensation
  exists to remove. Fixed by computing the raw, already-compensated delta
  fresh every tick (both terms the same tick's magnitude, so a pure depth
  change cancels to 0 before any smoothing happens) and smoothing THAT
  result instead of an intermediate, mismatched term. Also lowered the
  declared MPE pitch bend range (RPN 0) from 48 semitones (4 octaves, the
  MPE spec default) to 12 (1 octave) -- a comfortable tilt reaches full
  wire-value swing, and 4 octaves of glide read as extreme rather than
  expressive.
  **Y joined X, real feedback:** "incorporate the 2 axis tilt onto the
  pitch bend to provide a more strong reading of tilt... more sable
  reeds... make vibratos." A real physical tilt genuinely deflects the
  field in both X and Y to some degree (a dipole's off-axis response isn't
  confined to one hardware axis just because the intended gesture is), so
  X-only was discarding real, correlated signal -- and small/rapid
  wiggles (vibrato specifically) are exactly the amplitude range where a
  single axis's own noise floor matters most. `hall_x_and_magnitude()`
  became `hall_xy_and_magnitude()`; `pad_expr_t` gained
  `pitch_bend_baseline_y`/`pitch_bend_smoothed_y` alongside the existing X
  fields, settled the same way. The NOTE_ON loop now computes a
  same-magnitude-compensated `delta_y` exactly like `delta_x`, combines
  them as `sqrt(delta_x^2 + delta_y^2)` for MAGNITUDE (strictly >= either
  axis alone, so a tilt/wiggle landing partly on Y now adds to the
  reading instead of being lost) while keeping SIGN anchored to `delta_x`
  alone -- deliberately not a true 2D bend direction, which would need a
  real 2D bend axis with no established precedent here; this is the
  minimal change that makes an ordinary X-tilt wiggle read as a stronger,
  more reliable signal without redefining what "positive bend" means or
  disturbing the already-tuned left/right feel the deadzone/sensitivity
  constants were calibrated against.
  **Genuinely per-note now: MPE, not a single-owner workaround.** Real
  feedback: "we need to make sure we have individual per note pitch bend
  not just regular all key pitch bend. like the roli seaboard." Pitch
  Bend Change is a channel-wide MIDI message with no per-note addressing
  in the spec itself -- this used to mean bending one held note also bent
  every other note on this project's single MIDI channel, worked around
  with a single "owner pad" concept (only the most recently struck pad
  drove the shared channel). `midi/midi_out.h` now implements real MPE
  instead of routing around the limitation -- every currently-held note
  gets its own MIDI channel (see that file's own entry for the full zone
  setup), so there's no more sharing to arbitrate. This module's own
  per-pad voice-management sits on top of that wire-protocol support:
  `claim_mpe_channel()` claims a free Member Channel the instant a strike
  commits (or steals the oldest-claimed one, forcibly ending that note
  first, if all 15 are already in use -- `pad_expr_t.midi_channel` tracks
  which channel each pad's currently-held note is on), and
  `end_held_note()` -- the single shared function every normal note-off,
  retrigger, AND the channel-stealing path all funnel through -- always
  centers a channel's pitch bend before freeing its slot, so a reused
  channel can never inherit a stale bend from whatever note used it
  before. Every pitch-bend field that used to be single module-level
  state (`s_pitch_bend_baseline_cosine`, `_smoothed_cosine`, `_last_sent`,
  the settle/run-tracking fields) now lives directly on `pad_expr_t`,
  one full independent copy per pad -- multiple pads can each bend
  independently at the same time, exactly like a real Seaboard. Playing
  a chord and bending only one note now works as expected, not a known
  V1 simplification anymore.
  Toggled via a genuine square ("sentia", SW5) short click -- real
  feedback: "when you press sentia button once it turns on and off the
  pitch bend" (see `expression_control.h`'s entry below for the
  button-side gesture handling -- an earlier pass wired this to circle
  by mistake before real feedback corrected which physical button
  "sentia" actually is: "our shift and power button is circle. sentia is
  square button"). This is still a single global on/off PREFERENCE
  (`s_pitch_bend_enabled`), separate from each note's own
  `pitch_bend_active`, which latches whatever that preference was at the
  exact moment that note fired -- toggling mid-hold doesn't retroactively
  add or remove bend from an already-sounding note.
  `tiles_expression_toggle_pitch_bend()` and `tiles_expression_set_muted()`
  (`expression_control.h`'s expression-mute combo) both now walk every
  pad (`center_and_deactivate_all_bending_pads()`) and center whichever
  ones are actually mid-bend, rather than resetting one single piece of
  shared state -- the direct consequence of bend no longer being
  singular. **Not yet hardware-verified at all** -- neither the physics
  reasoning nor the pitch-bend sensitivity has been tried on a real
  strike yet.
  `s_depth_to_aftertouch_full_scale` is now real, not a placeholder: 900
  by default, derived from a serial-driven full-press capture session
  (`diagnostics/calibration.h`'s 'f' command) with all 24 magnets
  seated -- measured 784-1184 across all 24 pads, average 918; 900 uses
  the average rather than the low end of that spread so most of a
  strike's travel keeps real dynamic range, at the cost of the least-
  sensitive pad or two capping out very slightly before their absolute
  mechanical limit. Real feedback that prompted this session: "find an
  average to pin max [pressure] to, and aftertouch detects any
  additional pressure past that and less as well." Runtime, not a fixed
  constant, for the same reason pitch bend's sensitivity above is --
  `tiles_expression_set_aftertouch_sensitivity()` defaults to exactly
  this same 900 value, so a fresh boot's feel is unchanged. Aftertouch
  also now runs through an exponential moving average
  (`AFTERTOUCH_SMOOTHING_ALPHA` 0.35, a `smoothed_depth` field per pad,
  seeded from real depth at note-on so it doesn't ramp up from 0) before
  `aftertouch_from_depth()` -- deliberately not applied to velocity,
  which is a one-shot transient measurement smoothing would blunt, not a
  continuous signal that benefits from it. Real feedback: "you will
  need... smoothing to have good reads and make it feel good like a
  professional midi piano controller."
  **Explicitly unmeasured, still needs real-hardware tuning:**
  `AFTERTOUCH_SMOOTHING_ALPHA` is a first guess, not measured against
  how jittery a real held reading actually is -- the full-press capture
  session that calibrated the aftertouch default measured static depth,
  not held-reading noise. `MIN_STRIKE_DEPTH_DELTA` is real-data-informed
  (see round 2 above); `STRIKE_TIME_MAX_VELOCITY_MS`,
  `STRIKE_TIME_MIN_VELOCITY_MS`, and `VELOCITY_CURVE_EXPONENT` (round 5)
  are first attempts at a feel with no equivalent captured timing data
  yet -- see round 5's own note on why the `[expression]` print now
  reports `strike_time_ms` specifically to get that data next session.
  Also drives `haptics.c` at the same three points it
  drives `midi_out.c` (note-on -> kick, note-off -> stop, aftertouch
  change -> sustain level) with the exact same velocity/aftertouch
  values, so haptic and MIDI output never disagree -- unless
  `expression_control.h`'s expression mute is active, in which case
  neither pitch bend, aftertouch, nor any haptic effect fires at all,
  while note-on/off/velocity keep working exactly as before (see that
  entry below for the full mute reasoning).
  **New strikes suppressed while the expression sub-menu or transpose
  mode owns the pad grid:** `expression_control.h`'s square-alone hold
  (below) repurposes every pad as a slider tap target, and
  `octave_control.h`'s transpose mode (SW1+SW2 held) repurposes it to
  display the current key -- `tiles_expression_scan()`'s `PAD_STATE_IDLE`
  branch checks `tiles_expression_control_owns_pad_grid()` AND
  `tiles_octave_control_is_transpose_active()`, skipping
  `begin_awaiting_strike()`/the touch pulse while either is true, so a
  slider tap or a glance at the key display never also fires a MIDI note
  underneath -- the latter added after real feedback: "playing the grid
  in transpose menu exits the menu." A pad already past IDLE (mid-strike
  or already held) when either opens is deliberately left alone to
  finish normally rather than being cut off.
- `haptics.h`/`.c` — done for V1: per-pad feedback driven entirely by
  `expression.c`'s calls (not touch/Hall directly). Envelope: KICK
  (opens with a brief overdrive spike at max duty regardless of
  velocity, to overcome the motor's static friction/inertia fast, then
  settles to the velocity-mapped duty for the rest of the window) ->
  GAP (hard zero) -> SUSTAIN. A kick may sit briefly in an internal
  PENDING state first if another kick started too recently -- see
  `KICK_STAGGER_MIN_GAP_MS` below.
  Kick boosted -- real feedback: "the haptic kick is too soft for the
  touch... boost it a lot." `KICK_DURATION_MS` 30ms -> 45ms,
  `KICK_OVERDRIVE_MS` 6ms -> 10ms, `MIN_KICK_DUTY` (the floor even the
  weakest strike gets) 0.35 -> 0.65.
  SUSTAIN, previously built but disabled, is now **re-enabled**
  (`TILES_HAPTICS_SUSTAIN_ENABLED 1`) and reworked: real feedback said
  "map haptics to velocity and key travel, this is a mix... should feel
  stronger with more pressure and ease off when pressure is released
  slowly," not the old aftertouch-only design. `sustain_target_duty()`
  blends `sustain_base_from_velocity()` (this strike's velocity, scaled
  into the same `[0, MAX_SUSTAIN_DUTY]` range as the pressure term --
  deliberately not reusing `kick_duty_from_velocity()`'s boosted range,
  which would otherwise impose an inflated floor regardless of how
  gently a pad is held) with `sustain_duty_from_aftertouch()` (ongoing
  pressure/key travel), weighted `SUSTAIN_VELOCITY_WEIGHT` (0.3)
  toward velocity so pressure stays the dominant real-time driver. The
  *applied* motor duty then chases that blended target via an
  asymmetric slew run every scan tick (not just when aftertouch
  changes, so release keeps progressing even while pressure sits
  still) -- fast attack (`SUSTAIN_ATTACK_PER_MS`, full swing in ~30ms)
  but much slower release (`SUSTAIN_RELEASE_PER_MS`, ~200ms), the
  "ease off... slowly" feel. Two things changed since SUSTAIN was first
  disabled that made re-enabling it worth trying again: the magnets are
  now seated (previously not, so the depth/aftertouch signal it tracked
  was against a meaningless baseline), and `expression.c`'s aftertouch
  is now calibrated from a real capture session and smoothed
  (previously raw/unscaled) -- plausibly the real cause of the original
  "continuous buzzing" complaint, not sustain as a concept.
  **Hardware constraint, not a software choice:** each motor is a
  single low-side NMOS (AO3400A) to a fixed supply rail -- no H-bridge,
  no dedicated haptic driver IC (confirmed against the board map, see
  the file header) -- so real reverse-drive/active braking is
  physically impossible here. The GAP phase (an instant, complete
  cutoff rather than a soft ramp-down) is the closest achievable analog
  to "braking," standard practice for ERM motors without brake
  circuitry -- overdrive (above) is the real, physically-available
  technique for a snappier *attack* instead, not a substitute for
  braking on the stop side.
  Respects `power.h`'s `max_haptic_voices` ceiling -- a new kick past
  the ceiling is dropped (now logged via a `printf`, see below), never
  blocks the MIDI note. Real feedback: "haptics worked at some point...
  but they don't activate always" -- the prime suspect for an
  intermittent, silent dropout is `power.c`'s `TILES_POWER_MODE_FAULT`,
  whose `max_haptic_voices` is a hard 0 (every kick dropped while
  active); that GP22-derived mode has never been exercised on real
  hardware (see `power.h`'s own entry below) and could plausibly be
  flickering into FAULT transiently. `tiles_haptics_trigger_kick()` now
  prints `[haptics] dropped pad N kick -- voice ceiling (mode=...
  max_voices=... active=...)` on every drop, correlatable against
  `main.c`'s existing periodic `[power] mode=...` print, so the next
  real-hardware session can confirm or rule this out directly instead of
  guessing. Also enforces the hardware handoff's "stagger motor starts >= 15ms"
  guidance (`KICK_STAGGER_MIN_GAP_MS`): actual kick starts are spaced
  at least that far apart even if several trigger calls arrive at once,
  chained via a single global "next available slot" time so a burst of
  simultaneous strikes queues cleanly rather than all inrushing
  together. Only affects near-simultaneous multi-pad strikes -- a single
  note's own kick always starts immediately, so normal play has zero
  added latency; only a second (or third) pad struck within the same
  ~15ms window has its *haptic* pulse (never its MIDI note-on) pushed
  back slightly.
  Shares both PCA9685 chips with `buttons.c` via
  `tiles_buttons_pca9685_for_addr()`.
  **Voice stealing added**, replacing the old silent-drop behavior above
  -- real feedback: "additional notes pressed after the limit of haptic
  voices steal the first voices pressed so new notes always have
  priority." Each pad's active haptic voice now carries a monotonic
  `voice_seq` (from a single global counter); when `tiles_haptics_trigger_kick()`
  hits the ceiling, `steal_oldest_voice()` scans all active pads for the
  lowest `voice_seq` (oldest), cuts that pad's motor to 0, and frees its
  slot for the new strike -- FIFO, oldest first. Stealing only ever
  touches the stolen pad's HAPTIC motor; its MIDI note is completely
  unaffected and keeps sounding with no haptic feedback for the rest of
  its hold, matching this file's existing "haptics never blocks MIDI"
  stance. The kick is only actually dropped (with the existing `printf`)
  if literally no pad has an active voice to steal, which shouldn't
  happen given the ceiling check that gates this path.
  **Diagnostic print added to `start_kick_now()`** -- real feedback: "we
  lost the haptic preview a few prompts ago." Reviewed this file's voice-
  ceiling/stealing path and the shared PCA9685 wiring with `buttons.c`
  (see this file's own header on that sharing); three separate debug
  captures from the same session showed zero `[haptics] dropped/
  stealing` lines and a consistently healthy `[power] mode=USB_ONLY
  max_haptic_voices=5`, ruling out both the voice ceiling and
  `power.c`'s previously-suspected FAULT mode. No code-level cause found
  via review. `[haptics] pad N kick started: velocity=... duty=...` now
  prints at the actual motor-drive call, confirming the trigger path is
  at least reached -- whether the problem is downstream of that (wiring,
  the PCA9685 write itself, one specific pad) is still open, pending a
  session that watches for this print while testing.
  **Touch-only haptic pulse added** -- real feedback, raised three
  times with increasingly specific wording until it was clear this
  meant a genuinely new capability, not another report of the missing
  kick: "haptic pulse on touch without pressure is gone still as well."
  A new `HAPTIC_PHASE_TOUCH_PULSE`, distinct from the KICK/GAP/SUSTAIN
  envelope, fires via `tiles_haptics_trigger_touch_pulse()` the instant
  `services/expression.c` detects capacitive touch (IDLE ->
  AWAITING_STRIKE), independent of whether that touch ever becomes a
  real press -- a brief (`TOUCH_PULSE_DURATION_MS`, 15ms), soft
  (`TOUCH_PULSE_DUTY`, 0.35 vs. a kick's 0.65+) tick, not a strike
  confirmation. Deliberately bypasses `max_haptic_voices` entirely
  (`active_voice_count()`/`steal_oldest_voice()` both explicitly exclude
  this phase) -- far shorter/lower-duty than a real kick, so the
  current-budget concern the ceiling exists for doesn't meaningfully
  apply, and every touch getting *some* acknowledgment matters more here
  than voice accounting for a pulse this brief. If the touch goes on to
  clear `expression.c`'s `MIN_STRIKE_DEPTH_DELTA` before the pulse
  finishes, `tiles_haptics_trigger_kick()` takes the pad over exactly as
  it already does for any active pad (its ceiling check only applies
  when `phase == HAPTIC_PHASE_IDLE`) -- the pulse never blocks or delays
  a real kick. Conversely, if the pad is already doing something else
  (a held note's SUSTAIN, a pending KICK) when touch starts, the pulse
  is skipped rather than interrupting real feedback for an
  acknowledgment. Unmeasured -- a first attempt at "clearly felt but
  clearly not a strike," not tuned against real hardware.
  **Diagnostic prints added to `tiles_haptics_trigger_touch_pulse()`**
  -- real feedback: "touching the top won't give any haptic pulse." The
  function had no visibility at all, unlike `start_kick_now()`'s own
  print -- no way to tell "never called," "called but skipped because
  the pad was already busy," and "started but never physically felt"
  apart. Checked the per-pad haptic PCA9685 channel table
  (`board/pad_config.c`) for the more likely code-level cause first:
  every one of the 24 pads' `{pca9685_addr, channel}` pairs is unique,
  with no collisions against each other or against the 6 function
  buttons' LED channels (confirmed against
  `docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md`, which also confirms
  the function buttons have no motors at all -- only 24 motor PWM
  channels exist on the two PCA9685s, one per pad, entirely separate
  from the buttons' LED channels). No channel-mapping bug found, so
  visibility was the missing piece: `[haptics] pad N touch pulse
  started: pca=... channel=... duty=...` on success,
  `[haptics] pad N touch pulse skipped -- already busy (phase=...)` when
  a pad already mid-KICK/SUSTAIN correctly declines the pulse.
  **Found the real bug from that visibility**: a debug capture across
  ~15 plain touches (no press) on 9 different pads showed the trigger
  print firing correctly *every single time* -- the software path was
  never the problem, clarifying the report was about all pads, not just
  a "top" subset. The actual cause was `TOUCH_PULSE_DUTY` itself: 0.35,
  the *exact* duty `MIN_KICK_DUTY` used to be before real feedback
  proved it "too soft for the touch" and forced it up to 0.65+ (see
  `MIN_KICK_DUTY`'s own comment above) -- reusing an already-invalidated
  duty for this new feature was always going to be inaudible on the
  same hardware, for the same reason. Raised to 0.6 (still meaningfully
  below the kick range, so it should read as lighter/shorter than a real
  strike) and `TOUCH_PULSE_DURATION_MS` extended 15 -> 25ms, since unlike
  KICK this phase has no overdrive spike to force a fast start -- a low
  duty and a very short window compound each other's "never gets going"
  problem. Not yet re-verified on real hardware after this change.
  **Global intensity control added, then made column-based** -- real
  feedback: "when you hold and press - or + you can adjust intensity of
  haptics on device." A single global scalar (`s_haptic_intensity`,
  clamped `HAPTIC_INTENSITY_MIN`-`_MAX` 0.0-1.0) applies once, inside
  `set_motor_level()` -- the single low-level write every haptic path
  (KICK, its overdrive spike, SUSTAIN, TOUCH_PULSE) already funnels
  through -- so the one knob scales every effect consistently rather
  than needing a separate multiplier wired into each. `0.0` is now a
  real, legitimate "haptics off" floor, not just a low value -- real
  feedback after a first hardware pass: "the lowest setting is off."
  `tiles_haptics_set_intensity()` is the *only* way this scalar ever
  changes -- there used to also be a step-by-notch
  `tiles_haptics_adjust_intensity()` for `services/expression_control.c`'s
  square-button (SW5, "sentia") shift window (hold square alone, tap
  SW1/SW2), but that let the scalar drift to a value that didn't
  correspond to any of the expression sub-menu's 6 defined columns --
  real feedback: "theres no continuity between menu and arrow keys
  control for haptics... any changes that affect those 4 parameters
  should always be reflected on the menu." Fixed by removing the
  step-by-notch function entirely: "-"/"+" now step the sub-menu's own
  row-1 COLUMN (`expression_control.c`'s `step_haptics_column()`) through
  the exact same `apply_row()`/`tiles_haptics_set_intensity()` path a pad
  tap uses, so the two controls can never disagree about what's actually
  applied -- see `expression_control.h`'s entry below. An earlier pass
  wired the shift gesture to circle by mistake before real feedback
  corrected which button "sentia" actually is. No persistence
  (`services/storage/` is still an empty skeleton) -- resets to full
  (1.0) on every boot. Not yet hardware-verified.
  **Expression mute added** -- real feedback: "a shortcut that disables
  everything and leaves basic midi... it acts like a mute."
  `tiles_haptics_set_muted()`, driven by
  `services/expression_control.c`'s circle+square 3-second combo hold
  (see its own entry below), is a hard kill switch layered *on top of*
  the intensity scalar above, not folded into it -- deliberately
  separate so muting and unmuting always restores exactly whatever
  intensity was already set, and so a mid-flight SUSTAIN's slew state
  doesn't keep silently computing toward a target that will never reach
  the motor. While muted, `tiles_haptics_trigger_kick()`,
  `tiles_haptics_trigger_touch_pulse()`, and
  `tiles_haptics_set_sustain_level()` all become no-ops, and every
  currently-active motor is cut to 0 immediately the instant mute
  engages (looping `tiles_haptics_stop()`, already idempotent for an
  idle pad, across all 24). MIDI note-on/off are completely unaffected --
  only the physical haptic feedback stops. Not yet hardware-verified.
  **Not done:** every duty/timing constant (`KICK_DURATION_MS`,
  `KICK_OVERDRIVE_MS`, `KICK_GAP_MS`, `MIN_KICK_DUTY`,
  `MAX_SUSTAIN_DUTY`, `KICK_STAGGER_MIN_GAP_MS`, and the new
  `SUSTAIN_VELOCITY_WEIGHT`/`SUSTAIN_ATTACK_PER_MS`/
  `SUSTAIN_RELEASE_PER_MS`) is an unmeasured placeholder -- no per-motor
  current/duty data exists yet (see the board map's
  `measured_current_required` TODOs). The boosted kick and the
  re-enabled/reworked SUSTAIN mix above have NOT been tried on real
  hardware yet at all. The pre-boost kick had been tried on real
  hardware (kicks fired and felt like clicks, matching the
  KICK->GAP->silence design that was live then), but not reliably --
  see the intermittent dropout note above, still open.
  **Real bug found and fixed: "pulling power plug killed haptics tho."**
  Diagnosed as a likely rail glitch on the two PCA9685 chips (shared by
  button LEDs and all 24 motors) during an actual power-source switch --
  this file's own `s_pads[]` phase tracking stays completely correct and
  unaware anything happened, while the chips' own internal MODE1/MODE2
  config and per-channel PWM state may have silently reverted to
  power-on defaults without the RP2350 itself resetting. New
  `tiles_haptics_resync_hardware()`: force-rewrites every currently
  non-idle pad's motor to whatever level this file's own state already
  says it should be, regardless of whether that value has "changed" --
  necessary specifically for a long-held SUSTAIN, since
  `tiles_haptics_scan()`'s own "skip once settled" optimization means a
  steady-held chord's motor would otherwise stay silently silent forever
  even after the chips' configuration is restored. Called from a new
  `main.c` power-change callback (`services/power.h`'s
  `tiles_power_register_callback()`, previously registered by no
  consumer despite being built for exactly this), immediately after
  `services/buttons.h`'s `tiles_buttons_resync_pca9685()` re-configures
  the chips themselves -- see that file's own entry for its half.
  **Voice stealing confirmed already correct** -- real feedback asked
  "are we implementing haptic voice stealing after the haptic voice
  limit?" Yes, already built (`steal_oldest_voice()`, unchanged by this
  pass) -- a new kick past `max_haptic_voices` steals the oldest active
  voice rather than being dropped. Separately, real feedback: "we might
  have gotten to close to max draw in usb mode" -- see `power.c`'s own
  entry for the fuller budget accounting that led to lowering
  `max_haptic_voices` 5 -> 3 on USB-only power.
  **Root cause found for "some pads get haptics stuck idk why."** Two
  separate real bugs, both stemming from `trigger_kick()`'s own SUSTAIN
  phase never decaying to true zero on its own (`sustain_target_duty()`
  always has a nonzero floor from the strike's own velocity term) --
  matching a *held musical note*'s real behavior, but wrong for anything
  that isn't one:
  1. Every "momentary UI touch acknowledgment" in `op_mode.c` (the
     mode-picker's touch-click, the scale-picker's touch-click AND its
     periodic "still selected" pulse, the new pattern-picker's
     touch-click, the new pitch-assign commit click) was calling
     `trigger_kick()` -- correct strength, wrong lifecycle. Nothing ever
     called `tiles_haptics_stop()` for these pads (menus suppress
     `expression.c`'s own release-driven stop logic entirely while they
     own the grid), so every pad ever touched while browsing a menu was
     left buzzing at a low floor level indefinitely. Fixed in `op_mode.c`
     by switching all of these to `tiles_haptics_trigger_touch_pulse()`
     instead -- a fixed-strength, self-terminating pulse that needs no
     matching stop call at all, exactly the right primitive for a
     momentary acknowledgment (this file's own header already described
     it as exactly that). `trigger_kick()`'s full KICK->SUSTAIN lifecycle
     stays reserved for what actually needs it: `expression.c`'s real
     note strikes, and `op_mode.c`'s own sequencer playback (correctly
     paired with an explicit stop already).
  2. `op_mode.c`'s sequencer playback itself: entering standby/deep sleep
     (or game mode, the expression sub-menu, or transpose mode) while a
     sequencer note happened to be sounding stopped `tiles_op_mode_scan()`
     from running at all -- the only thing that would eventually call
     `tiles_haptics_stop()` for that pad. Fixed in `op_mode.c` by ending
     the current note the instant control is lost, not waiting for
     playback to naturally reach it again.
  **New `tiles_haptics_set_sleep_silenced()`**, real feedback: "in sleep
  mode haptics should be off." A flag SEPARATE from `tiles_haptics_set_
  muted()` (the user's own deliberate expression-mute combo) -- haptics
  are silenced whenever EITHER is set, but `standby.c` waking from deep
  sleep must never silently clear a mute the user set on purpose before
  falling asleep, which reusing one flag for both would do. Only deep
  sleep uses this; regular standby/screensaver deliberately leaves
  haptics running exactly as normal, matching this file's own
  "lighting-only concept" precedent for standby in general.
- `pedal.h`/`.c` — done: sustain (MIDI CC64) on by default, debounced
  with hysteresis, polarity defaults to the usual normally-open
  footswitch convention and is switchable at runtime
  (`tiles_pedal_set_polarity()`). Expression (CC11, continuous) is
  also implemented -- see this file's own later entry for the mode-
  select design (`tiles_pedal_set_mode()`) this grew into, replacing
  the original plain enable/disable flag. Real auto-sensing of
  polarity/disconnected-pedal state is still a later layer, see
  `docs/architecture/defaults-and-safeguards.md` "Pedal polarity".
  Sends both CCs via `midi_out.h`'s `tiles_midi_send_cc_broadcast()`
  now, not the single-channel `tiles_midi_send_cc()` -- since
  `services/expression.c`'s MPE support means every held note can be on
  a different Member Channel, there's no single "right" channel for a
  sustain/expression message to target anymore; broadcasting to the Zone
  Master Channel and all 15 Member Channels is what actually holds every
  currently-sounding note.
- `power.h`/`.c` — done for V1: derives the actual power mode
  (USB-only / external-only / both / fault) from GP22 (TPS2121 ST) +
  TinyUSB's mounted state, exactly matching the truth table in
  `docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md` "Power/connection
  states". The mux switching itself is fully automatic in hardware --
  this module only observes which state resulted. Debounces the
  combined raw (GP22, mounted) reading (50ms) before committing to a
  new mode, so a brief transient during an actual source switch doesn't
  immediately flip every consumer's limits. Exposes both a live
  accessor (`tiles_power_get_state()` -- a plain struct copy, safe to
  call as often as needed) and an event-driven trigger
  (`tiles_power_register_callback()`, fixed 4-slot table, fired
  synchronously on a debounced mode change) so a future module can pick
  whichever fits: `lighting.c` uses the live read for its brightness
  ceiling (the only consumer so far, proving both the derivation and
  the wiring actually work); haptics/CV-gate will likely want the
  callback instead, to react the instant power changes rather than on
  their next poll. FAULT mode (GP22 high while USB isn't mounted --
  "invalid/transient" per the hardware doc) always reports the safest
  limits (0 haptic voices, CV/gate not permitted, USB-only LED ceiling)
  so a consumer that just respects `tiles_power_state_t`'s fields is
  automatically safe during a fault with no fault-handling code of its
  own. Budgets/ceilings per mode are transcribed from the hardware
  handoff's named-profile table (USB_DEMO_SAFE / FULL_DEMO_EXTERNAL),
  not invented here.
  **Live source-switch transition confirmed on real hardware** -- real
  feedback: "transition worked." Plugging in external power mid-session
  hot-switches the mode within the 50ms debounce window with no reboot/
  power-cycle needed, and every live consumer (`lighting.c`'s brightness
  ceiling first and foremost, since that's the visibly obvious one) picks
  up the new limits immediately -- confirms both the GP22/mounted-state
  derivation and the live-read wiring actually work end to end, not just
  reasoned through against the truth table.
  **Real hot-swap bug found and fixed** -- real feedback: "pulling power
  plug killed haptics tho." See `haptics.c`'s and `buttons.c`'s own
  entries for the fix itself (a power-change-triggered PCA9685
  reconfigure + hardware resync); this file's own role was just already
  exposing the trigger (`tiles_power_register_callback()`) that nothing
  had actually used yet.
  **`max_haptic_voices` lowered 5 -> 3 on USB-only**, real feedback: "we
  might have gotten to close to max draw in usb mode." A fuller current
  budget (see `lighting.c`'s own "Pad brightness ceiling" section for
  the LED half of this same accounting pass) puts ~220mA of estimated
  MCU/sensor/IC/button-LED overhead against the 500mA USB-only total,
  and motor current itself is genuinely unmeasured -- a rough typical-
  small-ERM estimate (~80-100mA running each) means 5 simultaneous
  voices alone could be 400-500mA, potentially the entire budget before
  anything else is counted. 3 voices at that same estimate leaves real
  margin instead of assuming the rest away. Still a conservative
  estimate, not a precise number -- the hardware handoff's own "Five
  voices is an allocation ceiling; a current governor must still reduce
  duty or concurrent starts" already anticipated needing exactly this
  kind of further tightening. External power's 12-voice ceiling is
  unchanged -- the same accounting leaves ~1.8A of margin there even
  under the same pessimistic haptics assumption.
  **Then raised back 3 -> 4**, real feedback: "do 4 voices not 3." Same
  estimate re-run at 4 voices (~320-400mA) instead of 3: still real
  margin against the 500mA USB-only budget once the ~220mA overhead above
  is counted, just not quite as conservative as 3 was -- a middle point
  between the original uncapped 5 and the first, more cautious cut.
  Motor current measurement is still the actual highest-priority unknown
  here, not this specific number.
  **Not done:** `USB_DEMO_VALIDATED_1P5A` (a manual-only override, never
  auto-selected -- belongs to a future `profiles/` module, not this
  one), and any real current measurement (every budget here is a
  governance ceiling, not a live current reading).
- `standby.h`/`.c` — done for a demo V1: after 60s (1 minute -- an
  explicit demo-mode default, expected to change once this isn't just a
  demo) with no touch/button/pedal activity, the pad grid + 6 function
  buttons + underglow stop reflecting real input and instead run one of
  13 rotating ambient animations, switching to a random one every 2
  minutes (also a starting guess, not tuned against how it actually
  feels to watch) -- excludes both the animation ending and the one
  before it, so a switch never immediately repeats itself and never
  bounces straight back to the animation two ago either. The pick is
  also weighted (`s_animation_weight[]`, `ANIM_WEIGHT_REGULAR` 2 vs
  `ANIM_WEIGHT_GAME` 1): the four videogame ones (4 snake, 8 brick
  breaker, 11 Tetris, 12 Pong) are each half as likely as any of the
  nine regular ones on a given switch -- real feedback that the
  videogame screensavers should come up less often than the regular
  ones. `pick_random_animation()` sums the weight of every
  non-excluded animation and rolls into that range in one pass, rather
  than the previous uniform version's "re-roll until it isn't
  excluded" (which would need reweighting on every retry to stay
  correctly weighted once picks aren't uniform):
  1. diagonal traveling wave
  2. a sharp, squared-contrast ring pulsing outward from center
  3. comet-tailed "shooting stars" with per-star randomized speed/tail/twinkle -- fall speed roughly halved (`STAR_SPEED_ROWS_PER_MS_MIN`/`_MAX`) from real feedback that it read as too fast
  4. an actual game of snake: starts 2 segments long (real feedback: 3 felt cramped on a board this small), a pulsing red food dot appears, the snake (green, brighter head) moves toward it a cell at a time and eats it (grows by one segment, a new dot appears) -- direction each step is greedy-toward-the-food with a randomized perturbation (`SNAKE_RANDOM_TURN_WEIGHT`) so the path varies run to run, and it resets to a short length at a randomized start position/direction whenever it grows past `SNAKE_MAX_LENGTH` or traps itself with nowhere to go, so it doesn't settle into one repeating pattern long-term either. Reworked from an earlier version that was just a fixed-length segment ping-ponging a deterministic path -- real feedback was that it didn't feel like an actual game.
  5. a blue/purple RGB showcase where underglow shows the same moving color as the pads -- the whole point being to show off both
  6. a graphic equalizer: each column is a fake EQ/VU bar, bottom-up blue/blue/yellow/red -- red is only ever row 1's own bar color, not a separate marker. Reworked four times: an early pass was too fast/continuously lit, the fix for that (a long, low-biased "phrase" envelope forcing multi-second silences) overshot into "too slow" with "almost no peaks"; a second pass added a percussive, tempo-locked hit envelope at 127bpm plus a separate red "peak-hold" marker that could land on any row and slowly fall back down -- but real feedback was that this was now "too flashy and fast," the falling marker read as "dropping red lights," and at only 4 rows of resolution it just looked broken rather than like a VU meter; a third pass removed the peak-hold marker, added a short attack ramp (`EQ_ATTACK_FRACTION`, closer to a real VU needle's ballistics), and slowed the tempo to ~107bpm. Fourth (current) pass, more real feedback ("still too fast," "moving too crazy," and row 1/red almost never actually lighting): tempo slowed again to 90bpm (`EQ_BEAT_MS`), every column's hit-rate subdivision lowered again (busiest column now 2/beat, not 3 -- `s_eq_col_hits_per_beat`), and velocity now guarantees a real occasional redline -- the top `EQ_REDLINE_FRACTION` (6%) of hits (by the same deterministic golden-angle key already used for per-hit velocity variance) jump straight to full velocity instead of the old smooth 0.55-1.0 curve, which needed to land almost exactly at 1.0 to ever clear row 1's threshold and essentially never did. A deterministic golden-angle-stepped "miss" still occasionally drops a hit to 0 for breathing room. Function buttons are fully off; underglow is a constant blue accent unrelated to any one column.
  7. a circular underglow wave: only underglow moves, a wave traveling around the 4 pixels in their actual physical circular order (`g_tiles_underglow_circular_position` in `board/board_layout.h` -- chain order 0,1,2,3 zigzags diagonally, the real ring order is 0,1,3,2), each pixel rising and dimming significantly as the wave passes through, going around and around; pads/buttons sit at a flat, minimal, non-animated brightness.
  8. brick breaker: the function-button row is a wall of bricks, a 3-pad-wide cyan paddle (bottom pad row) tracks a warm-white/yellow ball with simple AI (moves at most one column per step toward the ball), the ball bounces around knocking orange bricks out until every brick is broken (won) or it gets past the paddle (lost) -- either way underglow flashes red and purple for a few seconds, then a fresh round starts. Ball checked before paddle in the render order so it draws on top during a bounce, when they briefly occupy the same cell.
  9. a scrolling marquee: "TILES - " scrolls across the pad grid using the shared font in `services/pixel_font.h`/`.c` (see its own entry above), with automatic inter-glyph spacing and seamless wraparound (`marquee_total_width()`) rather than a fixed animation; underglow and function buttons both stay off, keeping it purely a pad-grid text effect. Reworked twice from real feedback. First: scroll speed slowed (`MARQUEE_MS_PER_COLUMN` 260ms -> 420ms per column), and the font itself moved out to the shared module to fix a real mistake in the old one-off glyphs (E and F were nearly indistinguishable). Second (current): still "not readable" -- the message dropped "SENTIA - " entirely (now just "TILES - ", real feedback: "we can get rid of sentia"), `MARQUEE_GLYPH_GAP` doubled (1 -> 2 blank columns between letters, so adjacent full-width glyphs don't blur into one bar across only a single dark column), and the scroll slowed further still (420ms -> 600ms per column). The now-unused `N` glyph was removed from `pixel_font.h`/`.c` along with it. The individual letterforms themselves (T, I, L, E, S) were re-checked bit by bit and are each unambiguous on their own -- the readability problem was pacing/separation, not the glyphs.
  10. bouncing glow: the "simple but elegant" one -- a single soft white point bounces diagonally around the pad grid like a screensaver ball, purely a closed-form position (a triangle wave per axis -- a bounce-off-the-walls reflection with no velocity/state to track) with a soft falloff around it, no particle array or game state at all. Row and col bounce at different, non-integer-ratio periods (`BOUNCE_ROW_PERIOD_MS`/`BOUNCE_COL_PERIOD_MS`) so the path slowly traces a Lissajous-like figure instead of repeating quickly. Function buttons stay off; underglow mirrors the pad field like animations 1-4, so the glow naturally spills into it near an anchor.
  11. Tetris: the AI-played autonomous counterpart to `game_mode.h`'s real Tetris (below) -- same custom 5-piece small set/colors (dot, domino, 3-cell straight tromino, 3-cell corner tromino, 2x2 square -- NOT the standard 7 tetrominoes, which real feedback said were too big for this board), deliberately separate state and code from the interactive version, matching this file's existing snake/brick-breaker precedent. Pieces carry a variable `num_cells` (1-4) rather than always 4. A lightweight greedy AI (`tetris_ai_place()`) picks each piece's rotation and column at spawn by simulating every fitting placement and keeping whichever lands the piece's topmost cell deepest (a cheap "keep the stack low" proxy, no real hole-counting), then the piece visibly falls one row at a time toward that spot. A line clear triggers a brief, fast-toggling dramatic white underglow strobe (`TETRIS_LINE_CLEAR_FLASH_MS`/`_TOGGLE_MS`); topping out instead blinks plain red (not the red/purple alternation brick breaker's flash uses), then the well clears and a new game starts. Function buttons stay off.
  12. Pong: the AI-vs-AI autonomous counterpart to `game_mode.h`'s real, two-player Pong (below) -- same court/paddle/ball layout and colors, deliberately separate state and code. Each paddle uses the "move at most one row per step toward the ball" simple AI animation 8's paddle already established (`pong_ai_track()`), but only for the side the ball is currently heading toward -- the other side drifts back to its rest position instead (`pong_ai_recenter()`). Reworked from an earlier version where both paddles tracked the ball every step regardless of direction, which made them move in lockstep/mirror each other constantly -- real feedback: "doing the same on both sides," didn't feel like a real game. Rallies still essentially never end on their own; on the rare miss, a brief white underglow flash plays and the ball re-serves immediately -- the same "stay in this animation and continue" behavior the interactive version uses instead of a win/lose round-end. Function buttons stay off.
  13. falling dots: white dots fall one row at a time from the top, landing wherever they hit the bottom or an already-landed dot below and staying there -- a slow "filling up" screensaver. Unlike every other animation here, this one has real state that accumulates over its whole run instead of looping continuously: up to `FALLINGDOTS_MAX_CONCURRENT` (4) dots fall at once, new ones spawn periodically (`FALLINGDOTS_SPAWN_INTERVAL_MS`) into columns that still have room, and once the entire grid is full it holds for `FALLINGDOTS_FULL_PAUSE_MS` then clears and starts over. Dots always land on top of whatever's already stacked in their column (like Tetris pieces, no gaps), so checking whether row 1 is empty is a valid, cheap proxy for both "does this column have room" and "is the whole grid full." The actively-falling dot is full brightness, landed ones dimmer, for a bit of depth. Function buttons stay off; underglow mirrors the pad field like animations 1-5/10. **Slowed and smoothed** -- real feedback: "slow down falling dots animation and make it smoother." `FALLINGDOTS_STEP_MS`/`_SPAWN_INTERVAL_MS` both raised (~1.8x) for a calmer pace, and every falling dot now cross-fades (smoothstep-eased) between its current row and the next as it falls, instead of the old hard instant jump every step -- all active dots share one global step clock (`s_fallingdots_last_step_ms`), so `anim_fallingdots()` derives one 0-1 progress value from it and uses that to fade the current row out while fading the next row in. A dot about to lock (next row blocked) skips the fade-in target and just holds steady until it locks, since there's nowhere to fade into.

  Touch/button/pedal activity exits standby immediately. A Hall-depth wake fallback exists in the code
  (`hall_depth_wake_triggered()`, checked only while already in standby,
  never as part of deciding whether to *enter* it -- an early version
  that folded it into the entry check broke standby from ever
  triggering at all) but is currently **disabled**
  (`TILES_STANDBY_HALL_WAKE_ENABLED 0`): the magnets aren't in their
  final position yet (mid-plate assembly still being fabricated as of
  this writing), so hall.c's rest baseline and every depth reading right
  now are against a physically incomplete setup -- any threshold picked
  against that data is meaningless, not just untuned, which is why
  standby kept bouncing right back out of every entry even after being
  moved to wake-only. Re-enable once the magnets are seated and real
  rest-vs-pressed numbers exist to pick
  `TILES_STANDBY_HALL_WAKE_DEPTH` from. Until then standby only wakes
  via touch/button/pedal -- MPR121 touch alone not reliably waking it
  while the pad animation runs (buttons/pedal wake it fine) is a
  separate, still-open issue, most likely candidate being the pad-LED
  SK6805 chain's continuous ~25fps rewrite interfering with capacitive
  sensing, not confirmed. `print_wake_source()` now prints exactly which
  input (`touch pad N` / `button N` / `pedal`) caused each wake, paired
  with `touch.c`'s new per-pad `touched`/`released` prints -- meant to
  finally confirm or rule out the SK6805-interference theory: if MPR121
  never reports a touch at all while an animation is running, that's a
  hardware/EMI question outside this file's reach; if it does report one
  but standby still doesn't wake, that's a real logic bug still to find
  here.
  Deliberately a lighting-only concept: touch/Hall/
  expression/MIDI keep running completely unaware standby exists, so
  playing still works exactly as normal even while idle animations are
  showing -- only the idle *lighting* behavior changes. Buttons and pads
  are treated as one 5-row x 6-col grid (row 0 = the 6 function buttons,
  physically just above pad row 1); each animation is a single function
  of (row, col, time) sampled across every cell plus the 4 underglow
  anchor points, so a wave/ripple/etc. that reaches a given pad reaches
  the underglow pixel anchored near it at the same time. Needed two new
  hooks per side: `lighting.c` gained a standby-active guard (so
  touch.c's own continuous writes go inert without touch.c needing to
  know standby exists) plus per-underglow-pixel control (previously
  underglow was a single fixed color written once at init); `buttons.c`
  gained the same guard plus its first real use of
  `tiles_pca9685_set_pwm()` for smooth (not just on/off) button-LED
  brightness.
  Every animation now returns full RGB, not a single brightness scalar
  (`tiles_standby_color_t{r,g,b}` in `standby.c`) -- needed for the RGB
  showcase animation, and `lighting.c` gained
  `tiles_lighting_set_standby_pad_rgb()`/`_underglow_rgb()` to match
  (replacing the old brightness-only `_pad()`/`_underglow()` setters).
  This also fixed a real bug found from user feedback on real hardware:
  the old standby pad setter routed through `pad_level_for_press()`,
  which adds the idle-baseline floor meant for normal touch operation
  (pads "never fully dark in V1") -- so a standby animation asking for
  0.0 brightness still rendered at ~10% of ceiling, never true black.
  The new RGB path scales each channel directly by the ceiling with no
  floor, which is why the wave/ripple animations can now actually go
  fully dark between peaks. Also fixed: function-button LEDs read
  markedly brighter than pad LEDs at the same commanded duty on real
  hardware (different LED/drive path), overpowering every animation's
  top row -- `render_frame()` now collapses each cell's color to a
  single brightness for the (monochrome) button row and scales it down
  by `BUTTON_STANDBY_BRIGHTNESS_SCALE` (0.35, unmeasured) before writing
  it. The RGB showcase animation additionally holds its button row to a
  fixed, low, non-pulsing glow rather than tracking the wave (buttons
  can't show the actual color, so following the wave's brightness swings
  would just look like an unrelated flicker; a quiet constant glow reads
  as "present but not the point" instead).
  Underglow normally samples the same field the pad grid uses at its 4
  anchor points (animations 1-5), but animations 6 and 7 need genuinely
  different underglow behavior than any single pad shows -- gained
  `s_animation_underglow_override[]`, a parallel table of optional
  `underglow_fn_t(pixel_index, now_ms)` functions (NULL = old
  pad-sampling behavior) indexed by pixel rather than (row, col), since
  "a wave traveling around the loop" and "a constant accent color" are
  both about the 4 pixels themselves, not any particular pad position.
  The pad/button/underglow grid-mapping helpers (`board_pad_for_row_col()`,
  `board_button_for_col()`, `g_tiles_underglow_anchor[]`,
  `g_tiles_underglow_circular_position[]`) moved out to
  `board/board_layout.h`, shared with `boot_sequence.c` below rather than
  duplicated.
  After `TILES_STANDBY_DEEP_SLEEP_TIMEOUT_MS` (15 minutes of *total*
  inactivity, not 15 minutes of animation specifically -- same
  `s_last_activity_ms` clock that gates entering standby in the first
  place, just a longer threshold checked while already in standby)
  standby's animations stop and the board drops to a third state, deep
  sleep: everything dark except the circle button (SW6, the rightmost)
  pulsing slowly, the one indicator that it's in this state rather than
  fully off. Same wake conditions as standby.
  **Circle button (SW6) long-press gestures added**, running
  unconditionally every scan regardless of current state
  (`handle_circle_hold()`) -- real feedback: "build into the circle
  button some other features, holding for 10 sec send into power off
  standby meaning no animations just sleep, but holding for 6 seconds
  send into screensaver animations... also cycle through animations...
  when circle is pressed for 6 seconds entering screensaver mode
  serve as scroll through animations without waking the device."
  - Holding 6s (`TILES_CIRCLE_SCREENSAVER_HOLD_MS`) manually forces
    standby's screensaver to start right away (skipping the normal
    1-minute idle wait) and marks it `s_manual_screensaver`. While that
    flag is set, SW1/SW2 step `s_animation_index` sequentially
    forward/backward (`handle_manual_scroll_input()`) instead of picking
    randomly, and -- critically -- doing so does **not** count as
    activity that wakes the device: `real_input_active()` now
    conditionally excludes SW1/SW2 from the wake check while in this
    mode. It also always excludes the circle button itself from that
    check unconditionally, otherwise holding circle to reach either
    threshold would immediately wake the device the instant it's
    pressed, defeating both gestures before they could ever fire.
    Manual screensaver also gets a longer runway before dropping to deep
    sleep -- `TILES_STANDBY_MANUAL_DEEP_SLEEP_TIMEOUT_MS` (20 minutes)
    instead of the normal 15 (`current_deep_sleep_timeout_ms()` picks
    between the two) -- since a user who deliberately entered this mode
    to browse animations is more likely still watching than idle.
  - Holding 10s (`TILES_CIRCLE_DEEP_SLEEP_HOLD_MS`) escalates straight
    into deep sleep -- **the exact same state** the normal inactivity
    timeout above reaches, not a separate one. Real feedback caught a
    real design mistake here: an earlier version had this hold jump to a
    *second*, fully-blank SLEEP state instead ("the sleep mode after 10
    secs is the same as the timeout of the animations, not two separate
    things... both behave as sleep with a single circle light indicator
    pulsing slowly... rename that to deep sleep"). `enter_sleep()` and
    `TILES_STANDBY_STATE_SLEEP` are gone; the 10s hold now just calls the
    same `enter_deep_sleep()` the timeout path uses.
  - Both thresholds are edge-latched per continuous hold
    (`s_circle_screensaver_fired`/`s_circle_deep_sleep_fired`, reset on
    release) so one long hold can't re-fire either gesture repeatedly,
    and reaching 10s doesn't also re-trigger the 6s screensaver
    transition on the way past it.
  - New accessors `tiles_standby_is_deep_sleep()` and
    `tiles_standby_owns_octave_buttons()` (true only while
    `s_manual_screensaver` is set): the latter is checked by
    `octave_control.c` so its own SW1/SW2 handling goes inert during
    manual scroll, the same deferral pattern it already uses for
    `tiles_game_mode_is_active()` -- without it, every scroll press would
    *also* silently step the octave/transpose key underneath.
  **Circle keeps only its original 6s/10s role -- a wrong-button
  detour, corrected.** Real feedback: "remember and set up circle as our
  general shift button unless pressed for the intervals we said," then,
  once there was something to actually assign it to: "can we implement
  pitch bend... when you press sentia button once it turns on and off
  the pitch bend. When you hold and press - or + you can adjust
  intensity of haptics on device." That was read as "sentia button" =
  circle -- the only button with an established "future shift button"
  role at the time of the ask -- and a short-click pitch-bend toggle plus
  a held-circle SW1/SW2 intensity shift were built directly into this
  file (`handle_circle_shift_input()`, `render_circle_led()`,
  `CIRCLE_LED_HELD_LEVEL`/`_TOGGLE_ON_LEVEL`, a
  `tiles_standby_circle_shift_active()` accessor). Real feedback then
  corrected the button identity directly: "our shift and power button is
  circle. sentia is square button. sentia acts as a secondary shift for
  a single feature for now, everything else shift is power/sleep/round."
  All of the above was reverted out of this file -- circle is back to
  *only* the 6s screensaver / 10s deep-sleep gestures described above,
  with no LED override claim and no pitch-bend/intensity involvement at
  all. The corrected feature (pitch-bend toggle, haptic-intensity shift,
  and a considerably expanded circle+square sub-menu + mute design) now
  lives entirely in `services/expression_control.h`/`.c` -- see that
  entry below.
  **Not done / not hardware-verified:** the button-column and
  underglow-anchor mappings in `board/board_layout.h` are based on the
  user's verbal description of the physical board, not a hardware doc
  (checked: not documented in `docs/hardware/`) -- easy to correct there
  if the real LED1-4 order or button alignment turns out different once
  seen lit. The animation frame rate (~25fps) and every animation's own
  timing constants (including the deep sleep pulse period and the
  15-minute timeout itself) are unmeasured against real I2C bus load /
  how it actually looks. Animations 1-3, 5-7 and the deep sleep state
  have been seen on real hardware in some earlier form (several already
  reworked from that feedback, including animation 3's fall-speed halving
  above); animation 4's real-snake rework and animations 8 (brick
  breaker), 9 (marquee, including its font move to `pixel_font.h`/`.c`),
  10 (bouncing glow), 11 (Tetris, including its line-clear/loss flash
  colors), 12 (Pong), and 13 (falling dots) have NOT been seen at all
  yet -- their AI/pathing/step timing, the shared pixel font (hand-designed, not
  measured against how legible it actually is at 4 pixels tall), brick
  breaker's/Tetris's/Pong's paddle-or-placement AI reaction, and bouncing
  glow's periods/radius are all first attempts. The circle-button 6s/10s
  hold gestures, the deep sleep consolidation, and manual animation-scroll
  mode above are also all untested on real hardware -- both hold
  thresholds and the 20-minute manual deep sleep timeout are unmeasured
  starting guesses, same as every other timing constant in this file.
  **Two real wake-from-sleep bugs found and fixed after hardware
  testing.** Real feedback: "circle and square buttons are not waking the
  instrument up from sleep." Circle: `real_input_active()` deliberately
  excludes circle from its generic wake check (needed so a hold building
  toward the 6s/10s thresholds above doesn't wake standby on its very
  first tick), but that exclusion also silently swallowed the ordinary
  case of a short tap doing nothing at all while asleep. Fixed in
  `handle_circle_hold()`: a hold released before either threshold fires
  now wakes the board on release, same as any other button. Square: the
  actual bug lived in `expression_control.c` -- its sub-menu opens the
  instant square is held alone, which claims the pad grid via
  `tiles_expression_control_owns_pad_grid()`; `main.c` gates
  `tiles_standby_scan()` (the only place that actually wakes the board)
  on that same check, so a square press while asleep opened the sub-menu
  instead of ever reaching standby's wake logic. Fixed by having
  `tiles_expression_control_scan()` skip all of its own square/circle
  handling while `tiles_standby_is_active()`/`tiles_standby_is_deep_sleep()`
  is true, so standby's own `real_input_active()` (which already treats
  square as a normal wake input) sees and acts on the press instead.
  **Screensaver timeouts changed**, real feedback: "make screensaver 30
  min if triggered manually, 20 if auto" --
  `TILES_STANDBY_DEEP_SLEEP_TIMEOUT_MS` 15 -> 20 minutes,
  `TILES_STANDBY_MANUAL_DEEP_SLEEP_TIMEOUT_MS` 20 -> 30 minutes.
  **Circle hold thresholds also lowered**, real feedback: "hold sleep
  4sec not 6 and hold 8 for deep sleep" --
  `TILES_CIRCLE_SCREENSAVER_HOLD_MS` 6000 -> 4000,
  `TILES_CIRCLE_DEEP_SLEEP_HOLD_MS` 10000 -> 8000.
  **Animation 6 (graphic equalizer/VU meter) reworked again**, real
  feedback: "make the blue white in vu meter" (rows 3-4's bar color and
  the underglow accent, both previously blue, are now white) and "make
  sure some peaks do redline every once in a while" -- the
  guaranteed-1.0-velocity redline mechanism from the animation's own
  fourth pass (see above) was already in place but still essentially
  never visibly fired, for a reason distinct from what that pass fixed:
  the envelope only reaches exactly 1.0 for a single infinitesimal
  instant, and the whole animation renders at only ~40ms intervals, so
  that instant was almost never actually sampled. Fixed with
  `EQ_PEAK_HOLD_FRACTION`, a brief plateau held at the peak instead of an
  instantaneous one, long enough in real ms that a ~40ms-interval render
  reliably catches it.
  **Brick breaker (animation 8) ball-reachability bug found and fixed**,
  real feedback: "brickbraker is having a hard time hitting all function
  button leds, i suspect its because of the alignement" -- correctly
  diagnosed as an alignment issue, though not a rendering one. The ball's
  row and column used to always move by exactly +/-1 in forced lockstep
  every step (a wall bounce flips `dcol`'s sign but a step always still
  changes col by 1 either way), which makes `(row + col) mod 2` an exact
  invariant of the ball's entire trajectory -- it never changes, no
  matter how many bounces happen. Since `bb_new_round()` always starts
  the ball at an even `row + col` sum, the ball could only ever reach the
  brick wall (row 1) on the 3 columns sharing that same parity -- the
  other 3 bricks were mathematically unreachable every single round, not
  just unlucky. Fixed by throttling column movement to every OTHER step
  (row still advances every step, see `bb_step()`'s own comment) -- once
  row and col no longer move in forced lockstep, the parity invariant no
  longer holds, so wall/paddle bounces (now unsynchronized with the
  column's slower cadence) let the ball reach every column over a long
  enough run; also reads as a slightly shallower, more natural bounce
  angle than the old strict 45-degree diagonal.
  `services/game_mode.c`'s player-controlled brick breaker (`gb_step()`)
  had the exact same bug (identical physics, duplicated per this file's
  own precedent for autonomous/interactive pairs) and got the identical
  fix.
  **`tiles_standby_init()`'s RNG seed fixed** -- real feedback: "is simon
  says generating unique patterns every time? it should do that." The
  ONE `srand()` call this whole firmware's `rand()` calls all share
  (every standby animation's own randomness, snake/tetris/brick-
  breaker's piece/food/ball placement, `game_mode.c`'s Simon Says pattern
  generation) used to seed from `now_ms` -- boot-time milliseconds at
  this specific, fairly deterministic point in the boot sequence (right
  after `boot_sequence.c`'s own fixed ~4-second blocking animation) --
  close enough to identical across boots that the "random" stream itself
  could end up nearly repeating run to run. Now seeds from
  `get_rand_32()` (`pico_rand`, newly linked in `CMakeLists.txt`), which
  draws on real hardware entropy (ROSC ring-oscillator jitter or the
  RP2350's own hardware TRNG, RAM contents, a bus performance counter --
  see `pico/rand.h`'s own header for the full source list) -- genuinely
  different every boot rather than a predictable function of boot
  timing. Not yet confirmed on real hardware whether this actually
  produces visibly different sequences across power cycles.
  **Extended past Simon Says to every player-controlled game**, real
  feedback: "check the seed for all games." The global boot-time reseed
  above fixes the shared stream's starting point, but a game started
  hours into a session was still drawing from wherever that one stream
  had wandered to, not a fresh reseed of its own -- fine in principle
  (the stream itself is no longer predictable once the boot fix lands),
  but real feedback asked for every game to check its own seed
  specifically, not just trust the global one transitively.
  `game_mode.c`'s `gs_start()` (snake), `gb_start()` (brick breaker),
  `gt_start()` (tetris), and `gp_start()` (pong) now each call
  `srand((unsigned int)get_rand_32())` right at the top, the identical
  call `gsim_new_game()` (Simon Says) already had from the fix above --
  every game now independently guarantees a fresh hardware-entropy seed
  the instant it starts, not just at boot.
  **Longer idle/deep-sleep timeout while sequencer mode is active**, real
  feedback: "sleep screensaver should be set to 20 minute in sequencer
  mode since its a more stratic thing so 20 minutes and then screensaver
  and 10 later sleep." Sequencer mode can legitimately run unattended for
  a while (a pattern looping on its own with no hands on the board), a
  real difference from plain melodic idle that the existing 1-minute
  timeout wasn't accounting for. Both the AWAKE->STANDBY check and
  `current_deep_sleep_timeout_ms()` now call the new `services/op_mode.h`
  accessor `tiles_op_mode_is_sequencer_active()` and use
  `TILES_STANDBY_SEQUENCER_IDLE_TIMEOUT_MS` (20 min) /
  `TILES_STANDBY_SEQUENCER_DEEP_SLEEP_TIMEOUT_MS` (30 min = 20 + the "10
  later" from real feedback) instead of the normal 1-minute/20-minute
  pair whenever true -- computed fresh every check, not cached, so
  switching modes while idle (impossible mid-STANDBY since op_mode.c
  itself stops running then, but not before) always uses the CURRENT
  mode's own timeout. A manually-forced screensaver (holding circle)
  still takes priority over both if somehow active at the same time --
  the two currently share the same 30-minute deep-sleep number, but
  that's coincidence, not a dependency between them.
  **Deep sleep now silences haptics** -- real feedback: "in sleep mode
  haptics should be off." See `haptics.c`'s own entry for the new
  `tiles_haptics_set_sleep_silenced()` this file calls from
  `enter_deep_sleep()`/`exit_standby()`; regular STANDBY/screensaver is
  deliberately untouched, keeping this file's existing "lighting-only
  concept" precedent for the lighter state.
- `boot_sequence.h`/`.c` — done for V1: a ~4-second, blocking power-on
  animation run once from `main.c`, before the main loop starts (nothing
  else needs to run concurrently -- USB stays alive via TinyUSB's own
  background IRQ task regardless). A white "rain" floods down through
  the whole grid -- function buttons (row 0, monochrome PWM) light first
  as the flood's source, pad rows 1-4 follow (underglow off throughout)
  -- then buttons and pads fade together to complete dark, then a
  single, slow, smoothstep-eased "Sentia Instruments Magenta" (#FF00FF)
  pulse across pads + underglow finishes it. Function buttons are
  explicitly blacked out right before that last phase and never touched
  again for the rest of the sequence -- they're plain monochrome PWM,
  not addressable RGB, so they can't show magenta at all; the rain and
  fade are the only phases they participate in.
  Reworked three times already from real feedback:
  1. Direction and pacing -- it originally rose from the bottom-center
     outward (reversed to flow down instead, "like rain/flooding") and
     used linear, fairly fast, narrow-edged transitions that read as
     "jumpy" (now smoothstep-eased throughout, wider soft edges, longer
     durations).
  2. Buttons, first pass -- originally lit as part of the rain and
     (attempted, but apparently not fully) excluded from the magenta
     pulse; changed to held dark for the whole sequence with no
     exceptions.
  3. Buttons, second pass -- that turned out to be an overcorrection:
     real feedback was that buttons should be part of the rain/fade (as
     the flood's own source row), just not the final magenta glow, which
     they can't show correctly anyway. Reworked back to lighting buttons
     through phases 1-2 and excluding them only from phase 3.
  Reuses the exact same standby-active rendering path `standby.c`'s
  animations use (`tiles_lighting_set_standby_active()`, the RGB
  pad/underglow/button setters) rather than a second mechanism, and
  shares `board/board_layout.h`'s grid model with `standby.c`.
  Also uses the time productively: `hall.c`'s rest baseline is captured
  once at `tiles_hall_init()`, at the very first instant of boot before
  anything has settled -- this sequence re-captures it
  (`tiles_hall_recapture_baseline()`) right as the animation ends, a
  few seconds later, at essentially no extra cost since the animation
  was going to take that long anyway.
  **Not hardware-verified:** none of the rain/fade/pulse timing or edge-
  width constants have been seen on real hardware yet -- this rework is
  itself unverified, only reasoned through against the *previous*
  version's real feedback.
- `midi_clock.h`/`.c` — done for V1: MIDI clock RECEIVE (System
  Real-Time bytes only -- 0xF8 Clock, 0xFA Start, 0xFB Continue, 0xFC
  Stop), the timing source `op_mode.h`'s sequencer mode plays back from.
  Real feedback: "we pull midi clock from midi or usb from software or
  hardware and thats how the clock works" -- deliberately no internal
  free-running fallback tempo; the sequencer doesn't advance at all
  until a real external clock starts sending, exactly like a hardware
  sequencer chained off a DAW/master clock. USB MIDI IN only for now
  (`tud_midi_stream_read()` -- the composite device's descriptor already
  had a full IN+OUT endpoint pair, `midi/usb_descriptors.c`'s
  `TUD_MIDI_DESCRIPTOR` call, so no descriptor change was needed, just
  actually calling the read side for the first time); DIN MIDI IN isn't
  built yet (see `midi/README.md`), but would feed this same byte parser
  once it exists. Deliberately narrow scope: only the 4 real-time bytes
  above are parsed, everything else read from the stream (notes, CC,
  sysex, etc.) is silently discarded -- real-time bytes are always
  complete single-byte messages that can legally appear anywhere in a
  MIDI stream, so no running-status tracking is needed to find them
  safely. `tiles_midi_clock_get_state()` returns a monotonic pulse count
  (incremented by 0xF8 regardless of transport state, matching how a
  real master keeps clocking continuously) plus the current running flag
  and a one-shot start_edge (Start resets position; Continue resumes
  without one) -- `op_mode.c` diffs the pulse count against its own last
  reading rather than this file pushing per-pulse callbacks, robust to
  more than one pulse landing between main-loop iterations. **Not
  hardware-verified** -- untested against any real MIDI clock source
  (DAW, hardware sequencer, or otherwise) yet.
  **Master tap tempo added, a second pulse source feeding the same
  counter.** Real feedback: "lets make the midi clock work in a way where
  we can do master tap tempo on the instrument with shift round button
  when not derecting midi clock from a daw. the tapp tempo is only active
  in sequencer and arp mode and requiere 4 taps to calcualte minimum."
  This file no longer has "deliberately no internal free-running fallback
  tempo" as an absolute -- that reasoning still holds for a pure
  free-running default, but a player-initiated tap-tempo master clock is
  a different feature entirely, and real feedback explicitly asked for
  it. `tiles_midi_clock_register_tap()` takes a timestamp per qualifying
  circle-button press edge (`op_mode.c` owns deciding "qualifying":
  sequencer/arp mode active, no real external clock detected, not part of
  `game_mode.c`'s reserved 4-button combo -- see that file's own entry
  below) and accumulates them; once `TAP_TEMPO_MIN_TAPS` (4) taps land
  within `TAP_TEMPO_SESSION_TIMEOUT_MS` (2000ms) of each other, the
  averaged interval (up to the last 8 taps, oldest dropped) becomes an
  internal virtual-pulse generator running at the same 24-clocks/quarter-
  note resolution real bytes use, feeding the exact same `pulse_count`
  field real 0xF8 bytes do -- `tiles_midi_clock_scan()` only advances it
  while `tiles_midi_clock_external_active()` is false, so a real clock
  reappearing always wins immediately with no explicit hand-off code
  needed. The first time a tempo is established each session, `running`/
  `start_edge` fire exactly like a real 0xFA Start would, so `op_mode.c`'s
  sequencer reset-to-step-0 logic needed no changes at all to also work
  from a tap-tempo start. Every tap after the first re-syncs the
  generator's phase to that exact moment (the beat visibly snaps to your
  tapping, not just the tempo), and a gap longer than the session timeout
  starts a fresh 4-tap accumulation without ever interrupting whatever
  tempo/playback was already running -- pausing to think doesn't stop the
  clock. New `source_is_tap_tempo` field on the state struct is
  diagnostic only; `op_mode.c`'s own consumption of `pulse_count`/
  `running`/`start_edge` is identical either way, by design.
  "and then flash that light as the tempo even when midi sync flash the
  tempo there" -- see `op_mode.c`'s own entry below for the shared
  beat-flash this file makes possible for both sources at once, just by
  both landing in the same counter. **Not hardware-verified** -- the tap
  math, the 4-tap minimum, and the phase-resync-per-tap behavior are all
  first attempts, not felt on real hardware yet.
  **New `tiles_midi_clock_set_running()`**, real feedback: "we need a
  button that starts and stops sequencer" -- `op_mode.c`'s own manual
  transport buttons (see that file's entry below) call this directly to
  stop/resume the transport when it's being driven by the internal
  tap-tempo generator. A no-op while `tiles_midi_clock_external_active()`
  is true, the identical guard `tiles_midi_clock_register_tap()` already
  uses -- the DAW's own Start/Stop bytes are the only valid transport
  control whenever a real clock is present. Setting `true` does NOT set
  `start_edge` (Continue-style resume, not a reset to step zero).
  **Tap tempo now auto-starts even after a manual stop**, real feedback:
  "tap tempo should autostart sequence when 4 taps detercted even if
  stropped." The FIRST pass only auto-started on this BOOT's first-ever
  tempo establishment (`s_tap_tempo_established`, a lifetime flag) --
  re-tapping a fresh 4-tap sequence after a manual stop only updated the
  tempo number, silently leaving the transport stopped. Fixed with a new,
  SESSION-scoped `s_session_hit_minimum` flag (reset alongside
  `s_tap_count` on both the existing session-timeout path and the new
  inconsistency-reset below) -- every fresh session's first time reaching
  the 4-tap minimum now auto-starts, guarded on `!s_running` so re-tapping
  WHILE already playing (refining the tempo live) never yanks the
  transport, matching real Ableton-style tap-tempo behavior.
  **Tap-tempo now resets on inconsistent tapping**, real feedback: "it
  stops capturing tempo if taps very inconsistent similar to ableton lives
  tap tempo." Before appending each new tap, its interval from the
  previous one is checked against the running average of this session's
  OWN intervals so far (`TAP_TEMPO_CONSISTENCY_TOLERANCE`, 30%) -- a tap
  that strays further than that reads as a different tempo attempt, not
  noise to blend in, and resets the session to start fresh from just that
  tap (same non-disruptive shape the existing session-timeout reset
  already uses: doesn't touch whatever tempo/playback was already
  established). Needs `#include <math.h>` for `fabsf()`.
  **New `tiles_midi_clock_is_running()`** -- a pure read of `s_running`
  that, unlike `tiles_midi_clock_get_state()`, does NOT consume
  `start_edge`, so it's safe to call anytime. `op_mode.c`'s own transport
  handler uses it to tell "stop while already stopped" (rewind) apart
  from "stop while playing" (pause), and "play while already playing"
  (restart from the top) apart from "play while stopped" (resume) -- real
  feedback: "play position of head should reset when stop click twice and
  if playing and play again it starts from the top again."
  **Auto-latch to external clock the instant it's detected, not just
  on a real Start byte.** Real feedback, live mid-testing: "there is a
  sync issue between the clock on tiles and ableton. its not auto
  latching to ableton clock. it should auto switch to that clock when
  it detedcts it. midi clock has priority over iinternal clock." A real
  Start (0xFA) byte already gave `start_edge` correctly; a bare Clock
  (0xF8) byte deliberately never did (a pulse alone isn't "this is beat
  1"), which left a real gap matching this report exactly: if TILES
  starts (or resumes) receiving Ableton's clock WITHOUT ever seeing the
  Start that began it -- Ableton was already playing before TILES was
  listening, or before a dropped connection came back -- `external_
  active` flips true off nothing but plain Clock bytes, `s_running`
  never does (Clock alone doesn't set it, unchanged), and no lane's own
  step-boundary phase ever re-anchors to the new source -- genuinely
  indistinguishable from "not auto-latching" from outside this file,
  even though clock bytes are arriving and being counted correctly.
  Fixed by reusing the existing, already-safe Start machinery rather
  than inventing a new one: the instant `tiles_midi_clock_external_
  active()` transitions from false to true (tracked via a new
  `s_external_was_active`), `tiles_midi_clock_scan()` now sets both
  `s_running = true` (a real clock's mere presence outranks whatever
  TILES's own state already assumed -- "midi clock has priority") and
  `s_start_edge = true` (the exact mechanism `seq_reset()` already
  safely handles -- every running lane recaptures ITS OWN step-
  boundary reference against the CURRENT pulse_count; deliberately does
  NOT reset `s_pulse_count`'s own absolute value, which would be unsafe
  -- lanes already mid-flight are tracking elapsed pulses against it,
  and yanking it backward would underflow that unsigned math). A later
  real Start/Stop still behaves exactly as always; this only covers the
  gap where external clock's PRESENCE itself, not a specific byte
  within it, is what should have triggered the switch.
  **Diagnostics added for a follow-up report of the same symptom
  family.** Real feedback: "midi clock in sequencer ius not syinking to
  ableton clock, its not quantizing snapping how it should, its always
  at the right tempo but not quite synked. make sure it operates like
  in other hardware with precise sync." Audited this file's own receive
  path end to end: while a real external clock is active, `pulse_count`
  only ever increments from genuine 0xF8 bytes (the internal tap-tempo
  generator is hard-gated off by `tiles_midi_clock_external_active()`),
  and `op_mode.c`'s own step-boundary reference always re-anchors by an
  exact multiple of `OP_SEQ_CLOCKS_PER_STEP` off that same counter (see
  that file's own `seq_advance_clock()`) -- no interpolation or
  wall-clock extrapolation anywhere in the phase math that could drift
  once a real clock is genuinely present. That leaves the most likely
  real cause outside this file's own code: Ableton's Sync OUTPUT for
  whichever port TILES uses isn't ticked in Preferences -> Link/Tempo/
  MIDI (distinct from that same row's Track/Remote columns, which are
  what the transport-remote CCs already rely on and don't imply Sync is
  also on) -- TILES would then never see a real clock at all and fall
  back to its own free-running tap-tempo generator, which can
  coincidentally land near the right BPM (if a tap session or the
  default happens to be close) while never actually being phase-locked
  to Ableton's transport, matching "always the right tempo but not
  quite synced" precisely. Added `printf` tracing (`[midi_clock]`
  prefix, USB CDC console) for every real Start/Continue/Stop byte and
  for every `external clock ACQUIRED/LOST` transition, specifically so
  this can be confirmed or ruled out directly instead of guessed at: if
  `ACQUIRED` never prints while Ableton is audibly playing, that's the
  DAW-side Sync setting above, not a firmware bug; if it does print,
  the gap is genuinely in this file's own math and needs a second look
  with that confirmed.
- `op_mode.h`/`.c` — done for V1: operation modes (melodic/chord/
  sequencer/arpeggiator), SW4 ("diamond")'s function -- real feedback:
  "its time to implement the operation modes. we have standard melodic,
  chord trigger mode..., sequencer mode..., arpeggiator mode.... we
  trigger those with the rombus diamond button." A single click (not a
  hold -- real feedback explicitly compared this to `game_mode.h`'s own
  menu but as a simpler one-button click, since SW4 alone has no
  normal-play collision the way holding all 4 game-mode buttons would)
  toggles between melodic (no menu) -> the mode-select menu -> back to
  melodic, or exits any active non-melodic mode straight back to
  melodic. The menu itself is 4 rows (one per mode, not `game_mode.h`'s
  single row of 4 touch-targets) -- real feedback: "we have those 4
  modes for now each on its own row because we might add alternative
  modes derived from each to each corresponding column" -- reusing
  `expression_control.c`'s row/column sub-menu shape rather than
  `game_mode.c`'s menu layout. Row ("mode selector") colors, real
  feedback's own phrase: melodic = Sentia magenta, chord = green,
  sequencer = red, arp = blue.
  **Chord and arp are selectable but not yet implemented** -- real
  feedback named chord explicitly ("not implemented yet"); arp's actual
  trigger logic (pattern, rate, note order from currently-held notes)
  wasn't specified in enough detail to build without guessing blind, the
  same measure-before-building discipline this codebase has followed
  throughout. Both stay selectable from the menu and glow the diamond
  LED to confirm something other than melodic is picked, but neither
  claims the pad grid (`tiles_op_mode_owns_pad_grid()` returns false for
  them) -- touching pads keeps playing completely normal melodic notes
  underneath until each mode's real logic exists, a deliberate, honest
  stub rather than a guess dressed up as a feature.
  **Sequencer mode, fully built:** all 24 pads = 24 steps, real
  feedback: "lets build sequencermode with the full 24 keys as a
  standard 24 step." Step order is row-major top-left to bottom-right
  (pad 1 = step 0 ... pad 24 = step 23, `board_pad_for_row_col()`'s own
  numbering, no remapping). Tapping a pad while sequencer mode owns the
  grid toggles whether that step is armed (plays its own
  `note_map.c`-mapped pitch when the playhead reaches it); armed steps
  persist across leaving and re-entering sequencer mode (only cleared at
  boot), so checking something in melodic mode and coming back doesn't
  wipe the pattern. Playback is driven entirely by `midi_clock.h`'s real
  external clock: `OP_SEQ_CLOCKS_PER_STEP` (6) is the standard "1 step =
  1 sixteenth note" convention (24 MIDI-spec clocks/quarter note / 4).
  Start resets the playhead to step 0 and plays it immediately; Continue
  resumes from wherever it already was; Stop silences whatever's
  sounding and freezes the playhead. Notes fire on a single reserved MPE
  Member Channel (the last of the 15 -- v1 only ever sounds one
  sequencer note at a time, so one dedicated channel is enough rather
  than reaching into `expression.c`'s own private per-touch allocator,
  which is built around strike/touch lifecycle events sequencer playback
  doesn't have; a lingering touch-note already on that exact channel at
  the instant sequencer mode starts is a narrow, accepted edge case, not
  a structural conflict, since sequencer mode already suppresses new
  touch strikes while it owns the grid) at a fixed velocity (100 -- no
  strike/touch event exists to derive one from) and a full-length gate
  (note-off exactly when the playhead leaves that step, no separate
  gate-length concept yet). Haptics: "the happtics activate on each
  active step" -- `tiles_haptics_trigger_kick()`/`_stop()` fire in
  lockstep with the MIDI note.
  Step colors, real feedback: "in sequencer steps that are not playing
  are slightly dim red. steps thats on is white bright and steps that
  are unavailable are off." Three states: armed + not the current
  playhead step = dim red; armed + IS the current step while its note is
  actually sounding = bright white; never armed ("unavailable") = fully
  off ALWAYS, including at the instant the playhead passes over it -- no
  playhead flash on an empty step, per the literal "unavailable are off"
  with no stated exception.
  **Mutual exclusion, and a collision found while auditing for more like
  it:** real feedback asked to "look for other colisions" after the
  circle+square/game-mode-combo fix above -- this module's own SW4 click
  needed the identical guard (`game_mode.h`'s entry combo is
  SW3+SW4+SW5+SW6, so SW4 alone during that combo's imperfectly-
  simultaneous press/release could misread as a plain diamond click; see
  `s_diamond_press_had_conflict` in `op_mode.c`). `tiles_op_mode_owns_
  pad_grid()` was wired into every existing mutual-exclusion point this
  codebase already had (`game_mode.c`'s `gm_combo_held()`,
  `expression_control.c`'s scan guard, `octave_control.c`'s combo guard,
  `expression.c`'s new-strike suppression, and `main.c`'s standby-scan
  gate) rather than inventing a parallel mechanism.
  **SW3/triangle: each mode's own sub-menu**, real feedback: "the
  triangle is sub menues per each mode so that button toggles its onw
  menue in each mode. for melodic it toggles different scale modes."
  Same click-to-toggle shape as diamond's own mode-picker
  (`handle_triangle_click()` mirrors `handle_diamond_click()`, including
  its own SW4-conflict guard for the identical game_mode.h 4-button-combo
  reason), but one level down -- only melodic's sub-menu (the scale
  picker) is built; other modes don't have one yet, the same
  selectable-but-not-implemented spirit as chord/arp themselves. The
  scale picker is "Ableton Push style" per real feedback: one pad = one
  scale across the whole 24-pad grid (no row/column structure, unlike the
  mode-picker), pulled from `note_map.h`'s new 18-scale table (see that
  file's own entry above) -- tapping a defined slot calls
  `tiles_note_map_set_scale()` directly and stays open (not a one-shot
  pick-and-close like the mode-picker) so different scales can be tried
  while watching/hearing the difference; closes only via triangle again.
  **Menu visual language standardized**, real feedback: "lets
  standardize the pulsing and brightness and behaviour for menues,
  meaning select color or active color is always white bright pulsing in
  menues, respective less bright but still readable bright for non
  selected and available and of for unaveilabe." Three states everywhere
  this applies: selected/active = white, full brightness, pulsing
  (`menu_selected_pulse_level()`); available-but-unselected = a readable
  mid brightness in whatever color that menu uses (Sentia magenta here);
  unavailable = fully off and not selectable. Retrofitted into
  `expression_control.c`'s existing sub-menu too (`submenu_selected_
  pulse_level()`, the identical shape redefined per file same as
  `SENTIA_MAGENTA_R/G/B` already is) -- its selected column used to be
  static full-brightness magenta (an EARLIER real-feedback request, "make
  the selected level of everything sentia magenta color," now
  superseded) and its unselected level was raised 0.06 -> 0.35 to
  actually read as "readable" under the new language. The mode-picker
  itself deliberately keeps its own per-row mode-identity colors
  (magenta/green/red/blue, "mode selector color" is a separate, earlier
  real-feedback request) rather than being retrofitted to white-selected
  -- it has no persistent "currently selected, browsing" state for that
  language to apply to (tapping a row activates and closes immediately).
  **Also found while fixing "still dim in all modes and games":** real
  feedback after the earlier `TILES_LIGHTING_IDLE_BASELINE_PERCENT`
  raise. That constant only affects plain melodic idle -- every
  `tiles_lighting_set_standby_pad_rgb()` caller (this file's own menus,
  `game_mode.c`, `standby.c`) goes through `pad_channel_level()` instead,
  which scales by `ceiling_level()`: a real, documented power-budget
  safety cap (`docs/architecture/defaults-and-safeguards.md` -- full
  white across 24 pads + underglow is ~448mA against a 500mA USB-only
  budget), 37% on USB-only power specifically. Confirmed as the actual
  root cause of the persistent dimness; left unchanged per explicit
  direction ("leave USB-only ceiling alone, just note it") rather than
  silently loosening a documented safety margin -- external power gives
  75% at the time this was written, later raised to 90% (see
  `lighting.h`'s entry above) once a fuller current-budget accounting
  confirmed the real headroom there.
  **Menu selection now press-confirmed, not touch-confirmed**, real
  feedback: "when you touch but not click in menu make a strong haptic
  click be felt, push pad to at least mroe than 50% to sleect and
  selected pad give a constant haptic pulsing pattern to indicate its the
  active one." Applies to both selection-style menus here (the
  mode-picker and the scale-picker) -- sequencer mode's step-arm taps are
  a different interaction (toggle on/off, not "pick one") and
  deliberately untouched. A fresh capacitive touch alone now only fires a
  firm `tiles_haptics_trigger_kick()` acknowledgment
  (`OP_MENU_TOUCH_CLICK_VELOCITY`); the actual selection only commits
  once `tiles_hall_get_depth()` crosses `OP_MENU_SELECT_DEPTH_THRESHOLD`
  (450, half of the ~900 full-scale reference expression.c's own
  aftertouch already uses) while still touched. The scale picker's
  currently-selected pad now also re-fires a haptic kick every
  `OP_MENU_SELECTED_PULSE_PERIOD_MS`, synced to the same period as its
  visual white pulse -- a real felt "pattern," not
  `tiles_haptics_set_sustain_level()` (which needs an already-active
  voice from a kick to mean anything, not a fit for a menu pad that was
  never "struck"). The mode-picker doesn't get this pulsing step -- 
  picking a mode still closes the menu immediately, so there's no
  persistent "selected, still browsing" state for it to apply to.
  **Scale picker now closes on selection too**, real feedback: "when we
  select a menu item the menu should close not it doesnt stick around
  until disabeled." Reverses this file's own earlier deliberate choice
  to leave it open for trying different scales -- real feedback settled
  that the other way; matches the mode-picker's own close-on-select
  behavior now, one consistent rule for both menus. Re-opening (a fresh
  triangle click) still shows whatever's now selected pulsing, unchanged.
  Also, `OP_SCALE_AVAILABLE_LEVEL` raised 0.35 -> 0.5, real feedback: "we
  could have idle led in scale mode more bright not as dim."
  **Circle (SW6, "shift"): master tap tempo + shared beat flash**, real
  feedback (see `midi_clock.c`'s own entry above for that file's half):
  "lets make the midi clock work in a way where we can do master tap
  tempo on the instrument with shift round button when not derecting midi
  clock from a daw. the tapp tempo is only active in sequencer and arp
  mode and requiere 4 taps to calcualte minimum. and then flash that
  light as the tempo even when midi sync flash the tempo there."
  `handle_circle_tap()` registers a tap
  (`tiles_midi_clock_register_tap()`) on circle's own press edge (not
  release -- accurate tap timing needs the press instant itself) only
  while `s_active_mode` is SEQUENCER or ARP, `tiles_midi_clock_external_
  active()` is false, and triangle/diamond/square aren't ALSO currently
  held -- the same "skip while `game_mode.c`'s reserved 4-button combo is
  forming" guard this file's diamond/triangle click handlers already use,
  adapted to a press-edge check since this gesture (unlike a click) has
  to fire on press, before a release-based conflict flag would exist yet.
  `compute_beat_flash_level()` reads `pulse_count / 24` (one quarter note)
  each scan, and whenever that beat index changes, flashes circle's LED
  at full brightness for `OP_BEAT_FLASH_DURATION_MS` (100ms) -- driven
  purely from the shared clock state `midi_clock.c` exposes, so it flashes
  identically whether the beat is coming from tap tempo or a real external
  clock, exactly as asked. `tiles_midi_clock_get_state()` is now fetched
  exactly ONCE per scan (it clears `start_edge` as a side effect) and
  threaded as a parameter into both this beat-flash computation and
  `seq_advance_clock()` -- a real risk found while wiring this in: reading
  it twice per scan could silently drop a genuine start_edge on the second
  read. Sequencer mode writes the flash directly in `render_sequencer()`
  (which already claims the whole grid); arp mode -- still a stub with no
  grid claim -- drives it through `buttons.c`'s per-button LED override
  instead, claimed only while arp mode is actually active
  (`tiles_buttons_set_override_active(TILES_CIRCLE_BUTTON_ID, ...)` in
  `set_active_mode()`) and released the instant arp mode is left. Scoped
  rather than claimed permanently like diamond's own override
  specifically because circle already has a real, pre-existing default
  (`buttons.c`'s "LED follows press," circle's own visual feedback for
  `standby.c`'s screensaver/deep-sleep hold gesture) that a permanent
  claim would silently break outside sequencer/arp mode -- releasing the
  override immediately re-asserts that default per `buttons.h`'s own
  documented contract.
  **Sequencer redesigned into a full multi-pattern workflow**, real
  feedback: "we need to fix the sequencer. lets design a better
  achitecture... lets study available sequencers and implement into this
  device the best most human and cool workflow possible." Four pieces,
  each mapped to something real hardware step sequencers already do,
  adapted to this board's touch-only, no-encoder hardware:
  - **4 patterns** (`op_seq_pattern_t`, one per pad row -- 4 was picked
    specifically because it matches the grid's 4 rows exactly, letting
    the picker below reuse the mode-picker's own row-per-item shape
    rather than inventing a new layout). Each pattern owns its own armed
    steps, per-step pitch overrides, length, and MIDI channel; what used
    to be one flat `s_seq_step_armed[]` array and a single fixed
    `OP_SEQ_CHANNEL` constant are now behind `active_pattern()`.
    **Triangle now picks between them**, real feedback: "sub menu
    triangle is reserved for other stuff... maybe in triangle we can
    select midi channels for multiple patterns" -- `handle_triangle_
    click()` now branches on `s_active_mode` (melodic keeps the scale
    picker unchanged; sequencer gets a new pattern picker), reusing the
    mode-picker's exact touch-click-then-push-past-50%-selects gesture.
    Default channel per pattern claims from the TOP of the 15 MPE Member
    Channels downward (pattern 0 = nibble 15, exactly the original
    single-pattern behavior unchanged) -- a default, not a reservation:
    `expression.c`'s own live per-touch channel allocator is untouched,
    so this can only ever collide with live touch playing in the rare
    case of using many fingers at once while ALSO running multiple active
    patterns, an accepted edge case rather than something worth shrinking
    live MPE polyphony to avoid.
  - **Per-step pitch assignment** -- real feedback: "we need a way to
    assign pitches to the notes." Holding a step past
    `OP_SEQ_PITCH_ASSIGN_HOLD_MS` (350ms) opens a note-picker view of the
    whole grid, reusing `lighting.c`'s own melodic-idle root/natural/
    sharp coloring so it reads as a natural extension of melodic play
    rather than a new visual language; the step's currently-assigned note
    pulses white (this file's established "selected" language). A fresh
    touch on any pad commits that pad's current note as the held step's
    pitch, independent of the step's own position -- the same "hold the
    trig, play the note" workflow Elektron/Circuit-style hardware
    sequencers use for exactly this problem, the closest real-hardware
    precedent that fits touch-only input with no encoders. Releasing the
    originally-held step without picking a different pad cancels with no
    change. The quick-tap arm-toggle is unchanged and fires independently
    of this -- holding a step both toggles it (immediately, on touch-down,
    same as always) AND, if you keep holding, also opens pitch-assign;
    these don't conflict.
  - **Manual transport + length on SW1 "-"/SW2 "+"**, previously unused
    in sequencer mode (`octave_control.c`'s own default octave-shift
    function already yields whenever `tiles_op_mode_owns_pad_grid()` is
    true). Real feedback: "we need a button that starts and stops
    sequencer... we need to be able to adjsut length of sequence with
    shift + -." Plain "-"/"+" = stop/resume
    (`tiles_midi_clock_set_running()`, new in `midi_clock.c`, a no-op
    while a real external clock is present -- the DAW's own transport is
    the only valid control then, mirroring tap tempo's own "external
    always wins"); "+" specifically only fires if a tempo already exists
    (tap-established or external) rather than setting `running=true` with
    nothing to ever advance `pulse_count`. Circle held FIRST, then a
    fresh "-"/"+" press, steps pattern length by +/-1 (1-24) instead --
    circle is this board's own established "shift" identity, matching
    `expression_control.c`'s own "hold the modifier first" square-then-
    "-"/"+" convention; only that order is supported, not the reverse
    (holding "-"/"+" first then tapping circle just runs each button's
    own solo action independently -- acceptable "off-label" behavior, not
    a crash or a stuck state). Resolved on release with the exact
    "combo flag set during hold" shape `octave_control.c`'s own
    solo-step-vs-combo race fix already established, reused rather than
    reinvented.
    **Required revising last round's circle-tap handler**: it used to
    commit a tap-tempo tap on circle's raw PRESS edge. Holding circle
    then pressing "-"/"+" for a length change would already have
    registered a spurious tap before "-"/"+" was ever touched -- the
    combo-conflict check only looks at sibling buttons AT press-time and
    can't catch a sequential gesture like that. Fixed by deferring the
    actual `tiles_midi_clock_register_tap()` call to RELEASE, cancelling
    candidacy the instant "-"/"+" is seen held mid-hold, but still using
    the ORIGINAL press timestamp for the registered tap -- tap-tempo
    accuracy is unaffected by the small press-to-release latency of a
    real tap.
  - **Quantized (re-)start** -- real feedback: "we need to quantice to
    midi clock when that is conected," confirmed to mean: entering
    sequencer mode, or pressing "+", while a clock (real or tap-tempo) is
    already running waits for the next quarter-note boundary
    (`s_seq_pending_start`, consumed in `seq_advance_clock()`) before
    actually resetting to step 0, instead of jumping in at whatever
    `pulse_count % OP_SEQ_CLOCKS_PER_STEP` the clock happened to already
    be at. This also fixes a real latent bug from the original build:
    entering sequencer mode while an external clock was already running
    landed on an essentially random step immediately (`elapsed = pulse_
    count - 0` from a fresh `seq_start()`), never actually phase-aligned
    to the DAW's own beat.
  - **Current step lit up always**, real feedback superseding this file's
    own earlier explicit "no playhead flash on an empty step" rule: an
    unarmed (or armed-but-paused) current step now shows a dim cursor
    color (`OP_SEQ_CURSOR_LEVEL`) instead of falling through to off, so
    the playhead is always visible regardless of content or transport
    state. Steps at or beyond the active pattern's current length still
    render off regardless, matching the existing "unavailable are off"
    rule for a pattern shortened below 24 steps.
  **First real-hardware pass on the redesign above found four real
  issues, all fixed this round:**
  1. Pitch assignment was momentary (had to keep the originally-held step
     physically touched the whole time you picked a note) -- real
     feedback: "it should be a toggle to set pitch of sequencer note. not
     a momentary thing." `handle_pitch_assign_taps()` no longer cancels
     on releasing the held step; it stays open until a fresh touch on ANY
     pad commits, with triangle now serving as the explicit cancel-with-
     no-change escape hatch (`handle_triangle_click()`'s new branch).
  2. Diamond's LED glowed continuously the ENTIRE time any non-melodic
     mode was active, not just while the mode picker was actually open --
     real feedback: "the led for modes should light up on menu on not
     alwayus." `OP_DIAMOND_LED_MODE_ACTIVE_LEVEL` removed entirely;
     diamond now only ever lights via `render_menu()`'s own write while
     the picker is genuinely showing.
  3. "-"/"+" had no transport-state feedback at all (dark whenever not
     being physically pressed) -- real feedback: "the led for start and
     top shoukld light up as toggles respectively." New
     `OP_TRANSPORT_LED_LEVEL`, lit on exactly one of the two reflecting
     current `clock.running`, written in all three sequencer-mode render
     functions (normal view, pattern picker, pitch assign).
  4. Stop/start had no rewind/retrigger semantics -- real feedback: "play
     position of head should reset when stop click twice and if playing
     and play again it starts from the top again."
     `handle_transport_and_length()` now checks `tiles_midi_clock_is_
     running()` (new, see `midi_clock.c`'s own entry) at release time: a
     solo "-" while ALREADY stopped rewinds to step 0 instead of
     no-opping; a solo "+" while ALREADY playing re-arms
     `s_seq_pending_start` (a quantized retrigger, not an immediate jump)
     instead of no-opping. Fixed a real gap found while implementing this:
     "+"'s original resume path never actually set `s_seq_pending_start`
     at all, meaning resuming from a manual stop would fast-forward
     through however many pulses had silently accumulated while stopped
     (`pulse_count` keeps advancing regardless of `running`, see
     `midi_clock.h`'s own header) instead of quantizing cleanly -- now
     fixed alongside the new rewind/retrigger logic.
  Also found and fixed in this same pass: two real "stuck haptics" bugs
  (see `haptics.c`'s own entry for the full diagnosis) and deep sleep now
  silencing haptics, plus `standby.c` giving sequencer mode its own
  longer idle/deep-sleep timeout (see that file's own entry) --
  sequencer's real feedback this round covered lighting/transport
  interaction issues specifically; the haptics and standby-timing fixes
  address separate reports from the same testing pass.
  **Not hardware-verified beyond the four fixes above** -- the underlying
  multi-pattern bank, per-step pitch storage, and quantized-start math
  itself weren't independently re-verified this round, only the specific
  symptoms real feedback called out.
  **Per-step probability (with a master on/off) and ratchet added**, real
  feedback: "yes per step probablility but we should be able to turn that
  on and off, 2 retrigger yess but we need to be able to control that
  feature." Both reuse the exact same hold-a-step gesture pitch assignment
  already uses, escalating through two further hold thresholds
  (`OP_SEQ_PROBABILITY_HOLD_MS` 1.2s, `OP_SEQ_RATCHET_HOLD_MS` 2.2s) --
  the same shape `standby.c`'s own circle-hold already uses to escalate
  from screensaver (4s) to deep sleep (8s), reused rather than inventing a
  new gesture. `s_seq_edit_mode` generalizes what used to be a
  pitch-only `s_seq_pitch_assign_active` flag into a 3-way enum
  (`OP_SEQ_EDIT_PITCH`/`_PROBABILITY`/`_RATCHET`); `pitch_assign_enter()`/
  `_exit()`/`handle_pitch_assign_taps()`/`render_pitch_assign()` are now
  `edit_enter()`/`edit_exit()`/`handle_edit_mode()`/`render_edit_mode()`,
  the last two branching on which of the three is active.
  Probability and ratchet are a fundamentally different INTERACTION shape
  from pitch, not just a different value: pitch is a discrete pick-from-24
  (hence last round's "make it a toggle" fix, since holding one pad while
  tapping a second is awkward), while these are a continuous live DIAL --
  Hall depth of the SAME still-held pad maps directly to the value every
  scan while held (`probability_percent_from_depth()`/`ratchet_count_
  from_depth()`, both using the same ~900 full-scale reference this file
  already uses elsewhere), and releasing simply leaves whatever the value
  last read. Only one pad is ever needed for either, so neither has
  pitch's original two-finger problem to begin with. Rendered as a simple
  linear meter across all 24 pads (`render_value_meter()`, shared by
  both, amber for probability / blue for ratchet) -- how many pads are lit
  is directly proportional to the current value, giving immediate visual
  feedback as you press deeper or shallower.
  Probability is rolled once per step OCCURRENCE, not per ratchet hit --
  a whole step either fires (with all its ratchets) or it doesn't,
  matching how real hardware "trig probability" works, not a per-hit
  coin-flip. Gated on a new per-pattern `probability_enabled` (default
  off, so every existing/new pattern behaves exactly as before until
  explicitly turned on) -- real feedback: "we should be able to turn that
  on and off." Toggled by a plain circle click while the pattern picker is
  open (circle is otherwise unclaimed there -- length-adjust is already
  disabled in that view too), shown on circle's own LED; this also
  required excluding the pattern picker and any per-step edit view from
  tap-tempo's own `mode_ok` check (`handle_circle_tap()`), since tapping a
  tempo mid-edit never made sense anyway and it's what frees circle up for
  this new meaning specifically inside the picker.
  Ratchet fires additional sub-hits WITHIN a single step's own pulse
  window (`OP_SEQ_CLOCKS_PER_STEP`, 6) -- `seq_enter_step()`'s one-time
  note-on logic was split into a shared `seq_fire_note()` (no step-index/
  probability logic of its own) reused by both the step's first hit and
  every subsequent ratchet hit, which `seq_advance_clock()` now fires on
  schedule (`s_seq_ratchet_remaining`/`s_seq_ratchet_interval_pulses`/
  `s_seq_next_ratchet_pulse`) before checking for a step boundary each
  scan, so a hit due right at a step's edge is never skipped. A pattern
  switch mid-ratchet explicitly clears this state (`handle_pattern_menu_
  taps()`) -- the only path that changes what step index means without
  going through `seq_enter_step()`'s own reset, so it needed an explicit
  fix to avoid firing a stray hit against the NEW pattern's data.
  Armed steps with probability < 100% (only while `probability_enabled`,
  so a dialed-but-currently-inert value doesn't mislead) or ratchet > 1
  now tint away from plain dim red toward amber/blue respectively on the
  normal step view, so which steps have something set is visible at a
  glance without re-opening each one's own edit view.
  **Not hardware-verified** -- the escalation timing, the Hall-depth-to-
  value mapping, the meter rendering, and the ratchet sub-hit timing are
  all first attempts.
  **Second real-hardware pass found two more real problems, both fixed:**
  real feedback: "the interactions and mapping are not intuitive yet...
  time escalation is good but not for so many features. -+ aree not doing
  properly single play. when stopped makes play, play when playing brings
  head to start point again, stop when stopped brings head back to pad1,
  stop when playing jsut stops and play head strays the same without
  restarting."
  1. **Ratchet moved off the hold-escalation timeline.** Three timing
     tiers (pitch/probability/ratchet) on one continuous hold asked for
     too much precision to land on the right one reliably. Ratchet now
     has its own separate, immediate entry instead: holding circle FIRST,
     then touching a step, opens ratchet-edit directly with no wait at
     all (`edit_enter_ratchet()`, called from `seq_handle_step_taps()`'s
     own check for circle already held on a fresh touch) -- an extension
     of the SAME "circle = shift, modifies whatever you're doing" pattern
     circle+"-"/"+" already established for length, not a new convention.
     Pitch and probability still share the original two-tier hold escalate
     (`OP_SEQ_PITCH_ASSIGN_HOLD_MS`/`OP_SEQ_PROBABILITY_HOLD_MS`) -- just
     one fewer tier to land on now. This new circle+step combo needed the
     same fix circle+"-"/"+" already needed: `handle_circle_tap()`'s
     mid-hold tap-candidacy cancellation now also cancels on ANY pad
     touch (`any_pad_touched()`), not just minus/plus, since circle's own
     press already happens before the step is touched in this ordering
     too.
  2. **"+"/"-" resume vs. restart were conflated.** Real feedback pinned
     down four DISTINCT required behaviors precisely: "+" while stopped
     must RESUME exactly where a plain stop left the playhead (not reset
     it); "+" while already playing must RESTART from step 0; "-" while
     playing must just stop in place; "-" while already stopped (a second
     stop) rewinds to step 0. The first three of these were already
     correct, but "+" while stopped was silently behaving like a restart
     too, because the one `s_seq_pending_start` flag routed BOTH cases
     through `seq_reset()` (always step 0). Fixed with a second flag,
     `s_seq_pending_restart`, set true only for a genuine restart (fresh
     sequencer-mode entry, or "+" while already playing) and false for a
     plain resume ("+" while stopped) -- `seq_advance_clock()`'s pending-
     start handling now branches on it, calling the new
     `seq_resume_current_step()` (re-fires wherever the playhead already
     was, no position reset) instead of `seq_reset()` for the resume case.
  **Diamond's LED found stuck lit after canceling the mode picker**, real
  feedback: "load a fix for exiting menues, led stays toggled." Diamond
  has a PERMANENT override claimed since `tiles_op_mode_init()`, so
  `buttons.c`'s own `refresh_all_button_leds()` (run whenever standby
  ends) deliberately skips it -- "that controller's own next scan
  repaints it correctly" is `buttons.h`'s own documented contract for
  override-held buttons, but nothing was actually doing that repaint on
  this specific path. Selecting a mode from the picker was already fine
  (`set_active_mode()`'s own trailing override write covers it), but
  CANCELING the picker with a plain diamond click -- `menu_exit()` alone,
  no `set_active_mode()` call -- left diamond stuck at `render_menu()`'s
  own bright `OP_DIAMOND_LED_MENU_LEVEL` forever, since nothing wrote to
  it again afterward. `menu_exit()` now also writes diamond's override
  back to 0.0f directly; the mode picker is only ever reachable from
  melodic mode to begin with, so that's always the correct value to
  restore here. The scale/pattern pickers don't share this bug -- both
  already render diamond at 0 the whole time they're open, so there was
  nothing to restore.
  **Mode picker now only lights/selects AVAILABLE modes**, real feedback:
  "the mode selector has all these lights always on. only availabkle
  modes shouyld be on meaning for now only sequencer, and the note mode."
  New `row_is_available()` gates both `render_menu()` (an unavailable
  row renders fully off, the "unavailable = off" language this file's own
  scale/pattern pickers already established for their own reserved slots)
  and `handle_menu_taps()` (an unavailable row is a no-op to select,
  though it still gets the same touch-click haptic acknowledgment every
  OTHER pad does while browsing -- matches the scale picker's own
  identical precedent for its undefined slots).
  **Arp mode removed entirely, replaced by a real guitar/bass fret
  mode.** Real feedback: "lets imoplenment for note mode a guitar fret
  mode for 4 stings with the structure of bass shapes... this is going
  to be mode 3 on the mode function selector." Arp was never more than a
  selectable stub (no real trigger logic, just circle's beat-flash
  override) -- rather than leave it as unreachable dead code once its
  picker row was needed for a real feature, it's gone: `OP_MODE_ARP`,
  `OP_ROW_ARP`, `OP_MENU_ARP_*`, and the circle-override claim/release
  tied to it are all deleted. A genuine future arp mode would be a fresh,
  deliberate feature request, not scaffolding worth preserving unreachable.
  Chord keeps its own row and enum value (still a real planned mode, just
  correctly marked unavailable now like guitar's neighbor row) -- only
  arp was removed, since guitar directly took its exact slot. Reading top
  to bottom, the available modes land as melodic (1st), sequencer (2nd,
  skipping chord's dark row), guitar (3rd) -- matching real feedback's
  own "mode 3" under the most natural way to count "the 3rd real mode."
  **Guitar/bass fret mode itself** -- see `note_map.c`'s own entry for
  the note-mapping/fret-marker design (row=string in standard TAB order,
  column=fret, standard 4-string bass tuning) and `lighting.c`'s own
  entry for the idle rendering. This file's own share of it is
  deliberately small: `set_active_mode()` pushes `mode == OP_MODE_GUITAR`
  into `tiles_note_map_set_guitar_mode()` (the one flag that makes
  `services/expression.c`'s existing pipeline start playing guitar notes
  and `services/lighting.c`'s existing idle-coloring start showing fret
  markers, with no changes needed in either file), and
  `handle_transport_and_length()` (renamed in spirit though not in name --
  the function now serves TWO otherwise-unrelated mutually-exclusive
  modes rather than just sequencer's transport/length, sharing one read
  of "-"/"+" press state instead of duplicating it) steps
  `tiles_note_map_set_guitar_fret_offset()` by +/-1 per press on release,
  one fret per press with no auto-repeat -- matching this codebase's own
  established "-"/"+" convention everywhere else (octave shift, key
  transpose, sequencer length). Guitar mode deliberately does NOT claim
  `tiles_op_mode_owns_pad_grid()` -- see `note_map.c`'s own entry for why
  this mode is architecturally closer to "melodic mode with a different
  note-mapping function" than to sequencer's custom-rendered instrument;
  it needs only the new, narrower `tiles_op_mode_owns_octave_buttons()`
  (see this file's header for the full reasoning on why that accessor
  exists separately from `owns_pad_grid()`).
  Also fixed proactively, applying the exact lesson from the diamond-LED
  bug above before it could recur: "-"/"+" have a PERMANENT override
  claimed by `services/octave_control.c`, and entering guitar mode takes
  over their input without claiming standby_active -- `set_active_mode()`
  explicitly zeros their override LEDs on entry so nothing is left
  showing whatever pattern octave_control.c's own default behavior last
  displayed; leaving guitar mode needs no symmetric fix, since octave_
  control.c's own scan resumes and repaints them correctly on its very
  next tick once it stops yielding.
  **Not hardware-verified at all** -- the row reassignment, the
  availability gating, and the entire guitar mode (note mapping, fret
  markers, "-"/"+" fret-shift) are first attempts, none tried on real
  hardware yet.
  **First real-hardware pass found three more real issues, all fixed:**
  1. Guitar mode's row moved to the LITERAL 3rd row -- real feedback:
     "fret mode is not activating the correct lights, it shouldnt look
     like chromatic." The original placement put guitar on row 4 (reading
     "mode 3" as "the 3rd AVAILABLE mode counting down the list, skipping
     chord's dark row"), which didn't match what real feedback expected
     from that same phrase. `OP_ROW_GUITAR` is now literally 3
     (`OP_ROW_SEQUENCER` moved to 2, `OP_ROW_CHORD` to 4) -- the plain,
     unambiguous reading. Since every row check in this file already
     compares against the named `OP_ROW_*` constants rather than raw
     numbers, reassigning their values was the only change needed.
  2. **Probability/ratchet's live dial fixed a real value-corruption bug
     -- in two passes.** Real feedback: "im woried the value decreses
     before the mode is exited... lets make sure the lift dosnt loose the
     feature." Lifting a finger is a continuous physical release: Hall
     depth necessarily passes back down through every lower value before
     the touch sensor reports "released," so a value tied directly to
     CURRENT depth was always getting dragged toward zero by the release
     motion itself, silently overwriting whatever the player actually
     intended right as they let go. A first pass fixed this with
     peak-tracking (only ever write on a new maximum depth) -- immune to
     the release drag, but real feedback after trying it: "we solved the
     push and increse and hold but we didnt solve the reduce value...
     we need some way to hold and not loose value but still have reduce
     power funciton" -- peak-tracking also made it impossible to
     deliberately dial the value back DOWN while still holding, since any
     decrease at all was ignored, intentional or not. Replaced with a
     release-guard instead (`OP_SEQ_EDIT_RELEASE_GUARD_DEPTH`, 60 out of
     the ~900 full-scale reference): depth is written on every sample,
     up OR down, as long as it's still at or above the guard; only the
     FINAL approach toward the sensor's true near-zero rest depth (below
     the guard) is ignored, since that's the one part of a decrease that
     really is unambiguous -- a deliberate low setting settles and holds
     above the guard, while an actual release always finishes by crossing
     below it on the way to full contact loss. This does cap the dial's
     lowest reachable value while holding to just above true zero rather
     than exactly 0 (an accepted trade -- a step that should never fire
     is better served by disarming it than by fighting this gesture down
     to an exact zero). **`OP_SEQ_EDIT_RELEASE_GUARD_DEPTH` is a
     first-attempt guess, not yet validated against real capture data**
     the way `expression.c`'s `MIN_STRIKE_DEPTH_DELTA` was -- revisit if
     it's cutting off legitimately-low intended values, or still letting
     the release motion sneak in a bad write. `render_value_meter()`
     still shows live feedback either way, since it reads the same
     pattern data this writes into.
  3. **Current step now shows blue when armed but not sounding** -- real
     feedback: "make play head on active pad in sewquencer blue if pad
     active so it wont look as an inactive pad." Needed once per-step
     probability could make an armed step silently skip a given pass
     (`seq_enter_step()`'s own probability roll) -- without this, the
     cursor sitting on that step fell through to the same plain dim-white
     `OP_SEQ_CURSOR_LEVEL` an UNARMED current step shows, making "armed
     but this pass got skipped" visually indistinguishable from "never
     armed at all." New `OP_SEQ_CURSOR_ARMED_*` (blue) fires whenever the
     current step is armed but not this-instant sounding (paused,
     stopped, or skipped) -- bright white still wins whenever it actually
     IS sounding, unchanged.
- `expression.h`/`.c` — see that file's own entry above for the fuller
  strike-detection history; this round's addition: **`MIN_STRIKE_DEPTH_
  DELTA` lowered 300 -> 150**, real feedback: "reduce the deadzone before
  velocity picks up on pad pressed, rn we cant play lightly enough." 300
  was raised from 150 in an earlier round specifically to stop a
  fast-but-shallow flick from reading as a hard strike -- but that same
  section's own comment at the time flagged the exact risk that came
  true: "revisit... if deliberate soft presses stop registering." A
  genuinely light, SLOW, deliberate press has just as little depth as a
  fast shallow flick, so 300 was rejecting both alike with no note at
  all, a worse outcome than the misread it was avoiding. Restored to 150
  -- the one value this section's own real capture data (140 real
  touches) actually validated as the line between incidental contact
  (~96 ceiling) and a genuine press (~192 floor); 300 was a guess layered
  on top of that data, never itself measured against it. The existing
  elapsed-time velocity model still tells fast strikes from slow ones at
  this lower threshold exactly as before -- a fast-but-shallow flick
  still reads as quick (matching how real velocity-sensitive keybeds
  already work, speed of travel being the standard velocity signal, not
  a bug) -- while a slow, light press finally gets to register instead
  of being silently dropped.
- `note_map.h`/`.c` — see that file's own entry above for the fuller
  guitar-mode design; this round's addition: **chromatic added to the
  scale picker as slot 1**, real feedback: "add the first mode as
  chromatic, not major shifting all onse step so we can return to
  chromatic mode." There was no way back to chromatic once a real scale
  was picked, short of a reboot. Chromatic already had a valid interval
  table (it's the boot default), so no note-mapping code changed -- only
  `SCALE_GRID_ORDER[]` did: chromatic now leads, every named scale shifts
  down one slot, and `CUSTOM_6` is dropped (6 reserved slots -> 5) to
  keep the total at 24 -- none of the 6 had a real interval table yet
  regardless, so this costs nothing functional, just one fewer future
  custom slot.
  **Same real-hardware pass surfaced three more real issues, all fixed:**
  1. **A step's armed state could flip as a side effect of just holding
     it to edit** -- real feedback: "when setting the pitch of pad we are
     still affecting note on." `seq_handle_step_taps()` toggled
     `step_armed[]` immediately on the PRESS edge, before it was known
     whether that touch would resolve as a quick tap or the start of a
     350ms hold into pitch/ratchet edit -- so holding an already-armed
     step to edit it silently disarmed it first (or armed an unarmed one)
     the instant contact began, with no way to undo it once the hold
     escalated. Fixed by deferring the toggle to RELEASE, and only when
     `s_seq_step_touch_started_ms[step]` shows this exact touch's
     lifecycle was tracked start-to-finish by this same loop (it's reset
     to 0 by both `edit_exit()` and a circle+touch ratchet entry, so a
     touch that got diverted into an edit gesture, or a pitch-edit's
     pick-a-pad commit landing on a DIFFERENT pad, never also arms/disarms
     the step it touched -- that pad already did its job). Same fix also
     removes the old coupling where entering ratchet-edit (circle held +
     touch) always flipped the step's armed state too, whether or not
     that was wanted.
  2. **The sequencer cursor still showed plain white while actually
     playing** -- real feedback: "play head is still white on play when
     going over selected step," after the armed-cursor blue color above
     was added. `s_seq_note_sounding` is only ever true for an ARMED step
     (`seq_enter_step()` returns before setting it for an unarmed one),
     so it was always a strict subset of "armed" -- but `render_sequencer()`
     checked sounding FIRST and let it win as bright white, meaning the
     one moment blue mattered most (a note actually firing) was exactly
     when it didn't show. Fixed by checking armed before sounding, so an
     armed current step is blue unconditionally, playing or not.
  3. **The mode-picker menu never distinguished "available" from "the
     mode you're already in," and only ever showed flat full-brightness
     colors** -- real feedback: "diamond menu doesnt make sense. we
     defined each row for a family of modes, we defined color ways, but
     lights should only be on when a mode is available in that pad, aksi
     curent mode should pulse." This file's own header already documents
     a standard shared with services/expression_control.c's sub-menu --
     "select color or active color is always white bright pulsing...
     respective less bright but still readable bright for non selected
     and available" -- but `render_menu()` never actually implemented the
     pulsing-selected tier, just a flat hue for every available row
     regardless of whether it was the current mode. Fixed: the row
     matching `s_active_mode` (new `row_is_current_mode()`) now pulses
     white via this file's existing `menu_selected_pulse_level()`, any
     OTHER available row dims to `OP_SCALE_AVAILABLE_LEVEL` (the same
     "readable secondary" level the scale picker's own available-but-
     unselected pads already use), and an unavailable row stays fully
     off, unchanged. Real feedback also separately described "something
     triggering animations when clicking the diamond menu" -- no root
     cause was found for that in isolation (menu_enter()/render_menu()'s
     own state resets were reviewed and look correct); **follow-up: root
     cause found, see `standby.c`'s own entry below.**
- `standby.c` -- root-caused the "something triggering animations when
  clicking the diamond menu" report above. It wasn't the click itself:
  `tiles_standby_scan()`'s automatic idle timeout (60s outside sequencer
  mode) doesn't know or care that `op_mode.c` currently has a sub-view
  open, only whether there's been real touch/button/pedal input --
  reading a menu takes none of those, so simply pausing to look at the
  mode picker (or the scale picker, pattern picker, or a per-step pitch/
  probability/ratchet editor) for 60+ real seconds let the idle timer
  elapse mid-browse, and standby's own screensaver animation silently
  replaced the menu (`tiles_op_mode_scan()` yields the instant
  `tiles_standby_is_active()` goes true -- see `other_feature_owns_
  input()` -- and `tiles_standby_scan()` runs immediately after it in
  `main.c`'s scan order, so the very same tick's screensaver frame
  overwrites whatever the menu had just drawn). Fixed with a new
  `tiles_op_mode_has_menu_open()` accessor (true for any of that file's
  four sub-views) that now holds off the AWAKE -> STANDBY idle-timeout
  check entirely while true, refreshing `s_last_activity_ms` every tick
  the same way genuine touch/button/pedal activity already does -- the
  same shape `tiles_op_mode_is_sequencer_active()` already established
  for giving sequencer mode its own longer timeout, extended here to "a
  menu is open" rather than "a particular mode is active."
- **"the mode light is on at boot" (diamond, then triangle after the swap
  below) -- confirmed real, still not root-caused, given a defensive
  fix instead of a real one.** Traced the full boot sequence (`main.c`)
  twice, across two rounds, and found no logic gap: `tiles_buttons_init()`
  corrects the PCA9685's power-on-lit default to dark before outputs ever
  go live; `boot_sequence.c`'s ~4s animation legitimately lights every
  button LED as part of its own show but explicitly zeros them and calls
  `tiles_buttons_set_standby_active(false)` before returning, which
  repaints every non-override button from `s_debounced` (dark, if not
  physically held); `tiles_op_mode_init()` (the only thing that claims
  the mode-picker button's PERMANENT override) then explicitly writes it
  off again regardless. Real feedback after the triangle/diamond swap
  confirmed the symptom followed the ROLE, not the physical button ("now
  on boot the tringle is light up... it goes away after entering and
  exiting menu") -- ruling out a hardware-specific LED/channel issue,
  since the exact same code path (just pointed at a different physical
  button) reproduces it. That's real signal that this is a software
  timing issue -- most likely a PCA9685 chip-level glitch during power-on
  racing the one-shot init-time write -- but a live serial-log session to
  pin down the exact mechanism was abandoned mid-attempt (real feedback:
  "just fix that simopkle thing") in favor of a direct fix: `tiles_op_
  mode_scan()` now re-asserts the same "off" write on every scan for
  `OP_BOOT_RELIGHT_GUARD_MS` (1000ms) after init, not just once --
  self-healing against whatever the transient actually is, without
  needing to identify it first. Genuinely **not a root-cause fix** --
  flagged here so a future round doesn't mistake the guard window for an
  understood mechanism if something adjacent breaks it (e.g. a boot
  sequence slow enough to still be corrupting the LED after the window
  closes would need `OP_BOOT_RELIGHT_GUARD_MS` raised, not re-diagnosed
  from scratch).
- **SW3 (triangle) <-> SW4 (diamond): functionality swapped, fully** --
  real feedback: "switch triangle and diamond functionality swapp them
  fully." Triangle is now the top-level mode-picker single click
  (`handle_triangle_click()`, was diamond); diamond is now each mode's
  own per-mode sub-menu click (`handle_diamond_click()`, was triangle) --
  function names deliberately kept attached to the PHYSICAL button they
  now read (not the role), matching every other button-named function in
  this file. `TILES_DIAMOND_BUTTON_ID`/`TILES_TRIANGLE_BUTTON_ID` and
  their `_COL` counterparts (`board_layout.h`) keep their original
  physical meaning unchanged (diamond is still SW4/GP17, triangle is
  still SW3/GP16) -- only which of the two `op_mode.c` reads for which
  role moved. (This swap is also what confirmed the boot-LED bug above
  is software, not hardware -- see that entry's own updated text.)
- **`game_mode.c` haptics/MIDI cleanup + a menu/game exit redesign** --
  real feedback, three parts:
  1. **Root cause found for "haptics randomly happening in game modes"
     and unwanted MIDI from pads in game mode: `expression.c` never knew
     game mode existed.** Real feedback: "no haptics except for
     selecting game, no haptics if game doesnt requeire it... no midi
     notes grom nimi game unless its slecual effedcs... but no midi from
     pads in game mode." `expression.c`'s `PAD_STATE_IDLE` fresh-touch
     gate already checked three other "who owns the grid" conditions
     (`expression_control`'s sub-menu, `octave_control`'s transpose,
     `op_mode`'s menu/sequencer) but never `tiles_game_mode_is_active()`
     -- so every grid touch during a menu selection, or any incidental
     contact mid-game, ran the completely normal note+haptic strike
     pipeline the whole time, layered on top of whatever `game_mode.c`
     itself was doing with that same touch (worst case, Simon Says: a
     single correct press could produce its own confirmation kick PLUS
     `expression.c`'s touch-pulse PLUS a real note-on and strike-kick).
     Fixed with one added term, `!tiles_game_mode_is_active()`, matching
     the exact style of the other three -- see `expression.c`'s own
     comment there. Simon Says' own two haptic calls (`gsim_update()`'s
     playback echo, `gsim_handle_input()`'s correct-press confirmation)
     are deliberate, real game mechanic (its own original feature
     request: "the haptics play a big part on this one") and were left
     untouched -- they're read via Hall depth, not the capacitive-touch
     pipeline this fix gates.
  2. **New deliberate game-triggered notes, real feedback: "a short
     melody for win or a three note melody for loose."** A small
     non-blocking melody player (`gm_melody_start()`/`_update()`/
     `_stop()`, same "elapsed_ms / STEP_MS" stepping `gsim_update()`'s
     own pattern playback already uses) on a fixed, statically-reserved
     MIDI channel (`GM_MELODY_CHANNEL` 11 -- one nibble below
     `op_mode.c`'s own sequencer-reserved 12-15 range, same "reserve
     from the top down, never through `expression.c`'s dynamic per-
     strike allocator" pattern that file established). Win = a short
     ascending C-E-G-C arpeggio; lose = a plain three-note descending
     line, matching the quote exactly. Hooked into every real win/lose
     point across all five games: `gm_start_round_end()` gained a second
     `is_win` parameter (separate from its existing `red_only` visual
     flag, since `red_only` doesn't reliably mean win/lose -- Tetris and
     Simon Says are always a loss when they reach it, but snake and
     brick breaker route BOTH outcomes through it with the same
     `red_only=false`); Pong's match-win doesn't go through
     `gm_start_round_end()` at all (see that game's own file-header
     reasoning) so it calls `gm_melody_start(&GM_MELODY_WIN, ...)`
     directly from `gp_point_scored()`. The "maybe a quick plucked note
     for interactions" half of the same feedback was NOT built this
     round -- scoped out given the demo-day time budget; flagged here as
     a real, deliberately deferred follow-up, not an oversight.
  3. **New deliberate menu-select haptic, real feedback: "no haptics
     except for selecting game."** `gm_handle_menu_selection()` now
     fires one `tiles_haptics_trigger_kick()` per launch -- the ONE
     haptic this file fires outside Simon Says' own mechanic. Before
     this fix, whatever haptic a menu selection produced was entirely
     the bug in part 1 (an accidental touch-pulse from `expression.c`,
     not a deliberate confirmation), so this is a genuinely new,
     intentional call, not a restoration of something already there.
  4. **New exit gesture, real feedback: "if cicle cliucked in game menu
     it exxits to previuos mode and each othere function button
     oversides gasme mode, exiting and taking to respective menu."**
     `gm_override_button_pressed()`: triangle/diamond (never a live
     control in any of the five games) now override game mode
     unconditionally, menu or mid-game; circle/square (which Pong
     legitimately uses as live paddle controls, SW5/SW6) only override
     from the menu screen, matching the feedback's own "circle clicked
     in game MENU" framing -- overriding them mid-game would break Pong
     itself. Exiting needs no "restore the previous mode" step of its
     own: `op_mode.c`'s `s_active_mode` was never touched while game
     mode ran (the two are already mutually exclusive by design), so a
     plain `gm_toggle()` off is already "back to previous mode." One
     honest rough edge: the button that triggers the exit doesn't open
     its OWN menu on that same press -- `op_mode.c`'s/
     `expression_control.c`'s own "keep edge-tracking current while
     suppressed" pattern (a deliberate anti-spurious-click safeguard
     those files already document) means a release-then-press is needed
     to actually open it, not a single seamless press. Not fixed this
     round -- doing so would mean weakening that safeguard everywhere
     else it's used too, which wasn't part of this ask and isn't
     something to risk the night before a demo.
  5. **Real regression found and fixed same-session: game mode wouldn't
     enter at all after part 4 above landed.** Real feedback: "game mode
     wont louch anymore when 4 function buttons presed at once."
     `gm_override_button_pressed()` checked raw held state, not a press
     edge -- so the instant `gm_toggle()` turned game mode ON (after the
     4-button hold), all four override-eligible buttons were, by
     definition, STILL physically down from that same hold, which the
     very next check read as "triangle/diamond just got pressed" and
     immediately exited right back out, same tick. Fixed with real
     press-edge tracking (`s_gm_override_prev_*`) instead of raw state,
     seeded to `true` for all four right in `gm_toggle()`'s own entry
     branch -- since they're guaranteed already held at that exact
     moment, seeding them true means that first post-entry check
     correctly reads "still held, not a new press" and leaves the menu
     alone. (This fix originally shipped alongside an unrelated,
     unconfirmed `lighting.c` change -- a settling delay for a separately
     reported magenta boot-animation bleed -- that combination froze the
     board solid on real hardware: no USB, no bootloader, no haptics,
     lights stuck mid-frame. Reverted immediately and re-applied ONLY
     this game-mode fix in isolation; the `lighting.c` change is NOT
     included here and needs its own separate, careful attempt with
     actual hardware verification before trying again -- see this file's
     git history around the revert for exactly what was pulled back.)
  6. **Still-remaining haptics/MIDI leak into game mode, real feedback:
     "we have haptics vibration randomly in mini games, that shouldnt
     happen"** -- the `expression.c` fix in part 1 above only guarded
     `PAD_STATE_IDLE`'s fresh-touch gate, so a pad already past IDLE the
     instant game mode activates (near-certain incidental contact during
     the 4-button entry hold, both hands being busy holding it) was
     "deliberately left alone" per that gate's own stated philosophy --
     and kept running its full strike/haptic pipeline, unsupervised,
     for the rest of that game session. Real buttons sitting physically
     right above pad row 1, mashed constantly during Snake/Tetris/Pong/
     Brick-Breaker, made this reliably reachable via PCB vibration.
     Fixed with a new `tiles_expression_force_release_all()`
     (`expression.h`/`.c`): force-ends any `PAD_STATE_NOTE_ON` pad (real
     note-off + haptic stop) and resets every pad to `PAD_STATE_IDLE`,
     called once from `game_mode.c`'s `gm_toggle()` right at the OFF ->
     ON transition -- closing the one gap the touch-gate alone couldn't.
- **`expression.c`'s velocity curve and aftertouch full-scale, both made
  less steep -- real feedback: "make velocity curve and aftertouch less
  steep. more gradual for soft detection better."**
  - `VELOCITY_CURVE_EXPONENT` 1.8 -> 1.0 (plain linear). The 1.8 value
    was a deliberate earlier choice ("suppressing the low end... closer
    to how an acoustic action feels") but its own math cuts against soft
    detection: `d(curved)/d(time)` is smallest exactly where slow/soft
    strikes live for any exponent > 1, compressing a wide range of
    genuinely different soft touches into a narrow band near
    `MIN_VELOCITY` with little felt difference between them. Linear
    gives equal sensitivity across the whole speed range instead.
  - `s_depth_to_aftertouch_full_scale` default 900 -> 1450, now backed by
    unit 2's own real capture session (see `diagnostics/README.md`'s
    entry) rather than an earlier, different unit's data: that unit
    measured a "regular full press" (784-1184, average 918, the source
    of 900) but unit 2's session measured a genuine STRONG STRIKE across
    4 sampled corner pads -- 1697/1488/1328/1280, average ~1448, 60%
    higher. Leaving full-scale at 900 against unit 2's real ~1450
    ceiling meant aftertouch pegged at 127 well before a real hard
    press's actual travel was used -- steep/twitchy, with little room
    left for gradual continued-pressure expression once already maxed.
    Still one shared constant across all 24 pads (a real per-pad curve
    stays out of V1 scope, see `hall.h`), and still just 4 sampled pads
    on one unit, not a full 24-pad/4-unit sweep -- revisit if the rest
    turns out meaningfully different.
  Follow-up, same session, real feedback right after trying the linear
  curve above: "vewlocity shoots up to max xeasely, we need more playing
  range and less inmediatye hard strike." `STRIKE_TIME_MIN_VELOCITY_MS`
  widened 150 -> 300: the previous 140ms-wide window put an ordinary,
  unhurried tap well past the midpoint under the now-linear mapping, so
  it read as most of the way to max. Doubling the slow-end window
  stretches normal-to-slow playing across more of the range without
  changing what counts as a genuinely fast strike (still <= 10ms for
  127).
- **`game_mode.c`'s 4-button entry combo, real feedback: "we need some
  tolerance fotrht e 4 button press for menu open for game mode its very
  hard to trigger."** `gm_combo_held()` requires all four buttons
  simultaneously pressed on the exact current scan tick; the old
  `gm_check_toggle_gesture()` reset its 700ms hold timer to zero the
  instant even one button so much as blipped, so one momentary bounce
  anywhere in that window (four human fingers holding four separate
  physical buttons rock-steady is a harder ask than it looks) threw away
  all progress. Fixed by bridging brief drops the same way `expression.c`'s
  own `TOUCH_DROPOUT_GRACE_MS` already bridges a capacitive touch
  glitch -- a much longer grace window here (`GM_COMBO_DROPOUT_GRACE_MS`,
  250ms, not 12ms), since this is smoothing four-finger muscle micro-
  adjustments, not an electrical blip on one sensor. Only helps a hold
  that's already gotten all four down at least once keep going through a
  wobble -- it doesn't make four fingers land together any easier in the
  first place, so if entry is still hard after this, that's the
  remaining piece to chase.
- **`expression.c`'s velocity model, replaced with a depth+time hybrid --
  real feedback with a live capture showing why the pure-time model
  couldn't work: "slow light press is still to hard velocity wise...
  we need a way to measure light press with distance but we need some
  of the bias of speed as well."** A serial capture caught a deliberately
  light tap on pad 1 committing at `strike_time_ms=10` -- the MAX-
  velocity floor. Elapsed-time-to-threshold measures *quickness*, not
  *force*: a light but quick tap crosses `MIN_STRIKE_DEPTH_DELTA` just as
  fast as a hard quick strike does, so time alone can't tell them apart.
  Fixed by adding a second signal time can't provide: how far
  `peak_depth` had already overshot the threshold at the exact sample
  that crossed it (`depth_score_from_peak()`) -- free, no added latency,
  since `peak_depth` is already tracked every sample before commit. This
  works because Hall samples arrive at a roughly fixed rate: a hard
  strike covers much more depth between two samples than a light one
  does, so the sample that finally crosses 150 typically overshoots it
  by a lot for a hard hit and barely clears it for a light one --
  overshoot is a real proxy for force that was sitting right there,
  unused. `velocity_from_strike()` now blends `depth_score_from_peak()`
  (`STRIKE_DEPTH_WEIGHT`, 0.7 -- the dominant signal) with the existing
  `time_score_from_strike_time()` (0.3, kept as a bias per "some of the
  bias of speed as well," not dropped). `STRIKE_DEPTH_OVERSHOOT_FULL_SCALE`
  (350) is a first-attempt guess, not measured -- there's no captured
  peak-depth-at-commit data yet the way `MIN_STRIKE_DEPTH_DELTA` has;
  revisit once a real light-vs-hard session records those numbers
  directly instead of only `strike_time_ms`.
- **`note_map.c`'s scale picker, trimmed and reordered -- real feedback:
  "we have to many scales on the scale selector and its kinda
  overwhelming."** Checking every scale's actual note count first
  (real feedback: "cut the ones that have less than 5 notes") found
  nothing to cut -- every scale already has 5+ notes (Pentatonic Major/
  Minor, Egyptian, and Japanese Miyakobushi are the four 5-note scales,
  nothing sits lower; see each scale's own `_INTERVALS[]` array above
  for the exact per-scale count). The criterion changed instead:
  "we should get rid of non atractive
  experimental ones not experiemntal easy to get into" -- Locrian and
  Phrygian (simple, common modes, but among the least immediately
  pleasant-sounding to most ears) got cut, while the genuinely fun
  exotic scales (Arabian, Egyptian, Japanese Miyakobushi, Diminished,
  Whole Tone) were explicitly kept: "they sound fun." Combination
  Diminished and Raga Todi were also cut as the least load-bearing
  once the list needed shortening. Real feedback then reordered what's
  left: "rearange so we start with chrommatic, major, minor and then
  the rest" -- Ionian and Aeolian (major/minor) now lead right after
  chromatic, ahead of the other 12 named scales. All four removed
  scales keep their real interval tables in `scale_table()` (removing
  them outright would have been needless churn) -- they're simply no
  longer placed in `SCALE_GRID_ORDER`, so the picker can't reach them.
  Dropping 4 named scales freed 4 grid slots, filled with 3 new
  reserved custom placeholders (`TILES_SCALE_CUSTOM_7/8/9`, alongside
  the existing 6) to keep the grid a full 24 slots.
- **`expression.c`'s velocity model, corrected again -- the depth+time
  hybrid from the previous round still wasn't right, real feedback:
  "light preasure taps do medium velocity when they should do minimum
  velocity... gentil slow taps and fast light taps [need to be] low
  velocity it cant be strong and strong and deep has to be consistently
  strong like a piano. it shoul dfeel like a hammer action piano."**
  The previous fix measured depth overshoot at the exact INSTANT
  `MIN_STRIKE_DEPTH_DELTA` was crossed -- but a fast-but-light touch can
  still produce a real-looking overshoot at that one instant if it
  happened to be moving quickly right as it grazed the threshold, which
  is exactly the "fast light tap reads as medium/strong" symptom. A
  real piano hammer doesn't have this failure mode because its
  mechanism can't physically cover full key travel fast without real
  force behind it -- on a shallow Hall/capacitive pad, "fast" and
  "light" genuinely can coexist at the same shallow travel distance, so
  one instantaneous depth sample can't be trusted to mean "hard."
  Fixed with a real, if small, architecture change: a new
  `VELOCITY_FOLLOWTHROUGH_MS` (20ms) window after crossing, during which
  `peak_depth` keeps being watched (it was already tracked every
  sample regardless -- see `peak_depth`'s own struct comment -- so this
  needed no new tracking, just a later commit point) before the note
  actually fires. A gentle slow tap and a fast light tap both plateau
  near the threshold during that window (neither has real force behind
  it to carry depth further); a strong, deep press keeps climbing well
  past it regardless of exactly how fast it started -- this is the
  actual "hammer action" signal, depth over a real observation window,
  not an instantaneous sample and not elapsed time to a shallow
  threshold. Costs up to 20ms of onset latency for a touch that stays
  down that long; a touch releasing sooner still commits immediately on
  release with whatever depth it reached (`commit_on_release`,
  unchanged), so a genuinely brief tap adds none. `STRIKE_DEPTH_WEIGHT`
  raised 0.7 -> 0.85 (depth even more dominant now that it's measured
  properly) and `STRIKE_DEPTH_OVERSHOOT_FULL_SCALE` raised 350 -> 550
  (a hard strike has a real window to keep climbing in now, not one
  sample, so it can plausibly overshoot further than before) -- both
  still first-attempt guesses, not measured against real strikes.
- **Chord mode -- TILES' 4th play mode, built from real feedback: "lets
  create a mode that does chords on one side colum 1 and 2 (pad19
  cchord, pad20 d chord, pad13 chord e and loke that.) and melody in
  columns 3456 in a 4x4 grid starting with c in pad 21. the main thing
  is chords are one octave lower than melodic... leds for chords are
  color blue all of them together and meody does the usual black and
  white keys with root in sentia color. so this would go as our 4th
  play mode and its designated blue."** A genuine hybrid of this
  codebase's two existing mode architectures rather than a new one:
  columns 1-2 (pads 1/2/7/8/13/14/19/20, the "chord strip") behave like
  the sequencer -- `op_mode.c` claims those 8 pads directly and drives
  MIDI itself, bypassing `expression.c` -- while columns 3-6 (the
  4x4 "melody grid") behave like guitar mode -- pads stay unclaimed and
  play through `expression.c`'s completely unmodified touch/velocity/
  aftertouch/haptics pipeline, just remapped to different notes via
  `note_map.c`. Neither existing pad-ownership accessor could express
  that split: `tiles_op_mode_owns_pad_grid()` is all-or-nothing (claims
  every pad, the way sequencer mode needs, or none, the way guitar mode
  needs). A new, finer-grained `tiles_op_mode_owns_pad(uint8_t
  logical_pad)` was added alongside it in `op_mode.h`/`op_mode.c`:
  every mode except chord just defers to the existing blanket accessor
  (identical behavior to before this function existed), while chord
  mode answers per-pad, true only for its own 8 chord-strip pads.
  `expression.c`'s `PAD_STATE_IDLE` fresh-touch gate now calls this
  instead of the blanket accessor, so chord mode's 16 melody pads keep
  triggering real strikes exactly like melodic play while its 8 chord
  pads are correctly excluded.

  Both regions share one row/column reading, `note_map.c`'s new
  `chord_mode_degree()`: columns 1-2 fold to a 2-wide degree grid
  (`musical_row * 2 + (col - 1)`, musical_row counted bottom-to-top same
  as everywhere else in this file), columns 3-6 fold to a 4-wide degree
  grid the identical way -- both are the same bottom-to-top,
  left-to-right reading every other mode already uses, just narrowed to
  fewer columns, which is why pad 19 (bottom-left of the chord strip)
  lands on scale degree 0 (the root, C in the default key) and pad 13
  lands on degree 2, matching "pad19 cchord... pad13 chord e" exactly
  once folded through whatever scale is currently active. The melody
  grid reuses `tiles_note_map_get_note()`'s existing scale-degree fold
  (factored out into a new shared `note_for_scale_degree()` helper so
  both the melody path and the chord math below call the identical
  logic, rather than duplicating it) -- it plays as a real scale-aware
  4x4 melodic sub-grid, root/natural/sharp coloring included, exactly
  like normal melodic play just narrower.

  Chord notes are computed, not looked up from a fixed chord table: a
  new `tiles_note_map_get_chord_notes()` takes a chord pad's own scale
  DEGREE (not semitone) as the root, then folds root+2 and root+4
  through that SAME scale-degree math -- automatically producing
  whichever triad quality (major/minor/diminished) the currently active
  scale implies for that degree, the same way a real chord-organ/
  autoharp harmonizes each scale degree, with zero hardcoded
  major-vs-minor logic. Real feedback's "chords are one octave lower
  than melodic" is one flat `-12` semitone shift applied to all 3 notes
  after that fold. Each chord pad's 3 notes fire together as real
  polyphonic MIDI (`tiles_midi_note_on()` x3 on press,
  `tiles_midi_note_off()` x3 on release) on a new dedicated channel,
  nibble 10 (`OP_CHORD_CHANNEL`) -- one below `game_mode.c`'s own
  `GM_MELODY_CHANNEL` (11) and below the sequencer's own per-pattern
  channels (12-15), so none of this codebase's direct-MIDI claims
  collide. Like those existing claims, this is a default, not a hard
  reservation: `expression.c`'s live per-touch MPE allocator (still
  running for chord mode's own melody columns) is untouched, so a
  genuine collision is only possible while using many fingers at once
  AND holding chords simultaneously -- an accepted edge case, not worth
  shrinking live MPE polyphony to avoid, matching the precedent already
  established for game mode's and the sequencer's own channel claims.
  Per-pad (not single global) sounding-note state, since -- unlike the
  sequencer's one-step-at-a-time playhead -- multiple chord pads can
  plausibly be held down together.

  "leds for chords are color blue all of them together" -- `lighting.c`'s
  `pad_desired_rgb()` gained a chord-region branch (checked before the
  normal root/natural/sharp logic) that paints the whole strip one flat
  idle-brightness blue, no per-pad note-role coloring; the melody grid
  needed no equivalent branch at all, since `tiles_note_map_is_root_pad()`/
  `_is_natural_pad()` already read whatever `tiles_note_map_get_note()`
  currently returns, which is automatically chord-mode-aware once
  `note_map.c`'s own degree folding is. Chord's mode-picker color
  (`OP_MENU_CHORD_R/G/B` in `op_mode.c`) changed from its old placeholder
  green to blue for the same reason -- "its designated blue" -- matching
  the strip's own idle color. `col_is_available()` now returns true for
  chord's column (it was hardcoded false while chord was still an
  unbuilt stub), making it selectable from the mode picker for the
  first time.
- **Chord mode, corrected after real hardware testing -- real feedback:
  "chords are not structured propperly. they should all be the chords
  on a same key and real chords not random 3 note group. and melodic
  side should me in key not chormatic," then "make chords an octave
  loower and make the chords with inversions to make them feel more
  musical take insouration from the [Omnichord]."** Root cause of the
  first complaint: `note_map.c` boots into `TILES_SCALE_CHROMATIC` by
  default, and chord mode's skip-one/skip-two scale-degree
  harmonization only produces a real musical third/fifth when the
  active scale genuinely has 7 notes (that's the textbook definition of
  diatonic harmony) -- on the 12-note chromatic scale, "skip two scale
  degrees" is just "skip two semitones," which is exactly the "random 3
  note group" (and chromatic-sounding melody) heard on real hardware.
  Fixed with a new `chord_mode_scale_table()` in `note_map.c`: uses the
  globally selected scale as-is when it's already diatonic (7 notes --
  so picking Dorian/Lydian/Mixolydian etc. from the picker still colors
  chord mode correctly), falls back to Ionian (major) otherwise. Both
  the chord strip (`tiles_note_map_get_chord_notes()`) and the melody
  grid (`tiles_note_map_get_note()`'s chord-mode branch) now go through
  this instead of whatever scale happens to be globally selected, so
  melody is always real and "in key" and chords are always real triads,
  regardless of the global scale. `tiles_note_map_is_root_pad()`'s own
  modulo was updated to match, or it would have mislabeled root pads
  under the same fallback. `note_for_scale_degree()` was split into a
  table-parameterized `note_for_scale_degree_using()` to let both the
  normal path and this new fallback path share the identical fold logic
  rather than duplicating it.

  "make chords an octave loower": `CHORD_OCTAVE_DOWN_SEMITONES` raised
  12 -> 24 -- one octave down read as too close to the melody grid's own
  register to feel like a distinct bass/pad layer.

  "make the chords with inversions to make them feel more musical, take
  insouration from the [Omnichord]" -- real chord organs/autoharps (the
  Omnichord being the canonical example) don't just stack every chord
  root-third-fifth fresh off its own root; across 8 different
  scale-degree roots that would mean each chord's overall register
  keeps climbing as you move up the strip, never settling into a
  compact "pad" sound. Added `tiles_note_map_nearest_pitch_class(note,
  anchor)` in `note_map.c` -- folds `note`'s own pitch class to whichever
  octave sits closest to `anchor` (e.g. a fifth 7 semitones above the
  anchor folds to 5 semitones BELOW it instead, since |-5| < |+7|,
  automatically producing a real chord INVERSION wherever that's what
  keeps the chord compact). `op_mode.c`'s `chord_pad_note_on()` now
  tracks the actual 3 notes of whichever chord last STRUCK
  (`s_chord_voice_anchor_notes`, reset only on a fresh entry into chord
  mode -- see `set_active_mode()`) and re-voices each new chord's raw
  root-position triad toward the previous chord's corresponding voice
  (root toward root, third toward third, fifth toward fifth) before it
  ever fires, one octave-fold per voice. The very first chord struck in
  a session has no anchor yet and plays in plain root position, exactly
  as `tiles_note_map_get_chord_notes()` returns it. This function lives
  in `note_map.c` rather than being kept an internal op_mode.c detail
  because it's pure pitch-class math with no state of its own, but the
  "last chord played" state it needs stays in `op_mode.c` alongside
  this file's other chord-playback bookkeeping -- `note_map.c`'s own
  note-mapping functions otherwise carry no sequential/temporal state.
- **Chord octave, tuned back down -- real feedback: "chord are a bit
  low so bring them up one octave."** Two octaves down (the previous
  round's fix for "make chords an octave loower") overshot it.
  `CHORD_OCTAVE_DOWN_SEMITONES` back to 12 -- exactly this feature's
  original spec, "chords are one octave lower than melodic."
- **Pitch bend temporarily narrowed to X ("side tilt") alone, Y
  excluded -- real feedback: "lets debug side tilt and ignore other
  tilt for now. we might onlu keep 2 axisx sensing so preassure and
  side tilt for vibrato pitchbend."** An earlier round combined both
  in-plane Hall axes into the bend magnitude (`sqrt(dx^2 + dy^2)`, see
  this file's own history above for that reasoning), but with X and Y
  mixed into one number there's no way to tell, while specifically
  debugging the X/"side tilt" signal, whether a given reading is really
  X or partly Y bleeding in. The tick loop in `expression.c` now uses
  `delta_x` alone as the raw pitch-bend delta; the Y baseline/smoothing
  fields (`pitch_bend_baseline_y`, `pitch_bend_smoothed_y`) are still
  tracked exactly as before, just no longer folded into the bend
  itself, so restoring the combined signal later (if side tilt alone
  turns out not to be enough signal on its own once tuned) is a
  one-line change, not a re-derivation. Matches a real possible
  hardware direction floated in the same feedback: dropping to 2-axis
  sensing entirely (pressure + one lateral tilt axis) for vibrato/pitch
  bend, rather than the current 3-axis (X/Y/Z) Hall read.
- **Pitch bend: two real bugs found debugging X alone, then X+Y
  restored -- the full arc of a single real-hardware investigation.**
  Isolating the signal to X (see the entry above) let two problems that
  the combined X+Y magnitude had been partially masking get properly
  diagnosed and fixed:

  1. **Sign-flicker vs. genuine tilt.** A completely straight, non-
     tilted hold still crossed `PITCH_BEND_DEADZONE_COSINE_DELTA` on 35%
     of samples and `s_pitch_bend_max_cosine_deviation` on 11.5% (461
     captured) -- real "give" under sustained pressure, not noise. Real
     tilt sustains one sign for the length of the gesture; give flickers
     back and forth within ~100-200ms. `PITCH_BEND_ARM_MS` raised 15 ->
     120 to actually require a held direction before counting a
     deviation -- see that constant's own comment.

  2. **Baseline drift.** A dedicated ~9s straight hold showed something
     the flicker fix couldn't touch: raw X sat consistently 32-112 away
     from the baseline captured in the first `PITCH_BEND_SETTLE_MS` of
     contact, for the ENTIRE hold, no flip-flopping at all -- a pad's
     true resting position under sustained pressure genuinely differs
     from its position in the first 25ms of contact, and a single fixed
     baseline can't distinguish that from a real tilt held just as long.
     Real feedback: "there is always some minor give in pressed mode
     either way." Fixed with a new slow DC-blocking recenter
     (`PITCH_BEND_BASELINE_RECENTER_ALPHA`, ~40x slower than the
     existing smoothing, aiming for a multi-second re-center) --
     baseline_x/y keep drifting toward the CURRENT reading, but freeze
     the instant a real bend run is confirmed, so an actively-held
     deliberate tilt doesn't fade back to center on its own. First
     version recentered toward RAW X/Y directly and made things worse,
     not better -- real feedback, after testing an explicit slow
     deliberate lean held for a full ~10s: "the old thing was not
     working we need to compensate for preassure depth and drift."
     Averaging the raw samples by hand found the real signal WAS
     there (~13-15 raw units of consistent offset) but individual
     samples swing far more wildly around it than that (holding a pad
     LEANED takes continuous muscle tension, naturally less steady than
     a relaxed straight press -- real tremor rides on top of a real
     lean) -- and the recenter step, chasing every noisy raw sample,
     was itself erasing that real signal before a run could ever
     confirm. Fixed by recentering toward `pitch_bend_smoothed_x/y`
     (the existing medium EMA, ~100-200ms, already real-hardware-
     validated as enough to reject fast noise) instead of raw X/Y --
     that field now runs continuously every tick, not just before the
     initial baseline settles.

  With both fixes in place, X alone finally showed SOME real bend
  response to a deliberate lean (previously zero), but still not a
  clean, confident one -- individual samples still swing too much for
  the existing ~100-200ms smoothing to fully separate signal from real
  hand tremor on a single axis. Getting genuinely clean X-only bend
  would need either meaningfully more smoothing (a from-scratch
  estimate: doubling `PITCH_BEND_SMOOTHING_ALPHA`'s smoothing costs
  roughly 300-400ms of onset latency, enough to flatten real fast
  vibrato outright) or Y's noise-averaging back -- real feedback: "yes
  go ahead with X+Y." X+Y combined restored (see this section's own
  "Two axes combined" comment for the actual math, unchanged from
  before this whole round), keeping both bugfixes above -- neither was
  ever X-specific, and Y gets the identical treatment. This round's
  practical takeaway for a possible 2-axis (pressure + one lateral tilt)
  hardware simplification: X alone measurably doesn't have enough
  noise-rejection margin for a clean sustained bend without adding real
  latency, at least on this board's assembly -- averaging two
  uncorrelated axes is what gets noise rejection without that
  latency trade.
- **Pitch bend, hardened after real X+Y playtesting -- a firmware
  freeze, a redundant filter stage, a broken baseline-settle window,
  a failed self-learning depth model, and finally a properly-researched
  depth calibration curve, all found from one continuous round of real
  hardware use.**

  **The freeze.** Extended real play (many pads, several minutes)
  froze the instrument -- not a firmware logic bug, but this file's own
  accumulated debug `printf()` calls: the Pico SDK's USB-CDC stdio
  driver busy-waits internally for up to `PICO_STDIO_USB_STDOUT_TIMEOUT_
  US` (500ms) every time its output buffer fills faster than the host
  drains it, and does NOT return to the main loop while waiting --
  confirmed by reading `pico-sdk/src/rp2_common/pico_stdio_usb/
  stdio_usb.c` directly. With a per-pad diagnostic print firing every
  ~40ms across many simultaneously-active pads, sustained heavy play
  could stack up enough blocking writes to stall touch/Hall/MIDI/haptics
  entirely for seconds. Fixed by deleting both high-frequency prints
  (`[hall-cal]`, the X-only round's `[expression] ... tilt raw`) now
  that they'd done their diagnostic job -- both were explicitly
  "temporary bring-up visibility" from the start. A much lower-rate
  (150ms, globally throttled) `[depth-cal]` print was added later, for
  the depth-calibration capture below specifically, with this exact
  history documented at its own declaration as a reminder of why it
  must stay conservative and temporary.

  **A redundant third filter stage + a broken settle window.** Once
  X+Y was restored, `pitch_bend_smoothed_delta` (an EMA on the OUTPUT
  delta) was still running on top of the newly-cascaded X/Y/magnitude
  inputs -- an uncounted THIRD smoothing stage, on top of the two just
  added, that the "cascading costs ~40% more latency, not 100%" math
  never accounted for. Real feedback: "time before reacting is too
  long." Removed -- the two-stage INPUT cascade already does this job.
  Separately, `PITCH_BEND_SETTLE_MS` (25ms) was never revisited when
  smoothing became a two-stage cascade: 25ms isn't remotely enough for
  two cascaded stages to converge, so the baseline ended up captured
  while the cascade was still sitting near its noisy, often-percussive
  note-on seed -- then, over the next several hundred ms, the cascade's
  own convergence toward the pad's TRUE position read as a large, fake,
  growing "tilt." Real feedback pinned down two symptoms from this one
  bug at once: "it definitely still triggers on full press without
  bend" (the convergence transient firing on essentially every note)
  and "bend reacts but not consistently" (its size depends on how far
  the signal moved between the noisy seed and true convergence, which
  varies note to note). Raised 25 -> 250 first (fully safe), then real
  feedback ("bend takes too long to start... requires max tilt for it
  to happen" -- the 250ms settle stacked with `PITCH_BEND_ARM_MS`'s own
  120ms, ~370ms total before any tilt could reach full confidence)
  pushed both back down -- settle to 120, arm to 60 -- reasoning that
  the cleaner, properly-cascaded signal shouldn't need either constant
  to work as hard as when they were fighting a noisier one.

  **A failed self-learning depth-zone model.** Once genuinely stable,
  real feedback surfaced the underlying physical issue directly:
  "pressure chance also changes tilt. wich makes sence since all axxis
  readings change with preassure." Correct -- and a real captured
  slow, level, no-tilt press-and-release sweep confirmed raw Y
  specifically has a large, repeatable, depth-correlated shift (see
  `PITCH_BEND_DEPTH_Y_CURVE`'s own comment for the actual binned real
  numbers). First attempt: two independently-recentering baselines
  ("shallow" and "deep" depth zones), interpolated by current depth,
  self-calibrating from ordinary play with no dedicated calibration
  step. Real feedback: "preassure is generating crazy pitch bend" --
  WORSE than before. Root cause: both zones are per-NOTE state, reset
  at every note-on, and `PITCH_BEND_BASELINE_RECENTER_ALPHA`'s
  multi-second time constant (correct for nudging an already-close
  baseline) is far too slow to converge a whole new zone from scratch
  within one note's realistic hold -- pressing deep mid-note
  interpolated toward a "deep" baseline still essentially sitting at
  the shallow value, a larger error than having no depth correction at
  all. Reverted.

  **Research before another guess.** Real feedback: "lets think of the
  architecture the math and industry practices for this kind of
  situations before we implement anything else." Real research (not
  memory) found this is a known, named problem: 3-axis magnetic sensors
  are documented to have "cross-axis sensitivity," and the standard
  industry fix is the same technique used for 3-axis magnetometer
  calibration (hard-iron/soft-iron correction) -- fit a correction from
  REAL COLLECTED SAMPLES across the sensor's actual range of motion,
  not a physics-idealized formula or a live-learned runtime model. Most
  commercial Hall-effect joysticks avoid this entire class of problem
  by mechanical design (a gimbal keeps the magnet at a constant distance
  from the sensor, decoupling tilt from any pressure axis); TILES'
  pad combines press-depth and tilt into the same physical mechanism
  (one magnet, one compliant mount), so they're coupled by real
  mechanics, and even purpose-built gimbal joysticks cite the same
  category of error ("mechanical hysteresis" from friction/elasticity)
  as a real, fought-over problem.

  **The real fix: `PITCH_BEND_DEPTH_Y_CURVE`.** A slow, level, no-tilt
  press-and-release sweep was captured on one pad (every raw x/y/depth
  sample logged via the `[depth-cal]` print above), then averaged into
  150-unit depth bins -- real numbers, documented in full at the
  constant's own declaration. Y drops from ~0 near a shallow touch to a
  ~-210 plateau by mid-to-deep press, NOT linearly -- most of the drop
  happens early, then it flattens. X showed no comparably clean trend
  and is left uncorrected. A small `{depth, y_offset}` table plus
  piecewise-linear interpolation (`pitch_bend_y_drift_at_depth()`)
  replaces the single straight-line constant from the previous round --
  this is this file's scaled-down equivalent of hard-iron calibration:
  a curve fit from real collected data, deliberately as simple as
  piecewise-linear rather than a parametric curve fit, captured from
  one representative pad rather than all 24 individually, matching this
  project's own established "a few real numbers, not an over-built
  model" calibration convention -- just extended from a single point to
  enough points to capture the real curve's actual shape. The predicted
  baseline at any depth is now `baseline_y + (curve(current_depth) -
  curve(baseline_depth))` -- correct immediately from the moment
  baseline settles, no learning period needed, unlike the reverted
  two-zone attempt.
- **Pitch bend: the depth-vs-Y curve replaced by adaptive baseline
  recentering, after real research into the actual signal-processing
  problem.** Real feedback on the curve-based fix above: "preassure is
  still doing pitch bend. and for pitch bend to actually work its
  taking a lot of time and tilt for it to register. think about the
  math for a bit first and how we could logically solve this while
  retaining sensitivity or even emulate the feature with the readings
  we have without affecting vertical preassure." A static calibration
  -- whether one constant or a fitted curve -- can only ever be as good
  as the single capture it came from, and does nothing about the noise
  the deadzone/confirmation window still have to fight downstream,
  which is exactly what made genuine tilt slow and insensitive: two
  symptoms of two DIFFERENT causes (a spatial calibration problem and a
  temporal noise-rejection problem) that were never actually separable
  by tuning one static curve harder.

  The real insight, worked through before writing any code (per the
  explicit ask): the pressure-coupling artifact only happens WHILE
  depth is actively changing -- holding steady at any depth doesn't
  introduce it. So instead of predicting what X/Y SHOULD be at a given
  depth (any static model), gate the baseline's own RECENTER RATE on
  whether depth is CURRENTLY changing: `depth_activity` (0..1, driven by
  how much `s->smoothed_depth` has moved since the previous tick,
  normalized by `PITCH_BEND_DEPTH_ACTIVITY_FULL_SCALE`) blends between
  `PITCH_BEND_BASELINE_RECENTER_ALPHA_SLOW` (the original 0.002, several
  seconds -- used once depth holds steady, protecting a genuine held
  tilt from fading, unchanged from every earlier round) and a new
  `_FAST` (0.25, near-instant) used while depth is actively changing --
  there's nothing to lose moving the baseline fast during a press ramp,
  since a bend run is never confirmed during one anyway, and this is
  exactly what stops pressure from reading as fake tilt without needing
  to know in advance what the real X/Y-vs-depth relationship looks
  like. A confirmed bend run still always wins regardless of
  depth_activity (pressing harder while holding a deliberate bend can't
  erase it).

  This is the same underlying idea as the well-known **One Euro
  Filter** (a simple, widely-used technique in gesture/HCI tracking:
  adapt a filter's own rate based on the CURRENT speed of the signal it
  tracks -- heavy smoothing at low speed for stability, light smoothing
  at high speed for responsiveness) and, more directly, how biosignal
  processing rejects a KNOWN motion confound: use an independent
  detector of that confound (there, an accelerometer measuring motion
  known to corrupt a heart-rate sensor; here, depth's own rate of
  change, which is exactly what correlates with this mechanical drift)
  to gate trust in the signal it corrupts, rather than modeling the
  confound's effect directly. Removes `PITCH_BEND_DEPTH_Y_CURVE` and
  `pitch_bend_y_drift_at_depth()` entirely -- this needs no calibration
  capture at all, unlike either previous attempt (the reverted two-zone
  self-learner, or the curve fit this replaces): it measures the real
  depth/tilt relationship live, on every pad, continuously, so it can't
  go stale or fail to generalize from whichever one pad a capture
  happened to be taken from. `PITCH_BEND_DEADZONE_COSINE_DELTA`/
  `PITCH_BEND_ARM_MS` deliberately left UNCHANGED this round (still 0.04
  / 60ms) to isolate whether this mechanism alone fixes the pressure-
  coupling complaint before ALSO retuning the noise-rejection constants
  -- tightening those is the natural next step once this is confirmed
  working, not bundled in blind.
- **Pitch bend: the adaptive-recenter idea was right, its first
  implementation had a real bug -- found and fixed with two more rounds
  of real data, then tuned to "feels good," then extended for fast
  wiggles.** Four real-hardware rounds in one continuous arc:

  1. **Depth-activity gating alone wasn't enough.** Real feedback: "no
     tilt detected ever but preassure is very stable and good." A live
     capture (`depth_activity` added to the `[depth-cal]` print) showed
     WHY: tilting a pad, even trying to hold pressure steady, genuinely
     moves `depth` by a lot (cross-axis coupling running the OTHER
     direction -- tilt bleeding into depth, not just depth bleeding into
     X/Y). With `depth_activity` gated on the raw per-tick delta's
     magnitude, this read as "depth is always actively changing,"
     keeping the baseline in fast-snap mode almost constantly and
     swallowing tilt right along with pressure. Fixed by smoothing the
     SIGNED delta first, then taking ITS magnitude
     (`pitch_bend_smoothed_depth_rate`) -- a genuine sustained press
     keeps a consistent sign tick after tick and stays visible; depth
     wobbling both ways during a tilt (no consistent direction)
     partially cancels instead of accumulating. Same "sign consistency
     separates real motion from noise" principle this file's own
     pitch-bend run-tracking already used for X/Y, applied to depth.

  2. **The activity-to-alpha blend itself was still wrong.** Real
     feedback: "still no pitch bend on tilt but preassure works." A
     second capture (this time printing `baseline_x`/`x2` alongside
     `activity`) showed the baseline STILL tracking the live signal far
     too closely throughout an entire tilt gesture, despite `activity`
     reading only 0.05-0.32 (nowhere near 1.0). Root cause: `_FAST`
     (0.25) is roughly 125x `_SLOW` (0.002) -- a plain LINEAR blend
     means even modest, near-constant background activity already pulls
     the effective rate to 10-40x pure SLOW, defeating the "stay slow
     unless clearly, unambiguously pressing" intent for anything short
     of activity being almost exactly 1.0. Fixed by CUBING
     `depth_activity` before blending (0.3 -> 0.027, 0.1 -> 0.001,
     1.0 -> 1.0 unchanged) -- shifts the whole curve toward "stay near
     SLOW" without moving either endpoint. Real feedback immediately
     after: "wow it feels good."

  3. **Fine sensitivity/latency tuning, once the mechanism actually
     worked.** Real feedback: "it need a tiny bit more sensitivity and
     less trigger time... minimum trigger time and max ease of tilt but
     without loosing precision for regular press." With pressure-
     coupling now suppressed at its actual source instead of fought
     downstream, `PITCH_BEND_ARM_MS` (60 -> 30) and
     `PITCH_BEND_DEADZONE_COSINE_DELTA` (0.04 -> 0.025) shouldn't need
     to work as hard rejecting noise that's mostly already gone --
     changed one round apart from each other (not stacked with the
     mechanism change above) so a regression, if any, is traceable to a
     specific constant.

  4. **Fast wiggles/vibrato weren't registering.** Real feedback: "i
     just need fast wiggles of the keys to activate bend as well." The
     two-stage cascade (see `PITCH_BEND_SMOOTHING_ALPHA`'s own tremor-
     research comment) was applied to BOTH the baseline recenter target
     AND the live signal being compared against it -- but real vibrato
     and hand tremor sit in overlapping frequency bands, so a filter
     steep enough to fully reject tremor necessarily damps a genuine
     fast wiggle too. Split the cascade's two jobs across its two
     stages instead of using both stages for everything: the baseline
     reference stays on the fully-cascaded stage 2 (maximum stability --
     that's what the earlier convergence-transient and false-tilt bugs
     actually needed), while the LIVE signal (`current_cosine_x/y`) now
     reads the lighter, faster-responding stage 1, preserving more of a
     fast wiggle's real amplitude. `pitch_bend_smoothed_magnitude2`
     removed entirely -- once nothing read it anymore (the live
     computation moved to stage-1 magnitude, the recenter target never
     needed a magnitude at all), it was dead weight, not kept for
     symmetry. Real, not fully solved tradeoff: this also lets more raw
     hand tremor back into the live signal than the two-stage version
     did -- worth a fresh capture to confirm this doesn't reintroduce
     jitter during a plain, non-wiggling hold.
- **The fast-wiggle fix above reintroduced pressure-coupled false tilt
  on the very next boot -- found, explained, and reverted to a single
  shared cascade stage.** Real feedback: "the last boot re introduced
  the pitch bend issues with preassure." Splitting `current_cosine_x/y`
  onto the lighter first cascade stage while the baseline recenter
  target (`pitch_bend_smoothed_x2/y2`) stayed on the heavier second
  stage reintroduced exactly the "mismatched lag between the two terms"
  failure this file has hit more than once before (see
  `PITCH_BEND_SMOOTHING_ALPHA`'s own history): during a genuine press,
  the faster-reacting live signal moved ahead of the slower-reacting
  baseline before the baseline's own recenter (even in its FAST regime)
  could catch up, and that transient gap read as fake tilt again.
  `pitch_bend_smoothed_x2/y2` removed entirely; both the live delta and
  the baseline recenter target now read the SAME single EMA stage
  (`pitch_bend_smoothed_x/y`), eliminating the mismatch by construction
  -- there's only one lag now, so nothing can race ahead of anything
  else. Costs back some of the fast-wiggle amplitude the two-stage
  split was preserving; worth a fresh capture to see whether a single
  stage is still enough for genuine wiggles now that the adaptive
  recenter (not raw filtering) carries most of the pressure-rejection
  burden, rather than re-splitting the cascade again blind.
- **MPE pitch bend range: real feedback found the configured value
  wasn't reliably reaching the receiver.** "reduce the range of pitch
  bend, rn we can bend 4 octave" -- reported with
  `TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES` already set to 12 (one
  octave) in firmware, not 48. "4 octaves" is EXACTLY the MPE
  specification's own recommended default -- what a receiver falls back
  to if it never gets an explicit override on the channel it's actually
  reading pitch bend from. The Pitch Bend Sensitivity RPN was only ever
  sent once, on the Zone Master Channel, relying on every receiver
  correctly generalizing that to the whole zone per the MPE spec's own
  convention -- not every real MPE implementation does. Fixed by also
  sending the identical RPN on every Member Channel individually in
  `tiles_midi_mpe_init()` -- redundant on a receiver that already
  handles the Master-Channel version correctly, a real fix for one that
  doesn't. The semitone VALUE itself (12) was deliberately left
  unchanged this round so a fresh test can tell whether the interop fix
  alone resolves it before also re-tuning the value.
- **The single-shared-stage fix above was itself wrong -- collapsed
  both terms onto the LIGHTER cascade stage instead of the heavier
  one, making things worse than ever.** Real feedback: "you've ruined
  the stable version and bend is still extreme and unpredictable...
  everything became more unstable than it was... like it's just
  reading the unfiltered numbers." The diagnosis that both terms must
  read the SAME cascade stage was correct (that invariant is real and
  this file has hit violations of it more than once), but "make both
  terms read `pitch_bend_smoothed_x/y`" picked the wrong half of the
  fast-wiggle split's asymmetry to standardize on. Tracing back through
  every commit since the cubic-blend fix (`git show <rev>:...`, not
  guessing from memory) confirmed: from the ORIGINAL "too jittery,
  inconsistent" two-stage cascade all the way through the depth-rate
  gating fix and the cubic-blend fix that earned "wow it feels good,"
  BOTH the baseline recenter target and the live signal had ALWAYS read
  the fully-cascaded stage 2 (`smoothed_x2/y2`) -- the fast-wiggle
  attempt was the only round that ever moved the live signal onto stage
  1, and only the live signal, not the baseline. So the correct
  "make both terms match" fix was to put current_cosine_x/y back on
  stage 2, not to pull the baseline down onto stage 1.

  Why the wrong stage failed so much harder than merely "some noise
  came back": `PITCH_BEND_DEADZONE_COSINE_DELTA` (0.025) and
  `PITCH_BEND_ARM_MS` (30) were both tuned down to their current,
  tightest-ever values against a live signal that was ALWAYS the
  fully-cascaded one up to that point -- there is no real-hardware data
  showing those thresholds are enough to reject a single-EMA-stage
  noise floor, only a two-stage one. Worse, the baseline recenter
  chases whichever stage `current_cosine_x/y` reads, at up to
  `PITCH_BEND_BASELINE_RECENTER_ALPHA_FAST` (0.25, near-instant) during
  any depth change -- with the live signal downgraded to stage 1,
  baseline_x/y itself started picking up much more raw tremor
  specifically DURING every press/release ramp, exactly the regime this
  whole mechanism exists to keep clean. Two compounding noise sources,
  not one.

  `pitch_bend_smoothed_x2/y2` restored, both terms reading it again --
  functionally identical to the pre-fast-wiggle-attempt arrangement.
  The fast-wiggle-via-cascade-split idea is now considered a dead end,
  not just paused: this file's own tremor-vs-vibrato research already
  explains why splitting ONE shared live signal across two filter
  strengths can't work (real vibrato and hand tremor overlap in
  frequency), so a future attempt at that feature needs an INDEPENDENT
  wiggle detector layered on top of a stable bend path, not a lighter
  filter substituted into this one.
- **MPE pitch bend range, take two: RPN negotiation alone doesn't hold
  up on real receivers.** After sending the Pitch Bend Sensitivity RPN
  on every Member Channel (previous entry above), real feedback: "even
  tho you say that its reduced to one octave it still does more in
  Equator mpe mode." Researched ROLI's own documentation first rather
  than guessing again -- confirmed Equator's Pitch Bend Range is a
  value the USER sets manually in its MIDI/MPE settings to match the
  controller, not one it negotiates automatically from incoming RPN, so
  this looked like a ROLI-specific UI quirk rather than a firmware bug.
  Then: "tried serum and also is bending too far. so its not roli. the
  tilt pushes too far." A second real synth, from a completely different
  vendor, showing the identical symptom rules out "one plugin's
  particular settings quirk" as the explanation -- the real pattern is
  that dynamically honoring a third-party controller's Pitch Bend
  Sensitivity RPN just isn't something real-world MPE hosts/plugins
  reliably do, spec-legal or not.
  Fix: stopped trusting RPN negotiation to control the actual musical
  range at all, and instead compensate defensively at the WIRE value
  itself in `pitch_bend_14bit_from_cosine_delta()`. New constant
  `PITCH_BEND_WIRE_RANGE_COMPENSATION` = `TILES_MIDI_MPE_PITCH_BEND_
  RANGE_SEMITONES / 48` (48 = the MPE spec's own recommended default,
  i.e. the worst-case assumption: a receiver that ignores the RPN
  entirely) -- scales the final +/-8191 wire deviation down to 25% of
  full scale, so that even a receiver stuck at the 48-semitone default
  still produces the intended ~12-semitone swing at max tilt, with zero
  dependency on whether that receiver ever reads RPN 0 correctly. The
  RPN sends themselves stay in place (still correct, still harmless for
  a receiver that DOES honor them), but are no longer load-bearing for
  the actual musical range. Documented, real tradeoff: a receiver that
  DOES correctly honor the RPN (none confirmed yet, out of two tested)
  would now see a narrower ~3-semitone actual range instead of the full
  12 -- worth revisiting if one is ever found; until then, matching the
  two real receivers actually tested is the right default.
- **Temporary `[wiggle-cap]` capture print added, not yet acted on.**
  Remaining real feedback after the cascade restore: "not quite fully
  stable but mostly... when bent it wobbles but also its not sensitive
  to the natural vibrato wiggle." Rather than guess at another
  filtering/gating change, asked whether a real capture of the two
  gestures would help distinguish them -- "yes go ahead." Added a
  25ms-throttled `[wiggle-cap]` print (raw x/y AND cascaded x2/y2,
  plus depth) gated the same safe way as `s_depth_calibration_print_ms`
  (see that constant's own freeze history) but fast enough to actually
  resolve 4-15Hz tremor/vibrato content, for one deliberate single-note
  capture session. Meant to be removed once the capture actually
  informs a real fix, not left running.
- **`[wiggle-cap]` capture analyzed: explains the residual wobble, and
  quantifies (doesn't yet fix) why fast wiggle still isn't sensitive.**
  A real ~5.5s deliberate tilt hold and ~8.6s fast wiggle were captured
  on the same pad and cross-referenced against the actual `[expression]
  pitch bend sent` output from the same session:
  - **Wobble, explained.** During the "steady" tilt hold, real depth
    swung from 527 to 1059 (a human hand isn't perfectly steady in press
    force while concentrating on holding an angle) and the sent bend
    value wobbled by as much as ~600 (out of a ~2047 max) within a few
    hundred ms, in lockstep with those depth swings -- even though
    `pitch_bend_baseline_x/y` were already frozen (a run was confirmed
    throughout). Root cause: the "pure depth change cancels to 0" proof
    only holds when `x2 == baseline_x` (no real tilt) -- for an ACTIVE
    tilt, `delta = (baseline_x - x2) / magnitude` is still inversely
    proportional to whatever magnitude does, so natural press-force
    jitter during a real held bend directly modulates the OUTPUT, not
    just the (already-protected) angle. Fixed by freezing magnitude too,
    the instant a run is confirmed (`pitch_bend_run_magnitude`, chased
    in lockstep with baseline_x/y, same gate) -- extends the exact same
    protection baseline_x/y already have to how that angle gets scaled
    into a cosine. Accepted, documented tradeoff: a deliberate LARGE
    press change mid-run (not just natural jitter) now reads against a
    stale magnitude for the rest of that run; unmeasured how often that
    matters versus the jitter this fixes.
  - **Fast wiggle, quantified.** The tilt hold reached bend deviations
    up to 1929 (94% of the current ~2047 max); the fast wiggle never
    exceeded 240 (12% of that same max), even though the RAW x range
    during the wiggle (288) was comparable to the raw x range during the
    tilt (352) -- similar physical amplitude, wildly different output.
    Cross-referencing the underlying cosine deltas: wiggle peaked around
    0.029, barely above `PITCH_BEND_DEADZONE_COSINE_DELTA` (0.025),
    while the tilt ramped past 0.06 (close to `s_pitch_bend_max_cosine_
    deviation`, 0.065). This is the two-stage cascade's -12dB/octave
    rolloff doing exactly what it was designed to do (this file's own
    tremor-research math) -- a fast wiggle reverses direction before the
    cascade can catch up to the true instantaneous deflection, so its
    OWN amplitude gets cut down near the deadzone, not just "confidence"
    from `PITCH_BEND_ARM_MS`. Confirms, with real numbers this time,
    exactly what this file's own research already concluded: this can't
    be fixed by lightening the shared live signal (tried twice, both
    made things worse -- see current_cosine_x/y's own history) --
    a real fix needs an INDEPENDENT wiggle-energy detector (e.g.
    comparing a fast, lightly-filtered signal against x2/y2 to measure
    "how much faster is this moving than the stable path thinks")
    layered additively on top of the now-stable main path, not built yet
    -- flagged to the user as a separate, more exploratory follow-up
    given this file's two prior failed attempts at this exact feature.
- **Fast-wiggle vibrato, built as an independent detector this time.**
  Researched real precedent before implementing ("lets... do research
  and think first"): LinnStrument's own docs describe vibrato as
  nothing special -- "wobble a finger left and right" on the SAME
  pitch-bend path used for slides -- and Haken Continuum "converts
  finger tremble into vibrato" via direct high-resolution tracking;
  neither needs a separate mechanism because their sensors are precise
  enough that one live signal handles both slow tilts and fast wiggles.
  This hardware can't do that (its cascade has to be heavy enough to
  reject real cross-axis pressure coupling, confirmed all session), so
  a real fix needed a genuinely independent signal, not a lighter
  shared filter (both prior attempts at the latter made pressure
  stability worse).
  A dedicated three-gesture capture (hold still / deliberate tilt /
  fast wiggle, one pad, `[vib-cap]`) tested the gap between the two
  cascade stages ALREADY in the pipeline (`pitch_bend_smoothed_x/y`
  minus `_x2/_y2`) as the detector signal: mean 7.5 at rest, 8.5 during
  a held tilt (one single-tick blip to ~12, at RELEASE specifically),
  16.2 during a genuine wiggle, sustained above threshold for 93% of
  its duration. Onset (first 300ms of every strike, all three gestures)
  stayed under 7 -- an ordinary strike doesn't spike it.
  Implementation (`pitch_bend_apply_vibrato()`, called AFTER
  `pitch_bend_14bit_from_cosine_delta()` returns, never before): smooths
  that gap's magnitude into `pitch_bend_wiggle_energy`; a soft knee
  between the real captured floor (8) and sustained-wiggle level (16)
  maps it to a 0-1 depth; `VIBRATO_ARM_MS` (80ms) requires that to hold
  before ramping in at all, specifically because the one non-wiggle
  gesture that came close to the floor was a release transient -- a
  deliberate hard press MID-hold (a similar fast depth-ramp event) is
  untested and exactly the kind of thing this file's history says not
  to assume safe without a confirmation window. Once armed, a fixed
  ~5.5Hz sine (real feedback asked whether showing the motions would
  help distinguish them -- "yes go ahead" led to the data above, not a
  literal video), scaled by depth, is added directly to the FINAL wire
  value -- never routed through the deadzone/confidence logic the main
  bend depends on, so it cannot reintroduce either of the two previous
  regressions by construction. LFO phase is reduced via integer modulo
  on `now_ms` before ever touching a float, avoiding a real (if slow)
  precision drift a naive `sinf(2*pi*rate*now_ms/1000)` would accumulate
  over long continuous uptimes. `[vib-cap]` removed now that its capture
  has done its job.
- **Vibrato confirmed working; big tilt still jittery from pressure --
  fixed with an exponential response curve, not more filtering.** Real
  feedback: "wiggle works good. preassurte is afecting big tilt. big
  tilt should be an exponential curve that reaches the octave not a
  contant jittery." A near-max tilt sits right against the existing
  hard clamp to full scale -- under the previous LINEAR mapping, the
  ordinary residual x2/y2 noise this file has fought all session (much
  smaller since `pitch_bend_run_magnitude`'s freeze, never exactly
  zero) crosses that clamp boundary back and forth, reading as
  flickering between "near max" and "pinned at max" specifically at the
  top of the range, even though the same noise is imperceptible lower
  down.
  Rather than chase yet another noise source, reshaped the response
  curve itself: `pitch_bend_shape_response_curve()` maps the linear
  [0,1] tilt ratio through `y = (1 - e^(-k*x)) / (1 - e^(-k))`,
  `k = PITCH_BEND_RESPONSE_CURVE_K` (3.0, a first guess) -- NOT `y=x^k`,
  which grows steepest at the top, the opposite of what's needed here.
  This shape's slope is steepest near x=0 (more expressive resolution
  for an ordinary small tilt) and flattens continuously toward x=1,
  where the derivative is smallest -- the same real noise near max tilt
  now moves the output far less. Endpoint-preserving by construction
  (y(0)=0, y(1)=1 exactly), so "reaches the octave" still means exactly
  `TILES_MIDI_MPE_PITCH_BEND_RANGE_SEMITONES` at true full-scale tilt,
  not an asymptote that never quite arrives. Applied to the ratio AFTER
  `PITCH_BEND_ARM_MS`'s own temporal confidence multiplier (an
  orthogonal, time-based ramp) but before the sign is reapplied, so
  both bend directions get the identical shape.
- **The exponential curve above was REVERTED on the very next boot --
  real feedback: "you made the tilt still weird and also preaqssure
  change is affecting tilt."** Two real problems, not one:
  1. **Implementation bug.** The curve was applied to `ratio` AFTER
     `PITCH_BEND_ARM_MS`'s confidence multiplier was already folded in
     -- since the curve's slope is steepest near 0, this amplified the
     temporal ramp-in itself (a tilt only 10% into its 30ms
     confirmation window got curved to ~27% of full output, not 10%),
     distorting exactly the first 30ms of every gesture, which is also
     when residual noise is most present before a hold settles.
  2. **More fundamental.** Any curve with y(0)=0 and y(1)=1 has average
     slope exactly 1 over [0,1] (mean value theorem) -- reducing
     sensitivity near x=1 to fight top-of-range jitter mathematically
     REQUIRES increasing it somewhere else, which this curve's own
     steep near-zero slope did. That's not a bug, it's what this whole
     class of fix does by construction: it redistributes where noise is
     visible across the range rather than reducing the noise itself.
     Bug (1) made it worse than the tradeoff alone would have, but the
     tradeoff itself meant ordinary small-to-moderate tilts (where real
     playing spends most of its time) got MORE sensitive in exchange
     for a calmer arrival at an extreme that's rarely reached.
  Reverted to the plain linear mapping this file already had tuned
  and confirmed reasonably stable before this round. A future attempt
  at smoothing specifically the top of the range should restrict any
  reshaping to a narrow region near the clamp (identity below some knee
  point, eased only above it) rather than reshaping the whole [0,1]
  domain, so ordinary playing is never touched by it.
- **Diamond freed entirely; scale/pattern sub-menu moves to triangle+
  shift; diamond becomes an Ableton transport remote; mute-hold
  shortened.** Real feedback, four related changes in one round:
  1. **"lets put the scale menu into the mode menu when triangle plus
     shift pressed. freeing up diamond from everything for now."**
     `handle_diamond_click()` (melodic's scale picker / sequencer's
     pattern picker / per-step-edit cancel, depending on
     `s_active_mode`) is gone; that exact branching moved into
     `handle_triangle_click()`'s new shift path, firing only when
     circle ("shift" -- see `services/midi_clock.h`'s own naming
     precedent) was ALSO held during the press. A plain solo triangle
     click keeps its existing meaning (mode-select) unchanged.
     `s_triangle_press_was_shift` is edge-latched the same way
     `s_triangle_press_had_conflict` already is (circle and triangle
     won't always release in the same tick), and square joining too
     escalates to a full conflict instead of a shift -- three of
     `game_mode.h`'s reserved SW3+SW4+SW5+SW6 four buttons held
     together is clearly progressing toward that secret combo, not a
     genuine 2-button gesture. Both menu render functions' diamond-
     column LED moved to triangle's own `OP_TRIANGLE_LED_MENU_LEVEL`
     accordingly (the "which button got you here" indicator now points
     at the right button in both cases).
  2. **Diamond -> dedicated Ableton transport remote.** Real feedback:
     "the diamond for now will play and stop in ableton like a toggle
     and stop brings back to the start always. if we hold it for 2 sec
     it arms record and when we let go it counts down metronome into
     record play." Researched Ableton's actual MIDI behavior first
     rather than guessing: with a MIDI input's own "Sync"/"Ext" enabled
     in Ableton (Preferences -> Link/MIDI), incoming System Realtime
     Start/Stop messages fully drive its transport, and Start is
     spec-defined to always begin from position 0 -- never resumes like
     Continue would. New `tiles_midi_send_start()`/`tiles_midi_send_
     stop()` in `midi/midi_out.c` (a new `send1()` helper, single-byte
     System Realtime messages have no channel nibble at all) are the
     only two this file ever sends -- deliberately never joined by a
     Continue sender anywhere in this codebase, which is what makes
     "stop brings back to the start always" true for free, not
     something built by hand.
     A short diamond click toggles `s_transport_playing` between the
     two. Held >= `OP_TRANSPORT_RECORD_ARM_HOLD_MS` (2000ms, edge-
     latched so it only arms once per hold): on release, instead of the
     toggle, sends `OP_TRANSPORT_RECORD_CC` (CC 3, an Undefined generic
     controller number not used elsewhere in this file) once as a
     momentary trigger. Unlike Start/Stop, MIDI has no standard "begin
     recording" message -- confirmed via Ableton's own documented
     workflow (Key/MIDI Map Mode, map any CC/Note to the Record
     button), this needs a ONE-TIME manual mapping step in Ableton:
     Cmd/Ctrl+M, click Live's Record button, then do the hold-2s-and-
     release gesture on the hardware once. Live's own Count-In
     preference then handles "counts down metronome into record play"
     automatically once Record engages -- nothing about counting beats
     needed to be built in firmware at all. A CC rather than a Note-On
     specifically so a stray/unmapped receive can never sound an actual
     note the way a Note-On on the Zone Master Channel might on a
     receiver that isn't strictly MPE-aware.
     Diamond's LED is now a persistent `tiles_buttons_set_override_led()`
     indicator (same mechanism `services/octave_control.c` already uses
     for SW1/SW2) rather than anything menu-related: dim while stopped,
     solid while playing, fast-blinking once armed -- these buttons are
     monochrome PWM, so a blink pattern (a real-hardware "about to
     record" convention) stands in for the color distinction an RGB LED
     would give. Like every other override-LED button, it goes dark
     while any of this file's own full-grid menu views are open
     (`tiles_buttons_set_standby_led()` doesn't check per-button
     override state, only the global standby-active flag -- confirmed
     by reading `services/buttons.c` directly rather than assuming) and
     resumes correctly the instant one closes; accepted as consistent
     with existing behavior, not a new problem.
  3. **`EXPRESSION_MUTE_HOLD_MS` 3000 -> 2000.** Real feedback: "when we
     hold for haptic mute its too long so that combo hast to be reduced
     to 2 secodns."
- **Six fixes/changes in one round, following the first real hands-on
  test of the diamond/triangle rework above:**
  1. **Triangle LED stuck on after picking a scale.** Real feedback:
     "the light behabes weird for triangle, when scale is selected the
     light stays on." Root cause found by reading `services/buttons.c`
     directly (this exact bug, and its fix, already happened once before
     for the top-level menu -- see `menu_exit()`'s own comment): triangle
     has a PERMANENT LED override claimed, and `tiles_buttons_set_
     standby_led()` doesn't check per-button override state at all, only
     the global standby-active flag -- when `tiles_buttons_set_standby_
     active(false)` fires, `buttons.c`'s `refresh_all_button_leds()`
     deliberately skips override-active buttons ("that controller's own
     next scan repaints it correctly"), but nothing was doing that
     repaint for `scale_menu_exit()`/`pattern_menu_exit()` specifically,
     since triangle only just started owning those LED columns this
     round. Fixed by adding the identical `tiles_buttons_set_override_
     led(TILES_TRIANGLE_BUTTON_ID, 0.0f)` call `menu_exit()` already
     has, to both.
  2. **"why does a click of triangle send to melodic mode? in other
     modes? it should just bring menu up."** Removed the special case
     that force-jumped straight to melodic from any other active mode;
     a plain triangle click now always just opens/closes the mode
     picker, which already correctly pulses whichever mode is ACTUALLY
     active regardless of what it is -- there was never a real need for
     the shortcut.
  3. **"sequencer should not stop if mode is changed. it should be able
     to run in the background."** `seq_advance_clock()` (the actual
     step-advance/note-fire engine) moved outside the `s_active_mode ==
     OP_MODE_SEQUENCER` gate in `tiles_op_mode_scan()` -- it now runs
     every scan regardless of which TOP-LEVEL mode is currently
     displayed (still gated behind the same pre-existing pauses while
     the mode picker, scale menu, pattern menu, or a per-step edit is
     actually open -- unrelated to this fix, not something real feedback
     asked to change). `set_active_mode()` no longer calls `seq_end_
     current_note()` when leaving sequencer mode -- the currently-
     sounding note now keeps sounding and gets ended by the sequencer's
     own engine on its own schedule, not cut off by the display switch.
     `seq_start()` itself also had to change: it used to unconditionally
     reset the playhead to step 0 and arm a quantized restart every time
     sequencer mode was (re-)entered, which would have audibly restarted
     the pattern every time the player just glanced back at it while it
     was already correctly running in the background -- now skips that
     reset entirely when `tiles_midi_clock_is_running()` is already
     true, only refreshing view-level touch-tracking state.
     Known, accepted limitation worth watching for in practice, not yet
     hit: `services/expression.c`'s live-touch MPE channel allocator and
     the sequencer's own per-pattern channel claims are still two
     independent systems (see this file's own "Multi-pattern bank"
     section) -- this was already a theoretical edge case before, but a
     background sequencer running WHILE melodic notes are ALSO being
     played live is now the primary intended workflow this enables,
     not a rare corner case, so a real channel collision is more likely
     to actually surface than it used to be. No capture of one yet;
     worth a dedicated fix if real playing finds one.
  4. **Diamond transport not doing anything in Ableton.** Verified the
     firmware side directly rather than assuming: read TinyUSB's own
     `tud_midi_n_stream_write()` (`lib/tinyusb/src/class/midi/midi_
     device.c`) and traced a single 0xFA/0xFC byte through its packet-
     framing state machine by hand -- it correctly forms a complete
     `MIDI_CIN_SYSEX_END_1BYTE` USB-MIDI packet immediately, exactly the
     right encoding for a lone System Realtime byte. The firmware side
     was not the bug. Likely cause: Ableton needs TWO separate things
     enabled, not one -- (a) Preferences -> Link/MIDI -> tick "Sync" for
     this device's input port, AND (b) click the "Ext" button in
     Live's OWN transport toolbar to actually engage external sync (the
     Preferences checkbox alone does nothing until Ext is also on).
     Easy to do just one and assume it's enough. Restated clearly for
     the next test rather than left implicit.
  5. **LED behavior, fully specified.** Real feedback: "armed record and
     stopped is blik twice and pause then again, play is on, stopped is
     off. record is pulsing in the same fashon as the deep sleep for
     shift button." Four states, checked most-specific-first: **armed**
     (a 2-second hold in progress) is two quick flashes then a pause,
     repeating (860ms cycle: 120/120/120 on-gap-on then 500ms pause) --
     a real-hardware "about to record" convention; **recording** (armed,
     then released) is the exact same sine-breathing shape `services/
     standby.c`'s `render_deep_sleep_frame()` uses for circle's deep-
     sleep pulse (same period/min/max, duplicated since standby.c's own
     constants are file-local) -- real feedback pointed at that specific
     animation as the reference, not a new one invented here; **playing**
     is solid on; **stopped** is fully off (changed from a dim idle glow
     this round, per explicit "stopped is off"). A plain click now always
     means "stop everything" regardless of whether it was playing or
     recording, matching a real transport's single Stop control, rather
     than leaving recording somehow still armed underneath.
  6. **"when we do shift plus modifiers -+ for changiong length of
     sequencer mode we should have a flash indicating which length we
     made the sequence to make that a color sentia magenta."** The
     circle+minus/plus length-adjust gesture now sets a timestamp;
     `render_sequencer()` checks it every frame and, for
     `OP_SEQ_LENGTH_FLASH_DURATION_MS` (400ms) after a change, replaces
     the ENTIRE grid's normal step coloring with a direct read of the
     new length -- pads `0..length-1` in Sentia magenta (`OP_MENU_
     MELODIC_R/G/B`, this file's own established brand-color constants,
     not a new color guessed here), everything else off -- so the new
     value reads clearly instead of blending into whatever armed/cursor
     state those same pads already had.
- **Scale picker made universal; sequencer's own pattern/channel picker
  removed.** Real feedback: "make sure the shift scasle works on chord
  melodic mode and on sequewndcer as well measning remove whatever aux
  menu we had in sequencer mode." `handle_triangle_click()`'s shift
  branch no longer checks `s_active_mode` at all -- it always toggles
  the scale picker (or cancels an active per-step edit first, still the
  one thing that needs its own escape hatch). Chord mode's own melody
  columns and sequencer's own note mapping both already read
  `note_map.c`'s global scale the identical way melodic's idle grid
  does, so there was never really a reason for melodic to be the only
  mode that could open it.
  Sequencer's own DIFFERENT sub-menu (the "4 patterns, one per row"
  picker from the earlier multi-pattern rework) is gone outright, not
  replaced: `render_pattern_menu()`, `handle_pattern_menu_taps()`,
  `pattern_menu_enter()`/`_exit()`, `pattern_row_color()`, and their
  `OP_PATTERN_1..4_R/G/B` colors are all deleted. The underlying
  multi-pattern DATA MODEL is deliberately left in place (`s_seq_pattern
  [OP_SEQ_NUM_PATTERNS]`, `active_pattern()`, each pattern's own MIDI
  channel/length/armed-steps/pitch-overrides) -- reverting that too
  would have been a much larger, riskier change than what was actually
  asked for, and it costs nothing to leave it dormant. `s_seq_active_
  pattern` just has no UI left that can ever move it off 0 now. One
  real casualty: a plain circle-click while the picker was open used to
  toggle a pattern's `probability_enabled` master switch -- that
  gesture's home is gone along with the picker, so the setting is
  currently unreachable (stays at its default) until/unless a future
  round gives either pattern-switching or this specific toggle a new
  access point.
- **Diamond transport still not doing anything -- researched real
  hardware instead of re-guessing, changed the wire approach.** Real
  feedback: "diamond is still not doing anything why is it not sending
  transport controls to daw. look online foir hoiw other things do that
  liike the novation lounch key." The previous round's Play/Stop
  (System Realtime Start/Stop bytes, betting on Ableton's Sync/Ext
  external-sync mechanism) had a real gap: external sync fundamentally
  means slaving to a continuous MIDI Clock stream too, and this device
  has only ever RECEIVED clock, never sent it -- isolated Start/Stop
  bytes with no clock behind them landing on a port never fully in that
  state explains "not doing anything" better than a missed checkbox.
  Checked how a real Launchkey does it instead of assuming: its
  transport buttons "send MIDI Control Change events on Channel 16" --
  plain, mappable CCs, not Realtime bytes. Confirmed (again) that
  Ableton's own Play/Stop/Record ARE each individually MIDI-mappable via
  generic Map Mode, and that same generic "MIDI learn" concept exists in
  effectively every other DAW too (Cubase's MIDI Remote, Reaper's Action
  List binding) -- unlike Sync/Ext, needs no clock output and no
  per-DAW-specific feature.
  Play and Stop now each get their own momentary CC trigger
  (`OP_TRANSPORT_PLAY_CC`/`_STOP_CC`, 102/103), the same shape
  `OP_TRANSPORT_RECORD_CC` already used -- moved from 3 to 104 for
  consistency, all three now drawn from the MIDI spec's own "Undefined"
  generic-controller range (102-119), the same CONVENTIONAL block real
  transport hardware draws from (not a claim of matching a Launchkey's
  exact numbers, which weren't confirmed to that precision -- just the
  same real-world convention). The Realtime Start/Stop sends stay too,
  harmless and still a real win for the rarer setup that does have
  Sync/Ext genuinely engaged, but the CC triggers are now the primary,
  research-backed path. Needs the same one-time MIDI-Map step in
  whatever DAW is actually in use for all THREE triggers, not just
  Record -- mapping only one and assuming the others inherit it is the
  most likely way this still reads as "not doing anything" again.
- **Stuck-note audit: one real gap found and closed.** Real feedback: "is
  there anything needed to stop stuck niotes?" `services/expression.c`'s
  live-touch MPE channel allocator (`claim_mpe_channel()`) and
  `op_mode.c`'s own per-pattern sequencer channel assignment
  (`active_pattern()->channel`) are two independent systems that never
  knew about each other. Now that the sequencer runs in the background
  regardless of which mode is displayed (see "sequencer should not stop
  if mode is changed" above), a live touch could claim the exact MPE
  channel the sequencer was using for its own background note -- the
  sequencer's next `seq_fire_note()` would then end/steal whatever the
  live touch put there without `expression.c` ever knowing, leaving that
  pad's own state machine believing it still owns a note that's already
  gone, never able to send its own eventual note-off. New accessor
  `tiles_op_mode_sequencer_reserved_channel()` returns the sequencer's
  active channel while genuinely running (0, never a valid channel,
  otherwise); `claim_mpe_channel()` skips it in both the free-slot search
  and the steal-oldest fallback. Every other stuck-note path was already
  covered by existing code (grid-ownership-change and standby both
  already force every sounding pad off via `seq_end_current_note()`/
  `chord_end_all_notes()`/expression.c's own release handling) -- this
  channel-collision gap was the one real hole.
- **Sequencer capture mode (SW4 diamond + shift).** Real feedback: "make
  a sequencer capture mode when sifht and diamoind clicked together. this
  means the sequencer turns into the regular chromatic scale and captures
  the lplayed melody into sequecer in the current tempo quantized but
  also do allow overlap. this makes the diamond flash glow and then exit
  into sequencer is by shift or by diamond, not directly to the menu. the
  curent step of the sequencer should light up pink sentia when the
  sequencer is at that step." Entered with the same shift+diamond combo
  language `handle_triangle_click()`'s own shift detection already
  established (circle held + diamond, square NOT also held or it
  escalates to a conflict instead); forces sequencer mode active first if
  it wasn't already, switches `note_map.c`'s scale to chromatic for the
  duration (restored on exit) so every pad plays its natural note with no
  scale filtering, and arms a quantized-start just like a fresh Start
  would. While active, the grid is owned entirely by
  `seq_capture_handle_taps()`/`seq_capture_advance_clock()`/
  `render_seq_capture()` -- a dedicated dispatch branch, not routed
  through the normal playback engine at all, since capture's job
  (accumulate live touches, commit on the beat) is unrelated to
  probability/ratchet/pitch-override playback logic.
  "Allow overlap": a fresh touch always wins over whatever was already
  sounding -- ends the old note, starts the new one, the same "hold the
  trig, play the note" simplicity this file's per-step pitch-assignment
  view already uses -- so overlapping touches hand off cleanly instead of
  rejecting or glitching, both for the audible note and for which note
  gets written into the step currently being recorded.
  Quantization: each step has an accumulator (`s_seq_capture_step_note`/
  `_armed`) that a touch during that step's window writes into; only at
  the NEXT step boundary does `seq_capture_advance_clock()` commit the
  accumulator into `active_pattern()`'s real `step_armed`/`step_note`/
  `step_pitch_override` arrays and reset it. A touch's real timing only
  ever decides WHICH step's window it landed in, never a sub-step offset
  -- this is what makes capture genuinely quantized rather than free-time
  recording with a grid overlay.
  Exit is by a solo release of either shift or diamond alone (checked
  ahead of their normal meanings in both `handle_circle_tap()`'s and
  `handle_diamond_transport()`'s own release branches), matching "exit
  into sequencer is by shift or by diamond, not directly to the menu"
  precisely -- capture mode hands control back to the normal sequencer
  step view, never straight to a menu.
  Diamond's LED gets a fifth, highest-priority state while capture is
  active: `menu_selected_pulse_level()`, this file's own established
  "selected" pulse (the scale picker's own pulse speed), deliberately
  faster than recording's slow breathing and not a hard blink like armed
  -- satisfies "this makes the diamond flash glow" while staying visually
  distinct from transport's other four states.
  Current-step color: Sentia magenta/pink (`OP_MENU_MELODIC_R/G/B`, this
  file's own established brand-color constants, pulsed at the same
  `menu_selected_pulse_level()`) exactly matches "the curent step of the
  sequencer should light up pink sentia when the sequencer is at that
  step" -- overridden to solid white only for the pad currently actually
  sounding, so a held note is always visually unambiguous even when it
  lands on the current step.
  One proactively-caught bug while wiring the entry combo:
  `handle_circle_tap()`'s existing mid-hold cancellation only watched
  minus/plus/pad-touch to decide whether a circle-hold had become some
  other combo instead of a tap-tempo tap -- diamond joining the hold
  (this exact gesture) wasn't in that check, so "circle first, diamond
  joins" could have spuriously registered a tap-tempo tap on release
  despite genuinely being the capture-mode entry combo. Fixed by adding
  diamond to that same check, mirroring the identical fix the
  circle+minus/plus length-adjust combo already needed for the same
  "circle pressed first" ordering.
- **Pattern bank restored (SW3 triangle + shift, sequencer mode only).**
  Real feedback: "in sequencer mode shift plus triangle opens up the
  pattern bajnk. make all patterns white except for the selecteed onel
  tjhat ones is red flashing." `handle_triangle_click()`'s shift branch
  goes back to per-mode routing for this one mode only -- sequencer gets
  this pattern bank, every other mode still gets the universal scale
  picker (see "Scale picker made universal" above, which this doesn't
  reverse anywhere except this single gesture in this single mode). Not
  a straight revert of the old, fully-deleted pattern/channel picker:
  different visual language on purpose -- plain white
  (`OP_SCALE_AVAILABLE_LEVEL`, this file's existing "available option"
  brightness) for every non-selected pattern instead of 4 distinct row
  colors, and a hard on/off RED FLASH (300ms, deliberately a blink, not
  this file's usual smooth "selected" pulse, so it reads as visually
  distinct and matches the word real feedback actually used) for the
  active one instead of a pulsing white. Same touch-click-then-push-
  past-50%-depth selection gesture every other picker in this file uses;
  opening the bank silences whatever's currently sounding first
  (`seq_end_current_note()`), same as every other sub-view here; switching
  patterns clears any in-flight ratchet count too, the same cross-pattern
  mix-up guard the original picker already established.
- **Three sequencer bugs closed.** Real feedback: "ok we have clashing
  issues on modes, mode selectro shouldnt pause sequencer... also
  selectring a new sequence takes that initial press as a note on for
  that step, lets fix that... changing scale on a melodic modes or other
  sequences should not affect other sequences that are already set up or
  playing meaning fully scale independent sequences and no stopping on
  playing sequences regarless of manu displayed or scale changed
  somewoehre else."
  1. **Menus were pausing playback.** `tiles_op_mode_scan()`'s own
     `seq_advance_clock()` call used to sit at the BOTTOM of the
     function, past four separate `return`s (top-level menu, scale menu,
     pattern bank, per-step edit) -- every one of those silently froze
     the playhead for as long as it stayed open. Moved the call to the
     TOP of the function, unconditional (now looped per lane -- see
     below), closing the gap for all four sub-views at once. Capture
     mode stays the one genuine exception (it replaces the advance
     entirely with its own `seq_capture_advance_clock()` for the one
     lane it owns).
  2. **Selecting a pattern bled its touch into the next view as a note
     toggle.** The finger that just tapped a bank cell to select it is
     often still down the instant control returns to the normal step
     view; `pattern_bank_exit()` never re-synced `seq_handle_step_taps()`'s
     own touch-tracking arrays the way `edit_exit()` already did for the
     identical reason, so that same pad read as a fresh touch-down and
     toggled whatever step it landed on. Fixed by adding the exact same
     resync loop `edit_exit()` and `seq_start()` already establish.
  3. **Armed steps re-read the LIVE global scale every time they played.**
     A plain tap-to-arm left `step_pitch_override` false, so
     `seq_fire_note()` re-resolved that step's pitch from
     `tiles_note_map_get_note()` -- the current global scale/octave/key
     -- on every single pass, meaning changing the scale ANYWHERE later
     (melodic mode, a different lane's own pick) silently retuned every
     already-programmed step everywhere, including patterns actively
     playing in the background. Fixed by freezing the resolved note into
     `step_note`/`step_pitch_override` the instant a step is armed
     (`seq_handle_step_taps()`'s own release branch) -- the same
     always-absolute approach sequencer capture mode already used per
     step. Re-arming a step later re-freezes it fresh at THAT moment's
     scale (a deliberate new edit, not passive drift); octave shift and
     key transpose freeze the same way, alongside scale, since all three
     bake into the one absolute MIDI note number this returns.
- **Sequencer rearchitected: 4 independent, simultaneous lanes, 24
  selectable patterns.** Real feedback: "sequence selector should have
  all 24 pads as possible sequences... lets do 4 independent sequences
  that can be assigned to 4 channels selectable by each row of 6
  alternatives. by defoult they just do different midi channels...  make
  each row a different color in seelctor to signify 4 lanes." Replaces
  the previous round's flat 4-pattern bank (one pattern per row, only
  ever one "active" pattern playing at a time) with a genuine 4 LANE x 6
  ALTERNATIVE grid -- every one of the 24 pads is now a real,
  individually selectable slot.
  A **lane** is a fully independent playhead with its own fixed MIDI
  channel (`s_seq_lane_channel[OP_SEQ_NUM_LANES]`, defaulting to nibbles
  15/14/13/12 -- lane 0 keeps today's original single-pattern channel
  unchanged) -- all 4 now run SIMULTANEOUSLY once the clock is running,
  never just one at a time. Every scalar that used to describe "the
  sequencer's" playback state (`s_seq_current_step`,
  `s_seq_step_started_at_pulse`, `s_seq_pending_start`/`_restart`, the
  ratchet-scheduling fields, the sounding-note bookkeeping) became a
  `[OP_SEQ_NUM_LANES]` array, and every playback function that touched
  them (`seq_advance_clock()`, `seq_enter_step()`, `seq_fire_note()`,
  `seq_reset()`, `seq_resume_current_step()`, `seq_end_current_note()`)
  gained an explicit `lane` parameter -- `tiles_op_mode_scan()` now loops
  all 4 once per scan with the SAME clock snapshot (all lanes share one
  global tempo/transport; only their CONTENT is independent).
  Each lane's own row of 6 **alternatives** is a full, independent
  pattern (armed steps, pitch overrides, length, probability) -- picking
  a different column for a row swaps what that ONE lane plays without
  touching the other 3. `active_pattern()` deliberately keeps its old,
  narrower meaning throughout this change: "whichever pattern the player
  is currently LOOKING AT" (`s_seq_edit_lane` + that lane's own active
  alternative) -- every EDIT-side function (per-step pitch/probability/
  ratchet, length-adjust, capture mode, the bank's own rendering) still
  calls it completely unchanged. Only the PLAYBACK engine needed the new
  lane-parameterized `pattern_for_lane()` sibling -- this is what kept
  the refactor's blast radius to the playback functions and the bank
  itself, rather than touching every edit-side call site too.
  Tapping a bank cell does two things at once: sets that ROW's (lane's)
  active alternative to that COLUMN, and makes that ROW the one shown in
  the main step view (`s_seq_edit_lane`) -- one gesture unambiguously
  specifies both "which lane" and "which alternative," so no separate
  lane-select gesture was needed. Swapping a lane's active alternative
  reuses the SAME quantized-start mechanism (`s_seq_pending_start`) a
  fresh Start already uses, waiting for the next beat boundary rather
  than jumping into the new pattern's content at a possibly-mid-beat
  offset or an out-of-bounds step index if the two alternatives have
  different lengths.
  Colors: each row gets its own fixed identity hue at this file's usual
  `OP_SCALE_AVAILABLE_LEVEL` "available" brightness (amber/green/blue/
  purple for lanes 0-3, deliberately avoiding both red -- reserved
  everywhere in this bank for "selected" -- and Sentia's own brand
  magenta, reserved elsewhere for capture mode's playhead and the
  length-change flash); the selected cell in each row keeps the hard
  on/off red flash the previous round already established.
  MPE stuck-note reservation updated to match: since up to 4 channels
  can now be simultaneously reserved instead of just 1, `tiles_op_mode_
  sequencer_reserved_channel()` (a single return value) became `tiles_
  op_mode_sequencer_channel_is_reserved(channel)` (a query), and
  `services/expression.c`'s `claim_mpe_channel()` now asks it per
  candidate channel instead of comparing against one stored value.
  "write down that the control software can send the sequences to
  differetn out ports loke cv gate, or midi" -- noted in
  `companion-app/README.md`'s planned feature list; by default every
  lane still sends out the same USB-MIDI endpoint on its own channel,
  same as everything else in this firmware -- per-lane physical output
  routing (CV/gate vs. MIDI) is a companion-app-side feature, not built
  here.
  Known, accepted tradeoff: haptic step-pulses are a PHYSICAL pad
  resource, but up to 4 lanes can each independently reach "step N" (pad
  N+1) at the same instant now -- a stop from one lane can occasionally
  cut a kick another lane (or capture mode, or a live touch) just started
  on that same physical actuator. Rare, momentary, cosmetic only (the
  actual MIDI note is always fully separated by channel) -- not chased
  here, the same tradeoff the single-background-lane version already
  accepted.
- **Pattern bank moved to shift+diamond; shift+triangle became a
  per-pattern scale picker.** Real feedback: "i want shift plus diamond
  in sequencer only to be the pattern selector. shift plus triangle in
  [sequencer mode] scale selector for that specific pattern." Frees
  shift+triangle to become the scale picker UNCONDITIONALLY across every
  mode (no more per-mode branch in `handle_triangle_click()`), while
  shift+diamond in sequencer mode now opens the pattern bank instead.
  Diamond's own shift-combo used to mean sequencer capture mode
  unconditionally on release -- now split by HOLD DURATION, the same
  "short click vs. long hold" shape diamond's own plain (non-shift)
  record-arm already uses one level up: a quick shift+diamond click
  toggles the pattern bank; holding it past `OP_SEQ_SHIFT_DIAMOND_
  CAPTURE_HOLD_MS` (400ms -- shorter than record-arm's 2000ms, since
  entering capture mode doesn't itself start recording anything until a
  pad is actually played) instead arms capture mode, unchanged from
  before other than which release condition now reaches it. Capture
  mode's own EXIT gesture (solo shift or solo diamond release) is
  untouched.
  Per-pattern scale: each of the 24 patterns now carries its own
  `scale` field, read/written by the exact same picker view/tap-handling
  every other mode's shift+triangle already uses, completely unchanged
  -- `scale_menu_enter()` just temporarily points note_map.c's GLOBAL
  scale slot at the pattern's own stored value for as long as the picker
  stays open (the same swap-in/swap-out shape sequencer capture mode
  already uses for its own scale override), then writes whatever got
  picked back into the pattern and restores the real global scale on
  exit -- melodic mode's own live scale is never visibly disturbed.
  Only ever consulted at ARM time (see the "three sequencer bugs closed"
  entry above), so changing a pattern's scale later never retunes steps
  already armed under the old value, the same "fully scale independent"
  guarantee already established there.
- **Pattern bank colors standardized for 4 simultaneous lanes.** Real
  feedback: "lets standardize colors in sequencer. playing selected
  pattern is red, off patterns are not led enabeled. only when sequence
  is entered but not playing the pattern has the correct color
  prevousely defined. the playing but not selected pattern flashes
  white. remeber 4 patterns can play at once." `render_pattern_bank()`
  now reads as two entirely different displays depending on whether the
  transport is actually running -- "playing" vs. "not playing" per lane
  is meaningless while nothing is running on ANY lane at all:
  - **Transport stopped** ("the correct color previously defined"):
    unchanged from the previous round -- each lane's own identity color,
    dim, for its available alternatives; hard red flash for each lane's
    own current pick. Still a browsing/picking view, not a playback
    readout.
  - **Transport running**: becomes a pure "what's sounding right now,
    across all 4 simultaneous lanes" readout instead. Solid red for the
    ONE cell that is BOTH this lane's own playing alternative AND the
    lane currently shown in the main step view (`s_seq_edit_lane`) --
    "you are here, and it's playing." Flashing white for every OTHER
    lane's own playing alternative -- "also playing, but not what you're
    looking at" (the real reason this needs its own signal: up to 4 of
    these can be lit at once, on rows you aren't currently viewing).
    Every non-playing alternative goes fully dark -- lane identity color
    is a browsing aid, not meaningful once the point is "what's actually
    sounding."
- **Audit: sequencer sub-view mutual exclusion.** Real feedback: "lets
  do a check of everything weve changed over the past uploads," asked
  right after the pattern-bank/capture-mode button remap above. Focused
  the review on that remap and the 4-lane rearchitecture from the
  previous two rounds -- the newest, least hardware-tested code, and
  where real bugs were most likely to be hiding versus the earlier
  pitch-bend/expression work already validated on real hardware. Found
  and fixed one real class of bug, in four places: sequencer has five
  mutually-exclusive sub-views (top-level menu, scale picker, pattern
  bank, per-step edit, capture mode), but `handle_triangle_click()`/
  `handle_diamond_transport()` run their OWN button state machines every
  scan regardless of which sub-view is currently displayed -- nothing
  stopped a SECOND sub-view's entry gesture from firing while a FIRST
  one was still sitting open (e.g. open the pattern bank with a short
  shift+diamond tap, release, then hold shift+diamond again long enough
  to arm capture mode without ever closing the bank first). Since
  `tiles_op_mode_scan()`'s dispatch checks sub-views in a fixed priority
  order, the SCREEN would keep showing whichever one was checked first,
  while the other quietly became "active" underneath -- capture mode
  specifically would start swapping note_map.c's global scale to
  chromatic and taking over `s_seq_edit_lane`'s background playback
  while touches kept routing to the wrong view entirely. Worse for the
  scale-picker case specifically: capture mode's own scale swap would
  land on top of the per-pattern picker's ALREADY-in-progress swap,
  risking a pattern's real saved scale getting silently overwritten with
  chromatic.
  Fixed by having `seq_capture_mode_enter()` defensively close the
  pattern bank, the (real, swap-aware) scale menu, and any open per-step
  edit first -- and `pattern_bank_enter()` do the same for an open
  per-step edit -- before doing anything else, the same "close whatever
  else might be open" discipline `set_active_mode()` already applies
  when leaving a mode entirely, just applied one level down between
  sequencer's own sub-views. `handle_triangle_click()`'s shift branch
  also gained its own escape hatch out of capture mode (a fresh
  shift+triangle tap now exits it, same role that gesture already plays
  for per-step edit), rather than silently corrupting the scale swap by
  opening the picker on top of it.
  One more, smaller gap from the same review: `tiles_op_mode_has_menu_
  open()` (used by `services/standby.h` to hold off its idle timeout for
  a sub-view with no touch input) never included capture mode, even
  though it can sit genuinely armed with no touch at all while waiting
  for a tempo or the next beat -- added.
- **Corrected: capture mode moved to plain diamond; DAW transport remote
  disabled in sequencer mode; play/stop made fully per-lane.** Real
  feedback: "no, capture mode is triggered by diamond in sequencer mode.
  transport controls disable on sequencer mode... play and stop are
  independent per active pattern. the only thing global is tap tempo or
  midi tempo." Corrects the previous round's design in three connected
  ways:
  1. **Diamond's DAW-transport role (CC sends, record-arm hold, the
     5-state LED language) is now entirely suspended while sequencer
     mode is active.** In sequencer mode, plain diamond is a simple
     click-toggle for capture mode (no hold needed at all -- shift alone
     already tells it apart from the pattern bank); outside sequencer
     mode, diamond is back to being purely the DAW remote, unchanged.
     `s_diamond_shift_capture_armed`/`OP_SEQ_SHIFT_DIAMOND_CAPTURE_HOLD_MS`
     from the previous round's hold-gating are gone -- no longer needed
     once plain vs. shift alone cleanly separates the two meanings.
     `handle_circle_tap()`'s old "exit capture mode on solo circle
     release" override is also gone -- it only made sense back when
     shift+diamond WAS the entry combo; circle/shift has no role in
     capture mode's lifecycle at all anymore.
  2. **Play/stop became fully per-lane.** New `s_seq_lane_running
     [OP_SEQ_NUM_LANES]` -- "-"/"+" now start/stop only `s_seq_edit_lane`
     (the lane currently shown), not all 4 at once. `seq_advance_clock()`
     gates on this FIRST, ahead of even `clock.start_edge` -- a stopped
     lane must never fire its step-0 note just because some other lane
     (or a fresh external Start) happens to land while it's sitting
     stopped. `midi_clock.h`'s own shared `running` flag is untouched in
     contract -- still the ONE thing genuinely global ("tap tempo or
     midi tempo") -- and is now DERIVED from this: `set_running(true)`
     fires the moment any lane goes from stopped to running,
     `set_running(false)` only once every lane has stopped
     (`any_lane_running()`). `tiles_op_mode_sequencer_channel_is_
     reserved()` and `tiles_op_mode_is_sequencer_active()` both switched
     from the shared clock flag to this per-lane state too, for the same
     reason: a stopped lane can't have a note sounding and doesn't need
     its channel reserved; an external clock ticking with every lane
     still stopped isn't actually "a pattern playing in the background."
  3. **Pattern-bank colors corrected again**, real feedback after
     actually testing the previous round's two-state (running/stopped)
     design on hardware: "shift diamond does pattern opicker but only
     full or enabeled patterns are on, rn i see all of them on... if a
     pattern is empty there is no light but lights will appear if
     pattern is filled or modified... patterns with notes are led on
     respectively and emptu ones are off." Replaced with ONE unified
     per-cell rule, checked most-specific-first: the cell that's both
     this lane's own pick AND the lane you're viewing -- flashing red,
     always, empty or not (you need to see your own cursor even on a
     slot you're about to record into); any OTHER lane's own pick while
     THAT lane is genuinely running (`s_seq_lane_running[lane]`) --
     flashing white; any cell with real content (`pattern_has_content()`,
     new) -- that lane's own dim identity color; otherwise -- off. No
     more running/stopped branching in `render_pattern_bank()` at all.
  One more real bug caught auditing all of this together: switching a
  lane's alternative via the bank never reset `s_seq_current_step` for
  that lane -- if the lane was currently stopped, `seq_advance_clock()`
  never got a chance to normalize it (it now returns immediately while
  `!s_seq_lane_running[lane]`, before reaching the pending-start logic
  that used to do this implicitly), so a later "+" could have resumed
  the NEWLY picked pattern from whatever step index the OLD one was left
  at. Now reset explicitly at switch time. Also: capture mode now
  requires a tempo to exist before it can be entered at all (mirroring
  "+"'s own identical gate) -- without this, `seq_capture_advance_clock()`
  would sit permanently inert with no tempo, so live touches would
  audibly sound but never actually commit into the pattern, with no
  indication anything was wrong; and capture mode now marks its lane
  `s_seq_lane_running` on entry, so the freshly recorded pattern keeps
  looping once you exit instead of silently freezing (capture's own
  advance function never consulted that flag, so exiting used to hand
  back to the normal engine with the lane still marked stopped).
- **Chord mode: pressure-tiered voicing, static and predictable, real
  jazz theory behind the tensions.** Real feedback: "for chord plus
  melodic mode the tap of capacitive touch does regular chord but with a
  root bass note and an oppen voicing for the chord then when pad is
  pressed past 50% make the chord more spicy depending on velocity...
  and full press does a more jazz complex voicing and more tensions
  replacing notes," and separately: "we are having issues with the
  chords drifting positions in certain sequences of presses so remove
  the voice leading thing. we just keep same positions and inversion
  static not adaptive, find best voicings firast tho." A later session
  confirmed neither had actually landed yet: "the preassure dependant
  chord type is not working and we still have this situation when the
  chord shapes evolve in a way that transports the chords to different
  parts of the range. we need consistent predictable shapes."
  **Voice-leading removed outright, not tuned.** The old design
  (`s_chord_voice_anchor_notes`/`_valid`, `tiles_note_map_nearest_
  pitch_class()`) re-voiced each new chord toward wherever the previous
  one sounded, one note at a time -- exactly what caused the drift. Every
  voicing is now a pure function of (root pad, chord quality, press
  depth) with zero history: `tiles_note_map_get_chord_notes()` always
  returns the identical 7-note diatonic stack for a given pad regardless
  of anything played before it.
  **The stack extended from 3 notes (root/3rd/5th) to 7**
  (root/3rd/5th/7th/9th/11th/13th, `TILES_NOTE_MAP_CHORD_NUM_NOTES`
  3->7), reusing the exact same skip-two-scale-degrees-per-tone
  harmonization the triad already used, just carried further -- this is
  "for free" from `note_for_scale_degree_using()`'s own existing octave-
  doubling, so it automatically stays correct for whichever diatonic
  mode is selected with no new math. Register spreading (which tier uses
  which voices, the bass note, the "open" spread) stays entirely
  `op_mode.c`'s concern, not `note_map.c`'s -- the note-mapping file
  hands back raw chord tones, nothing about performance articulation.
  **Two tiers, chosen live off Hall depth while held** (`chord_tier_
  for_depth()`, one threshold at `OP_MENU_SELECT_DEPTH_THRESHOLD` --
  simplified from an original three-tier pass before it saw much real
  playing time: "lets simplify to basic tirads and anything past 50%
  press jazz chord"), morphing both directions within the same held note
  -- pressing past halfway escalates, easing back off reverts,
  re-striking only on an actual tier change:
  - **Tap**: bass (root, TWO octaves below the melody register --
    `OP_CHORD_BASS_EXTRA_OCTAVE_SEMITONES` on top of note_map.c's own
    existing one-octave chord drop) + an OPEN triad (root and fifth at
    the chord register, third raised a further octave on top) -- "a root
    bass note and an open voicing," "basic tirads."
  - **Past 50% depth**: the bass stays, but the upper structure goes
    ROOTLESS -- third/7th/9th plus a top tension, root and fifth dropped
    outright, the same "bass states the root, the chordal voice goes
    rootless" shape real jazz piano/guitar voicings use. Diminished-
    quality degrees cap short of this instead (root/fifth/third/seventh,
    no rootless swap, no 9th/11th/13th) -- a real, safe diminished/
    half-diminished 7th needs no chromatic alteration, but a
    diminished chord's own NATURAL upper tensions do, which this
    diatonic-only system doesn't attempt; adding them untreated would
    reintroduce exactly the clash this design is trying to avoid.
  **Chord quality read directly off the actual returned intervals**
  (root-to-third, root-to-fifth), not hardcoded per scale degree, so it
  tracks whatever diatonic mode is active automatically: major/minor
  determines the jazz tier's top tension (13th for major-quality, 11th
  for minor) specifically to dodge jazz harmony's textbook "avoid note"
  -- a natural 11th sits a minor 9th above a MAJOR 3rd once octave-
  reduced, a genuinely harsh clash, so it's only ever added where the
  3rd is minor and that clash can't occur.
  Verified by hand-tracing the actual interval math for major/minor/
  diminished scale degrees before flashing (not just by eye) -- see the
  session's own worked examples for the specific semitone arithmetic
  confirming no unintended half-step/minor-9th clashes land in either
  tier's final voicing.
- **Full device freeze during real Ableton MIDI clock playback -- root
  cause: `printf()` flooding in `services/haptics.c`'s per-note hot
  path.** Real feedback: "something made it freeze and crash in
  sequwencer mode with ableton midi clock," confirmed as a hard lockup
  (LEDs frozen, no response to touch/buttons, needs a full unplug/
  replug) happening during ordinary, un-interacted-with playback -- "it
  just froze again with no interactrio, just on regular ableton nplay
  and pattern running."
  This is the EXACT same bug class this file's own pitch-bend history
  already found and fixed once (see that section's own "The freeze"
  entry): the Pico SDK's USB-CDC stdio driver busy-waits internally for
  up to `PICO_STDIO_USB_STDOUT_TIMEOUT_US` (500ms) every time its output
  buffer fills faster than the host drains it, and does NOT return to
  the main loop while waiting -- confirmed there by reading `pico-sdk/
  src/rp2_common/pico_stdio_usb/stdio_usb.c` directly, and unchanged
  since. `services/haptics.c` had never been through that same cleanup:
  `tiles_haptics_trigger_kick()` (every kick), `tiles_haptics_trigger_
  touch_pulse()` (every touch pulse, including its two "skipped"
  branches), `steal_oldest_voice()`, `start_kick_now()`, and `tiles_
  haptics_resync_hardware()` all had unconditional, per-call `printf()`s
  -- all originally "temporary bring-up visibility" from earlier
  haptics-debugging rounds, per their own comments, never removed once
  their diagnostic job was done. The 4-lane sequencer rearchitecture
  (this file's own recent rounds) is what finally exposed it: up to 4
  lanes firing notes off a REAL external clock -- denser and less bursty
  than tap-tempo, but sustained and continuous in a way single-lane
  testing never was -- means `tiles_haptics_trigger_kick()` now fires
  far more often, per note, per lane, with nobody's serial terminal open
  to drain the output (the ordinary case when just using the device with
  a DAW, not actively developing firmware) -- exactly the condition that
  makes the stdio driver's internal wait actually block for real.
  Fixed the same way as the pitch-bend round: deleted every one of
  these prints outright rather than throttling them, once each had
  served its original diagnostic purpose. The LOGIC comments explaining
  WHY the surrounding code behaves the way it does (the voice-stealing
  policy, the FAULT-mode power-flicker theory, etc.) were kept --
  only the print statements themselves, and the prose that existed
  solely to justify keeping THEM around, came out.
  **Sequencer capture mode also disabled for now** (`OP_SEQ_CAPTURE_
  MODE_ENABLED` gates the entry gesture in `handle_diamond_transport()`,
  everything else untouched) -- explicit real feedback while chasing
  this: "for now also disabel the live capture stuff." The confirmed
  cause above is unrelated to capture mode itself (the crash reproduced
  during plain background playback, no capture-mode interaction), but it
  stays off until the actual fix has had real playing time to prove
  out. Re-enable by flipping that one constant back to 1.
- **A second real-hardware freeze turned up more of the same printf()
  class, in different files.** Real feedback: "froze again with enough
  time." Unlike the haptics.c round above (needed capture mode plus a
  dense external clock), this one didn't need anything specific to be
  happening -- "enough time" was the whole trigger, which pointed at
  `main.c`'s main loop rather than any one feature: a periodic 2-second
  bring-up dump (`tiles_diag_i2c_scan_expected_devices()` -- 9 printf
  calls, one per expected I2C device plus a summary -- plus 3 more for
  power/Hall/standby state, 12 total) ran unconditionally on a plain
  wall-clock timer, forever, from the moment the device powered on,
  whether or not a serial terminal was ever attached to drain the CDC
  buffer. With no terminal open (the normal case once TILES is plugged
  into a DAW/synth rig instead of a dev machine), the buffer fills and
  every printf() call blocks up to `PICO_STDIO_USB_STDOUT_TIMEOUT_US`
  (500ms) -- up to 12 * 500ms = 6s of main-loop stall possible every 2s
  once the buffer settled into "always full." Activity-independent,
  unlike every other entry in this class -- explains a freeze regardless
  of mode or what the player was doing, purely from elapsed power-on
  time, matching the report exactly. Removed outright (the whole 2-
  second-timer block, not just the prints inside it -- dead scaffolding
  otherwise). The one-shot i2c scan still called once at boot
  (`tiles_op_mode_init()`'s own neighborhood) is untouched -- fires once
  before the loop starts, not on a recurring timer, so it can't
  accumulate. `services/touch.c`'s per-touch-edge print (fired on every
  touch AND release of every pad -- common during any play at all, not
  rare) and three more in `services/expression.c` (every committed
  note-on, every touch that ended without a real press, and one firing
  whenever a held pad's live pitch-bend value changed from the last
  value actually sent -- during genuine expressive play this changes on
  very close to every scan iteration, making it the closest thing in the
  codebase to truly continuous/unthrottled) went the same way. A fourth
  expression.c print, `[depth-cal]`, was already throttled to at most
  once per 150ms after an EARLIER pitch-bend freeze in this same file's
  history -- removed too, since its own comment already said to do so
  "once the real X(depth)/Y(depth) data has been captured and used to
  build an actual depth-compensation model," which the surrounding
  depth-compensated cosine-delta code shows had already happened. Same
  treatment as every other entry in this class: printf() and any
  print-justifying comment came out, logic comments stayed. Two now-
  unused includes (`pico/time.h`, `stdio.h`) came out of `touch.c` too.
  **First attempt at shipping this broke MIDI output, haptics, and the
  sequencer's own clock advance entirely -- root cause still
  unresolved.** The initial fix was built and flashed on top of the
  session's newer work (per-pattern sequencer scale locking, the scale-
  menu-exit standby-active guard, and -- the largest of the three --
  first-ever real flash persistence for sequencer patterns). Real
  feedback after flashing: "broke midi clock adn tap to play. seqcuencer
  will not play at all," then, after this exact printf fix was reverted
  in isolation and the OLDER code underneath was still broken: "no,
  revert to the last working version everything broke," confirmed even
  after a full USB power cycle (ruling out an I2C-peripheral-wedge
  theory from repeated rapid reflashing that round). Bisected by
  reverting one commit at a time: the checkpoint at this exact entry
  above (haptics fix, no persistence, no scale-lock, no standby-exit
  guard) was reflashed and confirmed genuinely working end to end (MIDI,
  haptics, sequencer playback) -- it just freezes again given enough
  time, the original bug this entry exists to fix. That isolates the
  "everything broken" regression to somewhere in the persistence /
  scale-lock / standby-exit-guard delta, NOT to this printf removal --
  which was never actually tested in isolation against the old code
  before being bundled with those three. This printf fix was then
  reapplied fresh on top of that confirmed-working checkpoint (this
  entry) and is believed safe on its own; persistence, the per-pattern
  scale lock, and the standby-exit guard are NOT currently reapplied and
  need their own isolated bisection in a future round before returning
  -- prime suspect is `storage/seq_store.c`'s flash erase/program path
  (the only piece of that delta touching real hardware state rather than
  pure in-memory logic), but this is not yet confirmed, only the leading
  theory. Their original commits are still in history (search for
  "Persist sequencer patterns across power cycles", "Fix scale picker
  exit breaking sequencer mode's own step view", and "Fix per-pattern
  sequencer scale never being applied to step pitches") for whenever
  that investigation resumes.
- **Per-pattern sequencer scale abandoned in favor of one universal
  scale -- supersedes the entry just above's per-pattern-lock work
  entirely, not a bug fix on top of it.** Real feedback: "i need it to
  be a universal scale for now." `op_seq_pattern_t` no longer has its
  own `scale` field at all; `scale_menu_enter()`/`scale_menu_exit()`
  (shift+triangle) no longer take a `per_pattern` argument or swap
  note_map.c's global scale slot in and out -- they just edit that ONE
  global scale directly, exactly like melodic/chord mode already did,
  now unconditionally regardless of `s_active_mode`. The standby-active
  render-ownership guard from the abandoned per-pattern round (real
  feedback: "why does pressing a pattern then a scale sewnd me back to
  a broken melodic layout") is kept in `scale_menu_exit()` -- that bug
  was about sequencer mode's OWN rendering surviving the picker closing
  at all, unrelated to whether the scale itself was per-pattern or
  universal, so it's still correct and still needed here.
  **Immediately refined further**: "not universaly in a way that alters
  the underlying pattern but it alters the real time playing pattern.
  so no rewriting lyust aproximating to the locked scale selected." A
  universal scale still leaves a real question -- what happens to a
  pattern's already-programmed steps when the scale changes out from
  under them? Two options: rewrite `step_note[]` in place (destructive,
  and ambiguous for any step whose pitch was an explicit hold-to-assign
  reassignment rather than its own pad's default -- there's no stored
  flag distinguishing the two, see seq_handle_step_taps()'s own
  comment), or leave storage untouched and reinterpret live at playback
  time (non-destructive, reversible, standard "quantize to scale" the
  way Ableton's own Scale MIDI effect or most hardware sequencers do
  it). Real feedback picked the second explicitly. New `note_map.c`/`.h`
  function `tiles_note_map_quantize_to_scale(uint8_t note)`: searches
  outward from `note` by semitone (0, then the nearest neighbor up or
  down, then the next, ...) for the closest pitch whose pitch CLASS
  (under the current key offset) belongs to the current scale, capped
  at one octave each direction -- always terminates well inside that cap
  given `current_scale_table()`'s own chromatic fallback (see that
  function's neighbor `scale_table_with_fallback()`) guarantees a
  same-pitch match at distance 0 for a genuinely empty/corrupt scale.
  Wired into exactly one place, `seq_fire_note()`: a step's frozen
  `step_note[step]` gets quantized to the CURRENT scale every time it
  actually fires, never rewritten in storage; the not-yet-overridden
  fallback branch (`tiles_note_map_get_note(pad)`) needs no such
  treatment since it already resolves live against the current scale on
  every call. Net effect: changing the scale instantly changes how every
  pattern sounds, and changing it back instantly restores the exact
  original pitches, because they were never actually touched.
- **A second real-hardware freeze, this time with a haptic motor found
  locked fully on afterward -- root cause and fix are both defensive,
  not a single confirmed smoking gun.** Real feedback: "this last freeze
  happened again and lockerd haptics on... it might be the sequencer
  triggering notes or clock or running. the sequencer might be
  fundamentally broken." Investigated and RULED OUT with actual source
  evidence, not assumption: `tiles_midi_clock_scan()`'s USB MIDI drain
  loop (unbounded-looking, but TinyUSB's own `CFG_TUD_MIDI_RX_BUFSIZE`
  hard-caps the underlying FIFO at 64 bytes -- see `midi/tusb_config.h`
  -- so it can't actually run away); `tiles_midi_note_on()`'s send path
  (`tud_midi_n_stream_write()`, read directly from the vendored TinyUSB
  source, is provably non-blocking -- it stops the instant `tu_fifo_
  remaining()` says there's no room, silently drops the rest, never
  waits); the sequencer's own ratchet loop (already guarded by `OP_SEQ_
  MAX_RATCHET`, confirmed correctly bounded). `services/haptics.c`
  itself has no loops at all -- its KICK/GAP/SUSTAIN state machine is
  purely timer-driven off `now_ms`, which means a motor "locked on" is
  consistent with being a SYMPTOM of the main loop stalling somewhere
  else entirely (whatever pad happened to be mid-KICK -- `set_motor_
  level(cfg, MAX_KICK_DUTY)` already sent -- simply never gets the
  follow-up call that would ever turn it back off), not a bug inside
  haptics.c's own logic.
  **What was actually fixed**: every one of this project's 6 files that
  talk I2C (`drivers/mpr121.c`, `tca9548a.c`, `pca9685.c`, `tmag5273.c`,
  `tca9554.c`, `diagnostics/i2c_scan.c`) used plain `i2c_write_blocking()`/
  `i2c_read_blocking()`, which the pico-sdk documents with NO timeout
  bound at all -- a single wedged I2C transaction (a real, well-known
  failure mode on a shared bus, and this board has several devices on
  each of its two buses: I2C1 alone carries both PCA9685 haptic drivers
  plus the TCA9554 LED mux) could hang the ENTIRE main loop forever.
  This fits every symptom without needing a confirmed trigger: haptics
  "locked on" (a motor mid-KICK when the hang hit, per the paragraph
  above), MIDI/sequencer/everything else also dead (the SAME stalled
  loop feeds all of them), and the user's own correlation with heavy
  sequencer-driven load (denser haptics/touch/Hall I2C traffic under 4
  active lanes raises the odds of whatever triggers a wedge in the first
  place, even though this fix doesn't identify what that trigger is).
  All 12 call sites switched to the pico-sdk's `_timeout_us` equivalents
  (`i2c_write_timeout_us()`/`i2c_read_timeout_us()`), 5000us per
  individual call -- generous headroom (the longest real transaction
  here, a 6-byte Hall XYZ read at this board's 100kHz/`TILES_I2C_
  DETECT_HZ` bus speed, takes on the order of 1ms) so a genuinely
  working bus should never trip it, while a wedged one now degrades to
  "this one transaction gets skipped, the loop keeps running" instead of
  "everything stops forever." Explicitly NOT a fix for whatever wedges
  the bus in the first place -- true I2C bus recovery (detecting the
  stuck state, then bit-banging 9+ clock pulses to force a stuck slave
  to release SDA, matching the NXP UM10204 spec's own recommended
  recovery sequence) isn't implemented here, only the guarantee that a
  wedge can no longer take the whole instrument down with it. If a
  freeze still recurs after this, that's the next place to look --
  correlate against which specific device (mux vs. sensor vs. haptic
  driver) was mid-transaction, since a bounded timeout converts a silent
  hang into information: `write_reg()`/`read_regs()`'s own return value
  now actually means something again.
- **It did recur.** Real feedback, after real runtime on the I2C-timeout
  build above: "it still froze eventually so we didnt fix it yet." Since
  a wedged I2C transaction can no longer hang forever (every call site
  now bounded, previous entry), the freeze surviving that fix rules I2C
  out as the actual cause -- confirmation, not just theory, that
  something else entirely was responsible. Went looking specifically for
  OTHER unbounded waits the I2C-focused pass wouldn't have caught, and
  found one: `drivers/sk6805.c`'s `tiles_sk6805_write()` (the addressable
  pad/underglow LED driver) calls pico-sdk's `pio_sm_put_blocking()`,
  which -- read directly from `hardware/pio.h`, not assumed -- is a raw
  `while (pio_sm_is_tx_fifo_full(...)) tight_loop_contents();` spin with
  NO timeout at all. Same failure class as the I2C bug, different
  peripheral entirely (PIO, not I2C), which is exactly why fixing I2C
  didn't touch it. If this state machine ever stops draining its FIFO
  for any reason, the wait never ends. This call site is arguably a
  BIGGER exposure than I2C ever was: `services/lighting.c`'s own header
  comment says pad LEDs are written ONE PIXEL AT A TIME (muxed via the
  TCA9554/CD74HCT4051, per `sk6805.h`'s own file header), so a single
  `tiles_lighting_service()` pass -- called unconditionally on EVERY
  main-loop iteration, not just occasionally like most I2C reads -- is
  roughly 24 separate blocking pushes for pads alone plus 4 more for
  underglow, all with zero protection.
  **Fix**: new `sk6805_put_blocking_with_timeout()` in `sk6805.c`,
  hand-rolled (pico-sdk has no built-in timeout variant for PIO the way
  it does for I2C) using the same `absolute_time_t`/`time_reached()`
  pattern the SDK's own `i2c_write_blocking_until()` uses internally --
  5000us per pixel push, the same generous-headroom philosophy as the
  I2C fix (at this driver's configured 800kHz bit rate a 24-bit pixel
  takes ~30us to shift out with an 8-word-deep TX FIFO behind it, so a
  genuinely working state machine should never come remotely close to
  5ms). `tiles_sk6805_write()` now `break`s out of its write loop (not
  a hard early `return`) the instant one push times out -- still falls
  through to the reset/latch `sleep_us()` afterward so timing stays
  consistent for whatever DID get written, and the very next `tiles_
  lighting_service()` call naturally retries with fresh pixel data, no
  partial-frame state to reconcile. Grepped the rest of the codebase for
  any other `pio_sm_put_blocking()`/`pio_sm_get_blocking()` call --
  this was the only one. Same honest caveat as the I2C round: this
  bounds the wait, it does not explain what stalls the PIO state machine
  in the first place -- if a freeze still recurs after this, PIO is now
  also ruled out, and the next place to look is something in `services/
  standby.c`'s deep-sleep/wake path or a genuine hardware brownout
  (power.c's still-unverified-on-real-hardware FAULT mode), not another
  unbounded-wait audit -- every blocking peripheral call in this
  codebase (stdio/CDC, I2C, PIO) has now had one.
- **It froze a third time, on the PIO-timeout build.** Real feedback,
  decisively: "dont do half ass fixes, lets figure out whats wrong
  logically and work through the bugs." Two rounds of "bound the
  suspect, ship it, wait and see" had each ruled out one real hazard
  (unbounded I2C, then unbounded PIO) without ever actually explaining
  the freeze -- a real pattern of progress, but not a diagnosis, and
  guessing a third unbounded-wait candidate blind wasn't a good next
  step. Real feedback redirected accordingly: build the tool to actually
  see what's happening on the next freeze, instead of guessing again.
  **New `services/debug_mode.c`/`.h`**: a real-hardware trace logger,
  toggled by holding diamond+square+circle (deliberately NOT the
  reserved 4-button combo -- see below) for 8 seconds, confirmed by the
  underglow pulsing steady red for as long as it's active. Every main-
  loop stage in `main.c` gets a one-character marker
  (`tiles_debug_trace('X')`) written immediately BEFORE that stage runs,
  so the trace stream is a continuous `TMPBoudEGSYLHCXK` (repeating once
  per loop, one letter per stage -- see `main.c`'s own call sites for
  which letter is which) for as long as everything's healthy -- and the
  LAST character printed if it hangs is unambiguously where it's stuck,
  not merely the last thing that finished. `services/op_mode.c`'s own
  per-lane sequencer advance gets a second layer of granularity inside
  the `S` stage (`'0'`-`'3'` for lanes 0-3) specifically because "it
  might be the sequencer triggering notes or clock or running" was real
  feedback's own leading theory -- if it hangs mid-`S`, the trace also
  says which lane.
  **Deliberately NOT `printf()`**: this project's entire reason for
  chasing freezes this session is that `printf()` over USB-CDC blocks
  for up to 500ms per call whenever nobody's draining it -- using it for
  a debug-mode trace would risk becoming indistinguishable from the bug
  it exists to diagnose, or worse, causing a freeze of its own that gets
  misattributed to the real one. `tiles_debug_trace()`/`_str()` write
  directly via `tud_cdc_write()` instead -- confirmed non-blocking by
  reading TinyUSB's own vendored source (`cdc_device.c`'s `tud_cdc_n_
  write()` is a plain FIFO write, `_write_flush()`'s endpoint claim is
  skip-if-busy, never a wait), gated on `tud_cdc_write_available()`
  first so a full buffer (nobody's terminal open) means trace bytes get
  silently DROPPED, never queued or waited for. Debug mode trades "every
  byte eventually arrives" for "never once blocks the caller," which is
  the one property this feature actually needs -- watch it live with a
  terminal open (screen/minicom/whatever's on hand at
  `/dev/tty.usbmodem*` or equivalent) and the moment the stream stops is
  the diagnosis.
  **Button choice**: diamond+square+circle, WITHOUT triangle, is
  deliberately not a subset of `services/game_mode.c`'s reserved
  4-button combo (triangle+diamond+square+circle) that just happens to
  omit one finger -- that combo's own `GM_HOLD_MS` is only 700ms, so any
  hand that includes triangle while working toward this feature's
  8-second hold would trigger game mode's secret entry several seconds
  first. Requiring triangle to be explicitly UP (not just unchecked)
  keeps the two from ever firing off the same held hand.
  **Underglow indicator bypasses `s_standby_active` entirely** -- a new
  `write_debug_underglow()` in `services/lighting.c`, called directly
  from `tiles_lighting_service()` whenever `tiles_debug_mode_is_active()`
  is true, writes the pulse straight to the underglow SK6805 chain,
  unrelated to whatever `s_underglow_rgb[]`/`s_standby_active` currently
  hold. Fighting over standby-active ownership to get an indicator
  visible "regardless of whatever mode/sub-view currently owns
  rendering" is exactly the bug class this file's own history already
  had to fix twice this session (search "standby active" above) --
  sidestepping that fight entirely, rather than becoming a third
  claimant on the same flag, was the deliberate design choice here, not
  an oversight.
  **Scope, deliberately**: main-loop-stage and per-lane granularity
  only for this first pass -- `services/hall.c`'s own per-pad loop and
  `services/lighting.c`'s per-pad round-robin are NOT individually
  traced yet, even though both are real suspects from the last two
  rounds. If the top-level trace narrows a future freeze down to
  consistently hanging mid-`H` or mid-`L` without already pinpointing
  the cause, that's the natural next place to add finer tracing --
  informed by what's actually been observed, not speculative
  instrumentation everywhere up front.
- **A promising new lead surfaced independently of any of the above**:
  extended real-hardware stress testing with a NEW USB cable produced no
  freeze at all, where the same testing previously had. Real feedback's
  own theory: "im guessing it might have been the power draw through a
  usb hub with mutiple devices like audio card webcam hdmi and more."
  This fits the evidence better than anything the I2C/PIO rounds turned
  up: intermittent rather than deterministic (a real software bug tends
  to reproduce given the same inputs; a marginal shared-hub power budget
  depends on what ELSE happens to be drawing current at any given
  moment), and it directly matches a suspicion already sitting
  unaddressed in this codebase's own history -- `services/power.c`'s
  FAULT mode carries a comment (see `drivers/pca9685.c`'s own trigger_
  kick() cross-reference) noting it's "never been exercised on real
  hardware" and "could plausibly be flickering into FAULT transiently."
  A brief brownout would also explain "haptics locked on" specifically:
  the PCA9685 keeps outputting whatever PWM duty it was last told while
  the MCU itself stalls or resets, with no code bug required at all.
  Neither the I2C nor the PIO timeout work was wasted regardless -- a
  transient voltage dip is exactly the kind of thing that could wedge an
  I2C transaction or stall a PIO state machine, so those fixes may well
  be part of why it's holding up now too, alongside the better cable.
  **Given a real, external, plausible cause with a real, external,
  practical fix (direct power, skip the loaded bus-powered hub) already
  in hand, this is not necessarily worth more firmware work on its own**
  -- logged here for the historical record and cross-referenced from
  the crash-report entry immediately below, which now makes it possible
  to actually CONFIRM this on a future occurrence instead of continuing
  to just suspect it.
- **Real hardware debug mode extended into an always-on crash recorder**
  -- real feedback: "yeah [add the power-mode trace] and implement in
  case of crash a report is generated capturing last activity before
  blackout." The live trace above only ever helped if a terminal
  happened to be open and someone was watching at the exact moment of a
  freeze; this closes that gap for good, and does two new things this
  project has never done before:
  **A hardware watchdog, enabled for the first time** (`hardware/
  watchdog.h`, armed in `tiles_debug_mode_init()`, pet via `watchdog_
  update()` at the very end of `main.c`'s loop -- after every other
  stage has already run this iteration, so a hang anywhere upstream
  means this call is never reached) turns a hang from "sits frozen
  forever until someone finds the cable" into an automatic reset within
  `DEBUG_WATCHDOG_TIMEOUT_MS` (1 second -- generous headroom over any
  realistic loop iteration, including a pathological one where several
  of this session's own 5ms I2C/PIO timeouts all fire in the same pass).
  Checked against the pico-sdk's own header comment specifically to
  confirm `watchdog_enable_caused_reboot()` reads false after a normal
  `picotool load -x` reflash (that goes through a different `watchdog_
  reboot()`/bootrom UF2 path that clears the same scratch marker), so a
  routine firmware update during development is never mistaken for a
  crash.
  **A trace ring buffer that survives that reset**: `tiles_debug_trace()`/
  `_str()` now ALSO record, unconditionally regardless of whether live
  debug mode is toggled, into a small buffer placed via `pico/platform/
  sections.h`'s `__uninitialized_ram` -- RAM the C runtime deliberately
  does NOT zero on boot the way it zeroes ordinary `.bss`, so it retains
  whatever was there across a watchdog reset (only an actual power loss
  clears real SRAM). On a boot where `watchdog_enable_caused_reboot()`
  reads true, whatever the live ring held at that exact moment (the last
  thing recorded before the hang, by construction) gets copied into a
  second, similarly-persistent snapshot before normal recording resumes
  fresh for the new session. The next time debug mode is entered (same
  8-second hold), a pending unreported snapshot gets dumped as a
  one-time readable report -- uptime when it froze, then the ring's
  contents in chronological order, so the LAST characters printed are
  unambiguously the last thing that happened -- before live tracing
  continues, then marked reported so it doesn't repeat on every
  subsequent entry. No new gesture, no separate retrieval tool: notice
  something seemed off, hold the combo, and the answer is right there.
  **The power-mode trace this entry's own title references**: a new
  edge-triggered marker in `main.c`, right after `tiles_power_scan()`,
  fires only when `tiles_power_get_state().mode` actually CHANGES --
  never periodic, deliberately, given this exact file's own earlier
  history of a periodic power-state print being a real freeze cause in
  its own right (search "froze again with enough time" above). Feeds
  the same always-on ring buffer as everything else, so a genuine
  power-mode flicker right before a hang -- confirming or ruling out the
  brownout theory in the entry just above -- would now show up in a
  crash report even if nobody was watching live when it happened.
- **The crash recorder above got its first real-hardware exercise
  almost immediately, and two real bugs in IT (not the thing it's
  diagnosing) turned up.** First result was the actual point of this
  whole feature working: a genuine hang, watchdog-recovered, snapshot
  intact, showing `TMPBoudEGS0123YLHcXK` (every stage marker, every
  loop, textbook healthy) repeating for hundreds of iterations and then
  stopping cold on a bare `T` -- meaning the freeze is inside `tud_task()`
  itself, TinyUSB's own USB-stack pump, not in ANY of the I2C/PIO/
  sequencer code the last several rounds focused on. Genuinely new
  information no amount of further I2C/PIO auditing would ever have
  found, and the reason this session moved to actually investigating
  `tud_task()`'s own vendored source next instead of patching another
  peripheral driver.
  Two bugs surfaced getting to that result, both in `services/debug_
  mode.c` itself:
  **The report text came out corrupted** -- "Uptime when i" directly
  concatenated with "Last activity before the freeze," the actual
  uptime number and half a sentence just gone. Cause: `dump_crash_
  report_if_pending()` queued a report's worth of text (several hundred
  bytes) across many back-to-back `tud_cdc_write()` calls with nothing
  pumping `tud_task()` in between, overflowing the 64-byte CDC TX FIFO
  (`CFG_TUD_CDC_TX_BUFSIZE`) fast -- `cdc_write_raw()`'s own correct
  "truncate to whatever's available, never block" contract then quietly
  dropped most of it. Fixed with a new `cdc_write_paced()` that pumps
  `tud_task()` and flushes between chunks so the FIFO actually drains
  before more gets queued into it -- used only for this one-time report
  dump, not for `tiles_debug_trace()`/`_str()`'s own hot path, where
  that pumping overhead would be wasted on characters that already
  arrive one main-loop iteration apart naturally.
  **A second crash landed mid-investigation of the first, and the live
  trace had already gone silent** -- `s_debug_mode_active` lived in
  ordinary `.bss`, zeroed on every reboot regardless of cause, so the
  watchdog's own recovery turned debug mode back off right when a
  repeated failure needed it watched most. Joined `s_live_trace`/
  `s_crash_snapshot` in `__uninitialized_ram` -- trusted as-is on a
  confirmed crash-recovery boot (so it auto-resumes exactly as it was
  the instant before the hang, live tracing and the underglow pulse
  included, no re-entry needed), forced to a known `false` on a
  genuinely fresh boot (where leftover SRAM content can't be trusted).
  The auto-resumed report dump itself is deferred ~2 seconds past boot
  (`tiles_debug_mode_scan()`, not `tiles_debug_mode_init()`) rather than
  attempted immediately -- at `init()` time `tud_task()` hasn't run even
  once yet this boot, so USB hasn't re-enumerated and `cdc_write_paced()`
  would just be racing a connection that doesn't exist yet.
  **`cdc_write_paced()`'s own pumping loop is deadline-bounded**, not a
  plain "keep trying until every byte lands" loop -- sharing ONE
  deadline (`DEBUG_REPORT_DUMP_TIMEOUT_MS`, 2s) across an entire report
  dump, checked via `time_reached()` (`hardware/timer.h`, pulled in
  transitively through the `pico/time.h` this file already includes).
  Without this, a report dump attempted while nobody's actually
  connected to drain the port would spin forever waiting for FIFO room
  that never appears -- the freeze-diagnostic tool causing a new freeze
  of its own, exactly the failure class this entire session exists to
  remove. `s_crash_snapshot.reported` is set BEFORE attempting the dump,
  not after a confirmed success, so a report that fails to deliver once
  (nobody connected yet) is treated as lost rather than retried on every
  subsequent debug-mode entry.
- **The crash recorder's own reliability fixes introduced a WORSE bug
  than the one they fixed: a self-inflicted reset loop.** Real feedback,
  after the crash reporter had been running for a while: "it happened
  again... this was not as prominent of an issue before." That "before"
  is the tell -- the recovery mechanism itself had made things worse,
  not the original `tud_task()` freeze getting more frequent on its own.
  Root cause: `DEBUG_REPORT_DUMP_TIMEOUT_MS` (the crash-report dump's own
  budget) was 2000ms -- LONGER than `DEBUG_WATCHDOG_TIMEOUT_MS`'s 1000ms.
  The dump runs synchronously inside ONE main-loop iteration, and
  `watchdog_update()` is only reached at the very end of `main.c`'s loop,
  after `tiles_debug_mode_scan()` (and so the whole dump) has already
  returned. Right after a crash-recovery reboot -- exactly when the
  auto-resumed dump fires -- a host-side monitor often hasn't reconnected
  yet, so the dump would burn most of its 2-second budget waiting for
  CDC FIFO room that never appeared, comfortably exceeding the
  watchdog's 1-second one WHILE STILL INSIDE THAT SAME FUNCTION CALL --
  triggering a SECOND watchdog reset before the first dump even
  finished. That reset re-arms the identical auto-resume dump on the
  very next boot, which could hit the exact same problem again: a
  reset loop with nothing to do with the original freeze, caused
  entirely by the tool built to diagnose it.
  **Fixed two ways, not one, on purpose**: `cdc_write_paced()` now calls
  `watchdog_update()` on every spin of its own pumping loop, the same
  way `main.c`'s own loop pets it once per iteration -- this loop can
  legitimately run for a while (a host that hasn't reconnected yet is an
  expected condition, not a hang: `tud_task()` is being called and real
  progress is being checked for on every pass), and the watchdog should
  only ever fire for an ACTUAL stuck main loop, not a bounded, self-
  monitoring wait that happens to take a while. `DEBUG_REPORT_DUMP_
  TIMEOUT_MS` also dropped to 400ms, comfortably under the watchdog's
  1000ms, as defense in depth -- so even a future change that forgets to
  pet the watchdog during a long wait can't reproduce this exact class
  of bug against this specific timer again. Calling `tud_task()` in a
  tight loop (this function's own pacing mechanism) doesn't add NEW risk
  of triggering the original freeze either, worth noting explicitly: if
  `tud_task()` itself is what hangs (not just returns nothing to report),
  neither this loop's `watchdog_update()` nor `main.c`'s own would ever
  be reached regardless of which call site triggered it, so the SAME
  1-second watchdog timeout still catches it exactly as it would
  anywhere else `tud_task()` is called.
- **Two-board comparison test (same firmware, same computer, different
  ports) narrowed the freeze further, and a sharp real-feedback question
  found a second genuine bug along the way.** Real feedback: "board 1
  crashed alone while running animations but board 2 didnt... then
  board 2 crashed a min later." Two different physical boards, same
  build, failing independently -- rules out a defect specific to one
  unit. Switching board 2 to a proper standard 3A/12V external supply
  (dedicated port, alongside USB) did NOT stop the crash, ruling out
  power delivery from the computer's own USB ports as the cause. Board
  1's own crash-report ring showed a DIFFERENT last character than every
  prior capture (`L`, `tiles_lighting_service()`, not `T`/`tud_task()`) --
  consistent with a real electrical/USB event manifesting wherever the
  CPU happened to be doing hardware I/O at that moment, rather than one
  specific software bug in one specific function. Both of the captures
  where the moment just before the freeze was actually visible (board
  2's second, board 1's second) showed the identical `[power->FAULT]`
  signature immediately preceding it -- and `power.c`'s own `raw_mode_
  from_pins()` shows FAULT requires `tud_mounted() == false`, meaning a
  REAL USB bus-reset/disconnect event is genuinely occurring each time,
  not a software artifact.
  Online research confirmed this general class -- a Bulk-IN endpoint
  double-buffering race across SOF interrupts on the RP2040/2350's
  shared USB peripheral silicon, "errata E15" -- is real, documented,
  and still actively being found and fixed upstream (an UNMERGED
  hathach/tinyusb PR fixing a specific E15 sub-bug in a newer, refactored
  `dcd_rp2040.c` than this project's own Dec-2024 vendored copy uses,
  confirmed by direct diff comparison not to transplant cleanly onto
  this older code's different `ep->pending`/`hw_endpoint_start_next_
  buffer()` approach). Also confirmed directly against the actual
  compiler invocation (not assumed) that this vendored copy's OWN E15
  mitigation (`TUD_OPT_RP2040_USB_DEVICE_UFRAME_FIX`, gating a real
  workaround block in `dcd_rp2040.c`'s SOF handler) is already compiled
  in via the normal `tinyusb_device` CMake link, contrary to an initial
  worry that it might simply be unset. Whether this specific project's
  older buffer-management implementation has its own analogous,
  not-yet-found bug in the same general area remains a genuinely open
  question -- narrowed a great deal (a real, named erratum class,
  actively receiving fixes upstream even now, not an exotic guess), but
  not yet a proven, fixed root cause.
  **A second real bug found while chasing this, independent of the
  freeze itself**: real feedback pushed back hard and correctly on
  accepting watchdog-recovery as good enough -- "why would it reboot if
  power is table. it might loose conection but not reboot" -- a
  genuinely sharp point: a USB disconnect on its own should never
  require a full CPU reset; something in the recovery PATH itself was
  making a bad situation worse. Investigating that turned up `services/
  boot_sequence.c`'s own power-on animation, which had been blocking on
  `sleep_ms()` for its entire ~4.7 second duration (`RAIN_DURATION_MS` +
  `FADE_DURATION_MS` + `PULSE_TOTAL_MS`) on EVERY single boot, watchdog-
  recovery included -- built on the exact same wrong assumption
  (services/boot_sequence.h's own now-corrected comment: "TinyUSB's own
  background IRQ task keeps USB alive regardless of what the main loop
  is doing") that `main.c`'s own `tud_task()` comment had already found
  and fixed for the MAIN loop, right at the start of this whole session,
  just never carried over to this file. Meaning USB got ZERO service for
  ~4.7s on every fresh power-on too, not just crash recovery -- exactly
  when a host is actively trying to enumerate the device and needs the
  MOST attention. Fixed two ways: a new `boot_frame_delay()` pumps `tud_
  task()` throughout each animation frame's own pacing wait instead of
  sleeping blind (so the animation itself no longer starves USB, for a
  normal power-on too), AND `main.c` now skips the animation (and its
  Hall-baseline recapture, whose own "a few settled seconds since a
  just-power-cycled MCU" justification doesn't apply after a WARM reset
  where the sensors never lost power) entirely on a confirmed crash-
  recovery boot via `watchdog_enable_caused_reboot()`, so recovery is
  bounded by USB re-enumeration time alone, not that plus a second
  ~4.7s animation nobody asked to sit through twice.
- **Same thread, next question -- real feedback: "bootloader mode ready
  on both but we need a practrical solution how long will boot take if
  fail hapens now? mittigationg it is not solving it."** Computing an
  honest number surfaced a further gap the animation-skip fix above
  didn't cover: `main.c` still ran its startup banner `printf`,
  `tiles_diag_i2c_scan_expected_devices()` (9 more `printf`s), and
  `tiles_calibration_init()`'s help-text `printf` unconditionally on
  EVERY boot, crash-recovery included. Each individual `printf()` over
  CDC can itself block up to `PICO_STDIO_USB_STDOUT_TIMEOUT_US` (500ms)
  whenever nothing is connected to drain the USB-CDC buffer -- the
  ordinary case once TILES is running inside a DAW rather than sitting
  on a dev machine with a terminal attached -- so those ~11 calls could
  add up to ~5.5 more seconds on top of the watchdog's own detection
  time, on every single boot, not just a fresh power-on.
  Fixed by computing one `bool crash_recovered =
  watchdog_enable_caused_reboot();` at the very top of `main()`, before
  any peripheral init, and reusing it at every boot-time-only call site
  (the banner, the I2C scan, `tiles_calibration_init()`, and the
  existing boot-animation skip, which used to call the same function a
  second time). Confirmed safe to read this early and reuse rather than
  re-checking fresh at each site: `watchdog_enable_caused_reboot()` is a
  pure read of `watchdog_hw->reason` and a scratch register
  (`pico-sdk/src/rp2_common/hardware_watchdog/watchdog.c`), and a repo-
  wide grep confirms the ONLY other caller is `services/debug_mode.c`'s
  own independent check inside `tiles_debug_mode_init()` -- which runs
  later in `main()` and doesn't call `watchdog_enable()` (the one thing
  that writes that scratch register) until after its own read, so both
  reads always agree.
  This doesn't touch WHY the original freeze happens -- only how much
  worse a recovery makes things once the watchdog has already caught
  one. With it, a crash-recovery boot's own honest worst case is: up to
  1000ms for the watchdog to actually trip (`DEBUG_WATCHDOG_TIMEOUT_MS`,
  the longest gap possible since the last `watchdog_update()` pet
  before a hang), plus whatever USB re-enumeration itself takes on the
  host side once the reset completes -- not fully under this firmware's
  control, but no longer artificially padded by ~4.7s of animation and
  ~5.5s of unread `printf`s on top, both of which are now skipped
  entirely rather than merely shortened. The remaining ~1s watchdog
  detection window itself is the next thing worth shrinking, but that's
  a deliberate, separate tradeoff (a shorter timeout risks false-
  positive resets on a legitimately busy loop iteration) rather than a
  bug -- not changed here.
- **Real feedback pushed back on the whole USB-erratum theory of the
  case, correctly: "still we need to re evaluate whatever we are
  missing to solve this issue even a usb data issue shoudnt cause the
  crash."** Right -- a USB bus-reset is a normal, constant event (host
  sleep/wake, hub renegotiation, marginal signal); a well-behaved USB
  stack shrugs it off, it doesn't freeze the MCU. That reframed the
  question from "is the E15 erratum real" (yes, and still open) to "what
  in THIS codebase's own reaction to it can hang," and led somewhere new
  and, this time, concretely fixable: an I2C bus wedge, not a USB one.
  `drivers/pca9685.c` already carried a real, past incident (a haptic
  motor locked fully on after a freeze) and its own honest fix at the
  time -- switch every driver from unbounded `i2c_write_blocking()`/
  `i2c_read_blocking()` to the 5ms-bounded `_timeout_us()` variants --
  plus an explicit, never-acted-on caveat: **"Doesn't fix whatever
  wedges the bus in the first place... true I2C bus recovery needs a
  bit-bang clock-pulse sequence this driver doesn't have."** Revisiting
  that caveat directly, against this project's actual vendored pico-sdk
  (tag `2.3.0`) source rather than assumption, turned up two real,
  confirmed gaps that together reopen the exact "hang forever" failure
  the timeout fix believed it had already closed:
  1. `i2c_read_blocking_internal()` (`hardware_i2c/i2c.c`) waits for TX
     FIFO room to submit the read-request byte BEFORE any of its
     timeout-checked waits even start -- `while (!i2c_get_write_
     available(i2c)) tight_loop_contents();` (inlined from `hardware/
     i2c.h:430`) -- and that specific wait passes no timeout check at
     all, regardless of the `_timeout_us` argument the caller passed.
     If the TX FIFO is ever left stuck full, this hangs forever, for
     reads only.
  2. Neither `i2c_write_timeout_us()` nor `i2c_read_timeout_us()` cleans
     up the peripheral when THEIR OWN timeout fires (as opposed to a
     hardware-reported abort) -- deliberately, per the SDK's own
     structure (nothing safe to clean up if the transaction might still
     be live on the wire) -- but the side effect is that whatever
     originally wedged the bus is never cleared, exactly matching the
     pca9685.c comment's own caveat. That leftover state is a very
     plausible way to leave the TX FIFO stuck full for gap 1 above to
     then hang on.
  Both gaps sit specifically on the READ path, and this codebase's own
  comments already, independently, called out its two read call sites
  as the highest-exposure spots in the whole tree: `drivers/mpr121.c`
  ("touch is polled continuously... an even more exposed path") and
  `drivers/tmag5273.c` ("the highest-volume I2C traffic in this
  codebase" -- 24 pads x every Hall scan, via 3 `TCA9548A` muxes on
  `drivers/tca9548a.c`, itself flagged as "one of the most exposed
  paths" for the exact same reason). `board_pins.h` confirms touch and
  Hall share I2C0; haptics and the LED mux share I2C1 -- a wedge from
  any device can affect any other device on the same bus, not just the
  one that caused it.
  **Fixed with the actual thing the past comment asked for and never
  got**: a new `drivers/i2c_bus.c`/`.h` (`tiles_i2c_write()`/
  `tiles_i2c_read()`) now sits between every one of the 5 chip drivers
  and the raw pico-sdk calls -- closing gap 1 by checking `i2c_get_
  write_available()` itself, against its own deadline, before ever
  calling into the SDK's vulnerable read function, and closing gap 2 by
  calling a new `board_i2c_recover_bus()` (`board/board_init.c`) on ANY
  transaction failure, read or write. That function is the actual
  bit-bang bus-recovery sequence the original caveat named and never
  built: takes the bus's SDA/SCL pins back as plain open-drain-style
  GPIO, pulses SCL up to 9 times watching for SDA to release (the
  standard recovery bound for a slave stuck mid-byte, NXP UM10204
  3.1.16), drives a manual STOP regardless, then restores I2C function
  and re-`i2c_init()`s the peripheral (which also resets its internal
  FIFOs via the RESETS block, independent of the external wire state).
  Deliberately NOT applied to `diagnostics/i2c_scan.c`'s own boot-time
  probe: it runs at `TILES_I2C_DETECT_HZ` (100kHz), before `board_i2c_
  set_run_speed()` raises both buses to their real 400kHz -- recovering
  mid-scan would jump straight to run speed before device discovery at
  the deliberately conservative detect speed has finished. A plain
  timeout (no recovery) is still exactly right there; the file now says
  so explicitly instead of leaving that as an unexplained inconsistency.
  Honest framing, matching how this session has treated every other
  finding: this explains a real, confirmed, previously-unaddressed gap
  that plausibly accounts for at least the `tiles_lighting_service()`-
  and touch/Hall-adjacent freezes, and directly satisfies "even a usb
  data issue shouldn't cause the crash" -- it doesn't; a USB glitch and
  an I2C wedge are best understood as two independent symptoms of
  whatever the actual shared electrical trigger is, not one causing the
  other. It does NOT identify that original shared trigger, and does
  NOT retroactively prove every past freeze was this specific mechanism
  rather than (or in addition to) the still-open USB E15 erratum class --
  that needs the same real-hardware soak test this whole investigation
  has run on every other change.
- **Real feedback, immediately following the crash-recovery-boot-skip
  fixes above: "we need an indicator for crash now that we skip boot
  sequence so turn underglow a pulsing red to indicate crash that can
  be cancelled or aknowledged by presing shift for 2 secodns on its
  own, turn debug mode light to sentia magenta instead of red to avoid
  confusion."** Direct consequence of skipping the boot animation on
  crash-recovery (see above): that animation used to be the only
  visible "something just happened" signal on any boot, so skipping it
  specifically for crash-recovery left exactly that kind of boot
  looking completely silent -- worth fixing given how much of this
  session was spent making crash-recovery fast specifically so it'd be
  usable mid-set; a fast but silent recovery still leaves the player
  wondering whether anything happened at all.
  New `services/crash_indicator.c`/`.h`: activated once at boot
  (`tiles_crash_indicator_init(crash_recovered)`, reusing main.c's own
  early `watchdog_enable_caused_reboot()` read rather than a fresh
  call), scanned every main-loop iteration thereafter
  (`tiles_crash_indicator_scan()`, trace char `'R'`) watching for SW6
  (circle -- "shift," per services/expression_control.h's own real-
  feedback quote: "our shift and power button is circle") held ALONE
  (every other function button up) for 2000ms, the exact hold this
  codebase's other one-shot gestures already use as their shape
  (services/debug_mode.c's 8s combo, the expression mute combo's 3s),
  just a different duration because that's what was asked for. "Alone"
  specifically avoids colliding with the two other gestures that also
  hold circle down (the debug-mode combo, the expression-mute combo) --
  without it, either of those would restart this gesture's own hold
  timer on every scan for no reason.
  Rendering: `services/lighting.c`'s `tiles_lighting_service()` checks
  `tiles_crash_indicator_is_active()` first thing and, if true, pulses
  the underglow red -- the exact same "bypass s_underglow_rgb[]/
  s_standby_active entirely, write straight to hardware" mechanism
  `write_debug_underglow()` already established (see this file's own
  debug-mode entry, above, or lighting.c's comment on that function) --
  reused rather than reinvented, since "must stay visible no matter
  what else owns rendering" is exactly the same requirement both
  indicators have. Same sine-pulse shape/timing as the debug pulse too
  (0.35-1.0, 900ms period), kept as a separate copy per this file's own
  established convention for that shape (its own comment: "not shared
  code... just the same established visual convention").
  **Recoloring debug mode**, the other half of the same feedback:
  debug mode's own pulse (active since earlier this session, confirmed
  by "the underglow pulsing red steady") moved from red to Sentia
  Magenta (R and B both scaled by the same pulse level, G stays 0, so
  the hue stays true magenta throughout the pulse, not just at full
  brightness) -- red is now this firmware's one and only "a crash just
  happened, unacknowledged" color, on purpose, so the two can never be
  mistaken for each other the way they could have if both indicators
  had ended up red. Since services/debug_mode.c's own active-state
  flag lives in `__uninitialized_ram` and survives a crash-recovery
  reboot, debug mode and a fresh crash indicator CAN genuinely both be
  active at once (debug mode was already on from before the crash);
  `tiles_lighting_service()` gives the crash pulse priority in that
  case -- the more urgent, less-expected thing to see, versus debug
  mode being on, which the person already knows since they turned it
  on themselves.
- **First real-hardware pass on the crash indicator found two things to
  fix, both from real feedback: "the dismiss didnt work it just made
  the red color solid make the dismiss return to regular underglow and
  dimsiss is a single shift click not a hold."**
  **Bug (not a design change): underglow never restored on dismiss.**
  `tiles_lighting_service()` only ever WROTE the underglow from inside
  the crash/debug override branches -- there was no `else` case for
  "neither is active." That's fine while pads are involved (`write_pad()`'s
  own round-robin below keeps re-driving them regardless), but
  underglow has no other continuous per-frame driver, so the instant
  the override stopped being active, NOTHING wrote to it again -- it
  just stayed latched at whatever brightness the pulse happened to be
  at that exact frame, forever, reading as "solid" rather than restored.
  Fixed by tracking whether either override owned the underglow last
  frame (`s_underglow_override_was_active`) and, on the exact frame
  that flips from owned to not-owned, calling `write_underglow()` once
  -- which pushes `s_underglow_rgb[]`'s current value (always kept
  correct underneath the override by whatever normally owns it, the
  default or standby's animation) back to hardware, the same explicit-
  restore reasoning `tiles_lighting_set_standby_active(false)` already
  uses for the identical reason. This was a latent bug in debug mode's
  own pulse too (toggling debug mode off while running would have had
  the exact same "stays solid" symptom) -- the fix covers both since
  both go through the same override/restore path now.
  **Design change: hold -> single click.** `services/crash_indicator.c`
  originally required holding circle alone for 2000ms (matching the
  original ask); real feedback after trying it said single-click
  instead. Now fires on release of a circle-press during which no
  other function button was EVER also down (tracked for the whole
  press, not just checked at the release instant) -- so it can't
  accidentally fire off the tail end of the debug-mode or expression-
  mute combos if a hand lifts off them one finger at a time, circle
  last.
- **Real feedback, same test: "ok testing on both boards now they both
  crashed... the recovery time is good but still we need to dig deeper
  for the cause."** Confirms the recovery-time work (watchdog +
  boot-skip fixes) is working as intended, and confirms the I2C bus-
  recovery fix above did NOT stop these particular crashes -- an honest
  result, not the fix being wrong, just not the whole story.
  Checked macOS's own kernel USB log (`log show`) for the exact test
  window rather than guessing further from firmware alone, and found
  something that reframes the investigation: `AppleUSBHostFamily`
  itself logs `AppleUSBHostPort::terminateDevice: ... hardware
  connection lost` for the TILES device, repeatedly, roughly every
  10-45 seconds during the test window -- and several of the
  reconnects that follow come back as `RP2350 Boot` (the ROM
  bootloader), not the application. A watchdog reset ALWAYS re-runs the
  application; landing in the bootloader instead means something more
  severe than an app-level hang is happening -- most consistent with a
  genuine, brief power/connection interruption on the USB-C port itself
  (the same port-level Type-C signaling/current-renegotiation activity
  -- `AppleUSBHostResourcesTypeC::allocateDownstreamBusCurrentGated`
  briefly granting 0mA -- already spotted once earlier this session),
  not a firmware bug reacting to a bus-reset event. This is also
  consistent with real feedback mid-session: a completely unrelated
  USB MIDI device (a Novation Launchkey) on the same Mac was ALSO seen
  glitching, including once with no plug/unplug action at all -- a
  device-agnostic, host/port-level cause explains that far better than
  two unrelated devices independently developing the same symptom at
  the same time. Separately confirmed (and unrelated to the crashes):
  this session's own `picotool info -a` calls show up in the same log
  as `picotool@(null): ... failed to open Launchkey MK4 61` --
  picotool's device scan opens EVERY connected USB device to check it,
  not just Raspberry Pi ones, so at least one Launchkey disturbance
  during this session was this investigation's own tooling, not a
  mystery. No evidence of Ableton specifically in the log (no
  Ableton-attributed line anywhere in the window checked) -- doesn't
  rule out something it does indirectly, but the direct culprit visible
  here is the host's own USB-C port/power stack, not an application.
  Not yet root-caused further than that (which port, which cable, a
  hub in between, macOS's own Type-C power management, or the ports
  themselves) -- next real step is testing whether the disconnect
  frequency changes on a different port/cable, independent of anything
  in this firmware.
- **A batch of real feedback, all in one message, spanning capture mode,
  haptics arbitration, tap tempo, triangle's LED, and pattern
  persistence:** "lets re work capture mode into sewquencer witgh the
  diamond button but it should still display the pattern oplaying
  withthe leds unver the scale melodic layout leds that will show like
  the moving sequencer will appear and be red on enabeled steps also
  make sure to quatize capture mode to closest step. double check we
  have implemented gflobal scale aproximation in all modes when a
  scale is selected. to save patterns to memory before shutdown in the
  pattern selector menu we click shift and the pattern. that saves it,
  to delete or clear pattern we hold shift and patterrn for 3 seconds
  also tap tempo should auto triggere the current pattern also make
  trisngle fhash if pattern is playing and we exit to a different
  screen than the playing pattern. also in regular melodic mode when
  pattern is still playing haptics react to melodic not to the
  patterns in the backgorund. that also goes when switching betweeen
  enabeled patterns, only the dispalyed one has the haptics overide."
  Taken one piece at a time:
  - **Global scale approximation, audited, not changed**: verified
    directly against the code rather than assumed. `tiles_note_map_
    get_note()` (melodic/chord live play) and `tiles_note_map_
    quantize_to_scale()` (frozen/stored pitches -- every sequencer
    step with `step_pitch_override` set, capture-mode-recorded notes
    included, once they play back normally) both already correctly
    resolve against whatever scale is currently selected, live, on
    every call. Chord mode adapts via `chord_mode_scale_table()`
    (falls back to Ionian when the global scale isn't a 7-note
    diatonic one, so its chords/melody always stay musically real)
    rather than using the raw global scale directly -- a deliberate,
    already-correct choice, not a gap. Guitar mode and capture mode's
    OWN live-input scale (forced chromatic while active, by original
    design: "turns into the regular chromatic scale") are the two
    genuine, intentional exceptions -- a fretboard and a deliberate
    "always chromatic while recording" mode aren't supposed to be
    scale-approximated in the first place. Nothing to fix here.
  - **Sequencer capture mode, re-enabled and reworked.** Previously
    disabled (`OP_SEQ_CAPTURE_MODE_ENABLED 0`) after a past crash
    report, even though the confirmed cause was unrelated (haptics.c's
    own printf flooding, already fixed) -- flipped back to `1`, no
    change needed to the entry gesture itself (plain diamond click,
    already exactly what was asked for). Two real reworks alongside
    re-enabling it: `render_seq_capture()` now shows an armed
    (already-recorded) step as `OP_SEQ_DIM_RED_LEVEL` -- the exact dim
    red `render_sequencer()` already uses for "armed at rest" in the
    normal step view -- layered UNDER the existing moving-playhead
    pulse and OVER the melodic root/natural coloring, so a step
    already holding a note reads clearly at a glance while recording,
    not just once you leave and check the normal view. And capture
    mode's own quantization moved from "always the step in progress"
    to nearest-step: `seq_capture_handle_taps()` now computes, at the
    instant of touch, how far into the CURRENT step's window the touch
    landed, and targets the NEXT step instead once past the halfway
    mark (`s_seq_capture_target_step`) -- an early hit (anticipating
    the beat, a real and common thing) used to always round backward
    onto whichever step was about to end; now it rounds to whichever
    step it actually meant. `seq_capture_advance_clock()`'s own commit
    logic changed to match: only commits the pending note into the
    step it was actually quantized to (which might still be one step
    away), and explicitly clears whatever step IS ending right now if
    the pending note wasn't meant for it -- preserving capture mode's
    existing "this pass replaces the whole pattern with what you just
    played" behavior instead of accidentally leaving stale content
    behind. Genuinely new to real playing time again as of this
    change (the original disable was out of caution, not a proven
    bug) -- worth watching closely on the next hardware pass.
  - **Haptics: only the displayed lane, not every background one.**
    Real feedback reverses this file's own prior, explicit tradeoff
    (`seq_end_current_note()`'s old comment accepted background-lane
    haptics as "rare, momentary, cosmetic"). `seq_fire_note()`/`seq_
    end_current_note()` now gate `tiles_haptics_trigger_kick()`/`_stop()`
    on `seq_lane_haptics_visible()` (sequencer mode active AND this is
    `s_seq_edit_lane`) -- the actual MIDI note-on/off is completely
    unaffected, every enabled lane keeps sounding exactly as before,
    only its physical buzz is suppressed when it isn't the one on
    screen. Captured once at fire time into a new per-lane `s_seq_
    sounding_haptics[]`, not re-checked at end time -- the active mode
    or edit lane can change mid-note, and ending a note must always
    undo exactly what starting it did, the same reasoning `s_seq_
    sounding_channel`/`_note` already established for this exact
    class of bug.
  - **Tap tempo now auto-starts the pattern you're looking at, not
    just the clock.** `tiles_midi_clock_register_tap()` already
    autostarts the shared clock/transport on the first 4-tap
    establishment (a previous round's real feedback), but that's the
    CLOCK, not any specific lane -- closed with a new check in `tiles_
    op_mode_scan()`: a `start_edge` that's specifically tap-tempo-
    sourced (`clock.source_is_tap_tempo`) now also starts `s_seq_edit_
    lane` if it wasn't already running, mirroring "+"'s own fresh-
    start sequence exactly. Deliberately NOT extended to a real
    external Start message -- only tap tempo was asked for, and a
    DAW's own Start already has its own separate transport meaning
    elsewhere in this file.
  - **Triangle now flashes when a pattern is playing somewhere you
    can't see it.** `render_sequencer()` already shows the moving
    playhead directly on the grid, so this only fires for every OTHER
    mode (melodic/chord/guitar) -- a hard on/off blink (this file's
    own established "flash" language, reusing `OP_PATTERN_BANK_FLASH_
    MS`'s timing), on whenever `any_lane_running()` is true, off the
    instant nothing is.
  - **Patterns can now be saved to flash and survive a power cycle.**
    New in the "Pattern bank" (shift+diamond): holding circle down
    from the moment a cell is first touched turns that touch into a
    save/delete candidate instead of the bank's normal select gesture
    -- release before 3 seconds saves that slot, holding past 3
    seconds clears it (both to/from a new `tiles_pattern_store_t` in
    the LAST 4096-byte sector of the chip's 4MB flash, loaded back at
    boot in `tiles_op_mode_init()` for any slot that was ever saved).
    Real, unavoidable hardware cost worth being explicit about: RP2040/
    RP2350 flash is executed from directly (XIP), so nothing can read
    from flash -- any instruction fetch, ISR included -- while it's
    being erased/programmed; confirmed reading pico-sdk's own flash.c
    that `flash_range_erase()`/`flash_range_program()` do NOT disable
    interrupts for you. `pattern_store_write_all()` wraps both in
    `save_and_disable_interrupts()`/`restore_interrupts()` (correct for
    this single-core firmware), petting the watchdog immediately
    before and after (never during -- `watchdog_update()` is ordinary
    flash-resident code, unsafe to call from inside that same disabled
    window) rather than trying to avoid the pause: a 4KB erase +
    program on this board's own W25Q-family chip comfortably finishes
    well under the watchdog's own timeout, but it IS a genuine, brief
    (tens of milliseconds) freeze of everything -- MIDI, touch, USB --
    exactly once, at the exact moment a save/delete is triggered. An
    inherent property of writing this chip's flash, not something
    worth engineering around for a deliberate, occasional action.
- **A live-testing crash, captured with debug mode armed, put the
  crash investigation on new footing.** Real feedback isolated the
  trigger cleanly: "i found what causes it, its when ableton is
  playing, boards dont crash when ableton is not playing and not
  sending clock somehow it also affects my novation but it is based on
  the tiles devices." Then, moments later, live: "tiles crashed tiles
  2 crashed and novation crached rn." With debug mode already armed,
  that crash's own auto-dumped report was real, hard evidence instead
  of another round of guessing from code alone: board 1's crash report
  ring ended at `L` (`tiles_lighting_service()`) -- the SAME function a
  crash captured earlier this session also ended at, confirming this
  is a genuinely recurring hang location, not a one-off.
  Re-read that function's two blocking operations line by line rather
  than assume: the I2C mux-select calls (now routed through drivers/
  i2c_bus.h's tiles_i2c_write(), from this session's own earlier fix)
  and the SK6805 PIO write (drivers/sk6805.c's sk6805_put_blocking_
  with_timeout()) BOTH already have real, working timeout bounds --
  confirmed reading the actual code, not assumed from memory. The
  SK6805 one, it turns out, predates tonight entirely: its own header
  comment records a past "second real-hardware freeze" found after the
  first I2C-focused round, with the identical "pio_sm_put_blocking() is
  a raw spin with no timeout, same failure class" diagnosis this
  session kept independently re-deriving for I2C. That both timeouts
  are real and already in place, yet the hang still recurs at exactly
  this function, is the honest, open puzzle right now -- not yet
  explained, not papered over as solved.
  What WAS a real gap: main.c's own per-stage trace only proves the
  hang is somewhere inside `tiles_lighting_service()`, not which of
  its two genuinely different operations. Closed with two new trace
  characters local to this file -- `'i'` right before write_pad()'s
  4 TCA9554 I2C calls, `'w'` right before every SK6805 write (pad and
  all 3 underglow paths share it; which override was active that
  instant narrows pad vs. underglow back down if it matters) -- so the
  NEXT occurrence's crash report says definitively which class of
  operation it was, instead of leaving that as this round's own still-
  open question.
  Separately, chasing a related but different observation ("even
  before crash i can see the midi clock lights [in Ableton] not being
  in perfect sync from ableton internal clock and the clock its
  reciveing in return" -- clarified as Ableton's own MIDI activity
  lights, and confirmed Ableton has Track/Sync/Remote enabled on this
  same port, bidirectional): `handle_diamond_transport()`'s Play/Stop
  button was sending a raw MIDI Start/Stop byte back to Ableton
  alongside its own CC (the CC being "the primary, verified path," per
  this function's own long-standing comment, the Realtime byte only
  ever "a harmless bonus for a Sync/Ext setup") -- REGARDLESS of
  whether Ableton was already the one driving the clock TILES is
  slaved to. With Sync enabled on that same bidirectional port, that's
  a redundant, self-referential signal with no good reason to exist
  once Ableton is already the master -- now skipped specifically when
  `tiles_midi_clock_external_active()` is true, CC still sent either
  way (so the actual Play/Stop/Record remote-control feature is fully
  intact), Realtime byte still sent normally whenever no external
  clock is active (e.g. starting Ableton from fully stopped, where
  TILES genuinely is the one initiating). Not confirmed as a
  contributor to the crash itself -- MIDI clock desync and a firmware
  hang are different failure classes -- but a real, independently
  worth-fixing bug regardless, found investigating the same report.
- **A six-item batch of real feedback on capture mode, sequencer
  visuals, per-step probability, chord mode, and button-combo
  conflicts, all from one message, requested before the next round of
  crash testing:**
  - **Capture mode is additive again, not a replace.** "its
    additive and accumulates. i[t] shouldnt just override empty
    space. it a[d]ds whatever is being played on top not cle[a]ring
    previous steps" -- corrects this session's OWN earlier assumption
    (the previous entry above's "capture mode replaces a pattern's
    content with exactly what got played this time, not an overdub").
    `seq_capture_advance_clock()`'s commit no longer clears a step it
    didn't target this pass -- an untouched step now keeps whatever it
    already held, from an earlier capture pass or a manual arm,
    unconditionally.
  - **Triangle's background-pattern indicator is a slow pulse now, not
    a fast blink.** "the flashing of triangel is too fast... it should
    be a pulsing like the deep sleep pulse." New `background_pattern_
    pulse_level()`, same 3000ms pacing as `services/standby.c`'s own
    deep-sleep pulse (not shared code, same "same convention, separate
    copy" precedent as this file's other pulse shapes), brightness
    range raised to something a button LED actually needs to be seen.
  - **Save/delete now flash to confirm.** "we need a flash in green to
    confirm when a pattern is saved. flash green twice in underglow
    and pad. and for delete flash red twice." `pattern_store_save_
    slot()`/`_clear_slot()` now arm a short (`OP_PATTERN_FLASH_TOTAL_
    MS`, two full on/off cycles) override that `render_pattern_bank()`
    checks first, ahead of every other per-cell/underglow state --
    green for save, red for delete, on the one cell actually acted on
    plus all 4 underglow anchors.
  - **Per-step probability actually does something now.** "the chance
    porcentage when holding a step is not functioning properly its
    not adctually doing the chance." Root cause: `seq_enter_step()`'s
    probability roll was always correctly gated on `pat->probability_
    enabled`, but the ONLY thing that ever set that flag true was a
    circle-click on the pattern/channel picker this session's own
    "Sub-menu made universal" round removed outright (see that
    entry, much earlier in this file) -- with no replacement access
    point, it had stayed permanently false, for every pattern, ever
    since. Every dialed-in percentage was being faithfully stored and
    rendered but never once consulted at playback time. Fixed at the
    one place a percentage can be dialed in at all: entering the
    per-step probability editor (holding a step past `OP_SEQ_
    PROBABILITY_HOLD_MS`) now also sets `probability_enabled = true`
    on that pattern -- deliberately never auto-disabled again, same
    "master switch a performer can flip back to fully deterministic"
    framing the field's own comment already established.
  - **Chord mode simplified to velocity-sensitive triad + bass.** "the
    tap and then complex chord is not working nice so lets simplify
    to velocity sensitive chords with bass note not dual type of
    chord or light tap to chord." Removed the two-tier, live-morphing-
    by-depth design (a light tap gave a plain triad, pressing past
    halfway escalated to a full rootless-jazz voicing) entirely, not
    tuned -- every chord pad now always plays the same 4-voice shape
    (bass + root + fifth + an octave-raised third). In its place, real
    strike velocity: new `tiles_expression_velocity_from_strike()`
    (`services/expression.h`) exposes that file's own already-tuned
    velocity curve so chord mode doesn't have to separately guess and
    tune a second one, and a new `TILES_EXPRESSION_MIN_STRIKE_DEPTH_
    DELTA` constant (which `expression.c`'s own `MIN_STRIKE_DEPTH_
    DELTA` is now defined FROM, not duplicated alongside, so the two
    can never drift apart) is the exact crossing point that curve is
    calibrated against. `handle_chord_pad_taps()` now mirrors
    `expression.c`'s own touch-start/peak-depth strike tracking,
    deliberately without that file's extra post-crossing follow-
    through wait (real Hall depth crosses a meaningful threshold
    within single-digit milliseconds of a genuine press, well under
    perceptible latency) or its release-triggered fallback commit (a
    touch that never crosses just stays silent -- simpler, an accepted
    difference for a chord pad vs. a single melodic note).
  - **Button combos now require their own complete, exact set.**
    "fix combo presses like game mode enter triggering accidentaly
    the haptic mute or debug mode triggering haptic mute. combo
    presses should evaluate the complete combo not execute multiple
    different combos at once." `services/expression_control.c`'s own
    mute combo (`circle_held && square_held`) had no exclusion of
    diamond or triangle at all -- an EARLIER real-feedback fix already
    skips this file's whole scan while triangle+diamond are BOTH held
    (game_mode.c's own 4-button entry combo), but `services/debug_
    mode.c`'s own diamond+square+circle hold (deliberately excludes
    triangle, precisely to avoid colliding with THAT combo) was never
    covered -- and since `EXPRESSION_MUTE_HOLD_MS` (2s) is shorter than
    debug mode's own 8s hold, every attempt to enter debug mode was
    also toggling mute partway through. `combo_held` now additionally
    requires diamond AND triangle to both be up, not just checked
    together -- closes the debug-mode gap directly and is strictly
    stronger defense-in-depth for the already-handled game-mode case.
- **Every crash report captured this session was showing the wrong
  moment.** Continued digging on the recurring freeze -- both boards
  crashed again during a live Ableton-playing test ("it crashed"), and
  the newest report from each showed the exact same signature every
  single prior report this session had also shown: `Uptime when it
  froze: 20 ms`, and a 256-byte ring completely full of just one
  call's own trace characters (`i`/`w`, `write_pad()`'s I2C-mux-select
  and SK6805-write stages) repeating dozens of times with nothing else
  interleaved at all. That specific signature -- isolated, only two
  characters, very early uptime -- doesn't match the main loop's
  per-iteration `write_pad()` call (which would show the OTHER stage
  characters mixed in between pads); it matches `tiles_lighting_
  init()`'s own one-time 24-pad boot sweep instead, called from
  main() before the main loop exists at all. Root cause, once that
  clicked: `tiles_debug_mode_init()` -- the ONLY place that ever
  copied the crash-surviving trace ring into the reportable snapshot
  -- doesn't run until near the bottom of main(), AFTER `tiles_
  lighting_init()`/`tiles_buttons_init()`/`tiles_touch_init()`/`tiles_
  hall_init()`/the boot sequence have all already run. `tiles_debug_
  trace()`'s ring write is unconditional (recorded regardless of
  whether debug mode itself is toggled on, confirmed by reading `record_
  to_live_ring()` directly), and every one of those calls invokes it --
  meaning on EVERY crash-recovery reboot, that early activity was
  overwriting the ring's real evidence of the ORIGINAL hang before the
  snapshot ever got taken. Every report this session showed the
  recovery boot's own boot-time noise, never the actual freeze -- the
  crash reporter had a blind spot for its own most important case from
  the moment it shipped. Fixed by splitting the copy out into a new
  `tiles_debug_mode_capture_crash_snapshot(bool crash_recovered)`
  (`services/debug_mode.c`/`.h`), called from the very first lines of
  `main()` -- immediately after `crash_recovered` itself is computed,
  before `tiles_usb_device_init()`, `board_init()`, or anything else
  that could touch the ring. `tiles_debug_mode_init()` keeps its other
  job (clearing the ring fresh, arming the watchdog) at its usual spot
  near the bottom, just no longer the snapshot copy. This doesn't fix
  the underlying freeze -- it fixes the TOOL built to find it, which
  until now had never actually shown its real cause even once. The
  next captured report should finally show what's really happening in
  the moment things lock up, not this false lead.
- **A crash-recovery reboot used to come back to a clean slate --
  now it restores mode, scale, and per-lane play state too.** Real
  feedback, spotted mid-test: unit 1 crashed on a completely different
  path than unit 2's Ableton-correlated one (idle -> touched it ->
  entered screensaver -> crashed), suggesting more than one root cause
  is still in play; separately, and regardless of what eventually
  fixes the underlying freeze(s): "after crash it dosnt reset to last
  active screen and settings. its just rebooting to clean slate. we
  need to make sure it reboots to last state completely includeing
  sequence, layout, scale, play state." Sequence PATTERN CONTENT was
  already safe -- `pattern_store_load_all()` reloads every saved slot
  from flash on every boot, crash-recovery included, unconditionally.
  Everything else the player can see/hear as "what the board was doing"
  was not: `tiles_op_mode_init()` and note_map.c's scale/octave/key
  state were ordinary statics, which the C runtime re-initializes to
  their compiled-in defaults on every reset, watchdog-caused ones
  included -- only `__uninitialized_ram` (`pico/platform/sections.h`)
  survives that, the same mechanism `services/debug_mode.c`'s own
  trace ring already relies on. Moved into that section: `services/
  op_mode.c`'s `s_active_mode` ("layout"), `s_seq_lane_running[]` and
  `s_seq_active_alt[]` (per-lane "play state" -- which alt pattern
  each lane has selected, and whether it's running), and `services/
  note_map.c`'s `s_scale`/`s_octave_shift`/`s_key_offset`. Both files'
  init functions now take a `crash_recovered` parameter (main() passes
  the same value it already computed at the top, same precedent as
  every other boot-time skip there) and simply don't re-default those
  specific fields when it's true -- everything else about a fresh vs.
  recovery boot stays exactly as it already was. Two things restoring
  the raw flags alone wouldn't have gotten right, both handled at the
  end of `tiles_op_mode_init()`: note_map.c's own guitar-mode/chord-
  mode flags need to be re-synced to whatever mode was restored (fixed
  by replaying `set_active_mode()` with the mode already in place --
  its own inequality guards correctly no-op every "leaving" branch and
  only run the "entering" ones when old and new match, so this is safe
  even though nothing is actually being left); and a restored "lane is
  running" flag alone wouldn't make it AUDIBLE again, since `tiles_
  midi_clock_is_running()` isn't persisted the same way and would stay
  false forever, silently starving `seq_advance_clock()` of a running
  clock to advance against (fixed with an explicit `tiles_midi_clock_
  set_running(true)` if any lane comes back running). Deliberately
  scoped OUT: `s_transport_playing`/`s_transport_recording` (this
  device's own belief about ABLETON's transport state, not this
  device's own playback) stay reset-on-every-boot -- MIDI has no way
  to query the DAW's actual state back, Ableton may well have kept
  running or been stopped by hand during whatever downtime the crash
  caused, and restoring a guess risks the diamond button's next click
  sending the opposite of what the host actually needs. This is also
  scoped to surviving a watchdog RESET specifically, not a real power-
  off -- RAM doesn't survive that either way, and actual cross-power-
  cycle persistence remains the separate, larger, not-yet-built
  profiles/ module note_map.h's own header already anticipated.
- **The crash-report timing fix paid off immediately: the first real
  (non-boot-noise) freeze trace captured all session.** Live-testing
  crash, both boards independently, right after flashing the snapshot-
  timing fix above. Both reports finally showed a realistic uptime
  (~455-461 SECONDS, not the bogus "20 ms" every single prior report
  showed) and the ring's healthy repeating cycle (`TMPBRoudEGSYLwiwHcXK`)
  cut off at the EXACT SAME point on both boards: right after the 'w'
  trace for the underglow SK6805 write (debug mode's magenta pulse was
  active on both, per the standing test convention), with NOTHING
  recorded afterward for the rest of the ring. This is real signal, not
  noise -- two independently-running boards landing on the identical cut
  point, at realistic multi-minute uptimes, is exactly what the snapshot-
  timing fix was supposed to finally reveal.
  Genuinely strange part, though: the code at that exact point --
  `services/drivers/sk6805.c`'s `tiles_sk6805_write()`, a per-pixel loop
  each bounded to `TILES_PIO_TIMEOUT_US` (5ms), then one `sleep_us()` for
  the 300us reset/latch pulse -- has no plausible path to explain
  holding the CPU silent for anywhere near the full 1000ms watchdog
  window, even in its own absolute worst case (every one of underglow's
  handful of pixels timing out back to back). Either that 5ms timeout
  genuinely isn't bounding the wait the way its own code implies it
  should here, or the true hang is happening somewhere else entirely --
  an interrupt context, most plausibly -- with this 'w' simply being
  the last thing the MAIN loop got to record before something else froze
  it from outside. Added one more trace character to tell these apart
  on the next occurrence: `tiles_debug_trace('x')` right after all 4 of
  `tiles_sk6805_write()`'s call sites (`write_pad()`, `write_debug_
  underglow()`, `write_crash_underglow()`, `write_underglow()`) in
  `services/lighting.c`. `w` then `x` next time means the hang is AFTER
  this call returns; `w` with no `x` means it's genuinely stuck inside
  the call despite its own timeout. Not yet resolved -- this is the
  clearest lead the whole investigation has produced, and the next
  capture should finally answer which half of the bisection it is.
- **Diamond's transport LED now shows the real clock signal, not this
  device's own guess about Ableton -- and sequencer mode gets a genuine
  four-state indicator instead of showing nothing.** Real feedback: "the
  play light indicator is working[,] wherever youre getting th eplay
  indicator from is good and paiored to transport in ableton. pull from
  there for the diamond in other play modes." The "already good" signal
  is `tiles_midi_clock_is_running()` -- already what `render_transport_
  toggle_leds()` drives the "-"/"+"  LEDs from in sequencer mode, a REAL
  reflection of an actual incoming/tap-tempo clock, unlike `s_transport_
  playing` (this device's own belief about what it last told Ableton,
  which can silently drift wrong -- see that flag's own declaration
  comment). `handle_diamond_transport()`'s non-sequencer render now
  checks `tiles_midi_clock_is_running()` instead of `s_transport_
  playing` for the playing/stopped LED levels -- `s_transport_playing`
  itself stays (a click still needs to know whether to send Play or Stop,
  which is about intent, not something a clock signal alone answers),
  it just no longer drives the LED. Sequencer mode's own diamond LED
  used to show only capture mode's pulse or nothing at all -- real
  feedback added a genuine four-state language on top: "pulsing tho if
  ableton is playing but sequence is stopped[,] hold solid only when
  sequencer is playing as well. stop pulse if sequence is paused and
  solid stop if sequence is fully stopped from head and ableton is not
  playing." Mapped onto existing state, no new flags needed: "paused" is
  `s_seq_lane_running[s_seq_edit_lane]` still true while the shared clock
  itself isn't ticking (this lane wants to keep going, just has nothing
  to advance against right now -- exactly what `seq_advance_clock()`'s
  own "leaves that flag alone" comment already describes), "fully
  stopped from head" is that same flag false. Solid play reuses
  `OP_TRANSPORT_LED_PLAYING_LEVEL`, solid stop reuses `OP_TRANSPORT_LED_
  STOPPED_LEVEL`, the "clock going, this lane isn't" pulse reuses
  `background_pattern_pulse_level()` (triangle's own "active elsewhere"
  shape -- a good semantic match), and "paused" reuses the recording
  pulse's exact dim shape (a second copy, not a shared call -- matches
  this file's own established precedent of separate copies over one
  parameterized pulse helper). Capture mode's own pulse still takes
  priority over all four while actually active, checked first.
  Separately confirmed, no change needed: chord mode's velocity request
  from this same message ("chord mode should be velocity sensitive") was
  already correctly wired from the earlier chord-mode redesign --
  `chord_pad_strike()` already sends every voice's Note-On with the real
  computed strike velocity, not a fixed value.
- **The 'x' bisection landed: the freeze is genuinely INSIDE
  `tiles_sk6805_write()`, not after it -- meaning its own 5ms timeout
  isn't actually bounding the wait.** Next live-testing crash after the
  'x' trace shipped. One board's report cut off at exactly `...YLw`
  again -- still no 'x' anywhere, meaning the hang happens strictly
  BETWEEN the 'w' trace and the 'x' trace, i.e. inside `drivers/
  sk6805.c`'s `tiles_sk6805_write()` itself, on the underglow path
  (4-pixel write) specifically, same as the previous capture. This
  matters a lot: `sk6805_put_blocking_with_timeout()`'s own wait loop
  checks `time_reached(deadline)` every spin, bounded to 5ms -- for it
  to still be stuck when the FULL 1000ms watchdog window expires, either
  that timer check itself isn't running (execution never gets back to
  it), or something is stalling at a lower level than software can
  intervene at, most plausibly a hung bus transaction reading/writing
  the PIO peripheral's own registers directly -- a genuine hardware-
  level stall would explain why a correctly-written software timeout
  can't help: the CPU can't execute the NEXT instruction (including the
  timeout check) until that transaction resolves, and if it never does,
  neither does anything downstream of it. Checked and ruled out as
  contributing factors: no other PIO consumer anywhere in this firmware
  (`s_pad_chain`/`s_underglow_chain` are the only two `pio0` state
  machines that exist, confirmed by grep), and no application-registered
  interrupt handler exists either (TinyUSB's own internal USB IRQ is the
  only one, not something this codebase's own code touches directly).
  Added one more layer of resolution since the existing 'w'/'x' pair
  only bisects "before this call" from "after it," not WHERE inside it:
  `tiles_sk6805_write()` (drivers/sk6805.c) now traces 'p' before each
  individual pixel's own write attempt, and 'q' once the per-pixel loop
  fully exits (before the trailing 300us reset/latch `sleep_us()`) --
  counting 'p's in the next report pins down exactly which pixel index
  it dies on, and whether 'q' shows up at all separates "stuck inside
  the PIO wait" from "stuck in the latch delay instead." This is a
  deliberate, temporary exception to drivers/ never depending on
  services/ (see the new include's own comment in sk6805.c) --
  restructuring the caller to push pixels one at a time instead would
  have corrupted the reset/latch timing (that delay must run once, after
  the LAST pixel, not after each one), so reaching into the driver
  directly was the only way to get this resolution without a real
  behavior change alongside it.
  Separately, and operationally important: THE SAME test round's OTHER
  board's crash report showed NEITHER 'x' NOR the underlying 'w'/'i'
  pattern change at all -- its ring looked exactly like reports from
  BEFORE the 'x' trace shipped, strongly suggesting that whichever
  physical board answered to bus 2 at flash time is not the same
  physical board that answered to bus 2 during this test (macOS/
  picotool bus/address numbers are not guaranteed stable across a
  replug, and real feedback separately reported the boards being
  unplugged/replugged around this same test round while chasing a
  still-open, separate lead: "everytime we plug in tiles it crashes and
  [affects] the novation. its like on command when i plug in tiles" --
  a DIFFERENT signature than this entry's multi-minute-uptime freeze,
  not yet investigated). Worth explicitly reconfirming which
  physical unit is which before trusting bus-number-based flashing
  again, rather than assuming bus 2 now means what it meant a flash or
  two ago.
- **Correction to this session's own earlier finding: "Ableton playing"
  is not actually a strict trigger.** The original isolation, much
  earlier in this file, reads as unconditional: "boards dont crash when
  ableton is not playing and not sending clock." Real feedback now,
  after more testing: nothing is MIDI-mapped to Ableton's Tap Tempo
  (ruling out the specific mechanism the CoreMIDI/CoreAudio deadlock
  research lead pointed at), and "the fail does happen without ableton
  playing but it did appear to be more recurrent [with it] either way
  we can be doubtfull about that specific trigger." Consistent with
  where the bisection trace has actually been pointing (a hardware-level
  PIO/bus stall inside `tiles_sk6805_write()`, not anything MIDI-clock-
  specific) -- a timing-sensitive hardware stall wouldn't need Ableton's
  clock at all, just enough sustained activity to land in whatever
  window triggers it, which naturally correlates with heavier play
  without being caused by it specifically. Treat "Ableton playing"
  as correlated with FREQUENCY, not as a precondition, in any future
  reasoning about this -- and don't over-trust the original quote above
  as still-accurate just because it's written down.
- **Found it (real confidence, not a guess this time): `sleep_us()` in
  `tiles_sk6805_write()` was never actually a busy-wait, despite this
  file's own header comment claiming it was.** The 'p'/'q' trace from
  the previous entry got its first real hit: the next live-testing crash
  cut off with all 4 underglow pixels' 'p' present, 'q' present (the
  per-pixel loop had fully exited), then nothing -- no caller-side 'x'.
  That bisects the freeze to exactly one call: the trailing `sleep_us(
  TILES_SK6805_RESET_LOW_US)` (300us) right after the loop, previously
  assumed to be a simple register-polling delay the same way the PIO
  wait just above it is. Reading pico-sdk's own `pico_time/time.c`
  directly instead of trusting the assumption: `sleep_us()`, for
  anything above `PICO_TIME_SLEEP_OVERHEAD_ADJUST_US` (6us default --
  300us is nowhere near that threshold), calls `sleep_until()`, which
  calls `add_alarm_at()` to schedule a HARDWARE ALARM INTERRUPT and then
  blocks on a spinlock waiting for that alarm's own callback to notify
  it. That's a real, working dependency on the timer/alarm-pool
  interrupt subsystem actually firing -- not the plain `while (timer <
  target) tight_loop_contents();` spin this file's own header comment
  describes and every other timeout in this codebase (the PIO wait
  right above this call, drivers/i2c_bus.h's own 5ms bound) actually is.
  If that notification is ever missed for any reason -- a lost wake-up,
  an alarm queued during another code path's own interrupt-disabled
  window (this file's own pattern-store flash-save entry, much earlier,
  disables interrupts for "tens of milliseconds" doing exactly that,
  right before this same underglow write could plausibly run again) --
  this call blocks forever, and nothing else in the whole call chain
  can catch it: the 5ms-bounded PIO wait already returned by the time
  this runs, and a hang waiting on an interrupt notification has no
  deadline of its own to check.
  Fixed by using what the header comment already claimed was
  happening: `busy_wait_us()` (`hardware/timer.h`, already reachable
  through the existing `pico/time.h` include) polls the raw hardware
  timer counter register directly in a tight loop -- no IRQ, no alarm
  pool, nothing else in the system that could go missing underneath it.
  For a fixed, short, timing-critical latch delay like this one, that
  isn't a workaround bolted on to dodge a bug -- it's the correct
  primitive to have used from the start; `sleep_us()`'s own
  interrupt-based design exists specifically to let OTHER code run (or
  the core sleep/save power) during a longer wait, neither of which
  this 300us LED-protocol delay ever needed or benefited from.
  Not yet proven from a clean next capture -- that's the real test --
  but this is the first fix all session backed by reading the actual
  mechanism start to finish and finding a genuine, concrete gap between
  what the code claimed to do and what it actually depended on, rather
  than another round of "already timeout-bounded, still hangs, unclear
  why."
- **A batch of real feedback while stability testing continued in
  parallel, six items:**
  - **Scale selection already affects both melodic and chord mode, by
    design -- verified, not changed.** "when selecting a global scale
    it should affect the melodic and chord mode so double check that."
    `note_map.c`'s `chord_mode_scale_table()` reads the globally
    selected scale first and uses it as-is whenever it's a genuine
    7-note diatonic scale (Ionian/Dorian/Phrygian/Lydian/Mixolydian/
    Aeolian/Locrian) -- only falls back to Ionian for the other 11
    (chromatic, pentatonic, blues, whole-tone, diminished, etc.),
    deliberately, because chord mode's skip-one/skip-two harmonization
    can't produce a real triad against a scale that isn't 7-note
    diatonic. Not a bug; melodic mode's own `note_for_scale_degree()`
    has no such exception at all.
  - **Opening the scale (or mode) picker while chord mode was active
    let its pads still fire real notes underneath.** "selecting a
    scale should not trigger midi sound when slecting so no midi on
    select scale just menu input." Root cause was chord-mode-specific:
    `tiles_op_mode_owns_pad()`'s chord branch answered purely from
    chord-region membership, never checking whether a menu was ALSO
    open on top -- unlike every other mode, which falls straight
    through to `tiles_op_mode_owns_pad_grid()` and so already correctly
    suppressed new strikes the instant either menu's own visible flag
    went true. `handle_chord_pad_taps()` (chord's separate pipeline,
    bypassing that accessor and services/expression.c's gate entirely)
    never checked menu state either. Both now check menu state first.
  - **Capture mode no longer force-ends whatever was already sounding
    the instant it's entered.** "capture mode should not mute the midi
    notes that are playing underneath it should be additive and real
    time." `seq_capture_mode_enter()` used to call `seq_end_current_
    note()` on entry -- reasoned at the time as "capture mode takes
    over this lane," but in practice an abrupt, audible cutoff the
    moment capture starts, not a smooth "start layering on top of
    what's already going." Removed; whatever was ringing keeps ringing
    and ends on its own normal timing while newly captured content
    layers in from there.
  - **The ambient "sequencer running" pulse (triangle's background
    indicator, and diamond's own new play-pulse state) now beats with
    the actual tempo instead of a fixed 3000ms.** "the pulse for
    sequencer is running should be a bit faster, how about we make it
    match the bpm of clock." New `tiles_midi_clock_get_ms_per_beat()`
    (`services/midi_clock.h`/`.c`) exposes the current tempo from
    whichever source is actually driving the clock -- real external
    Clock bytes now get their OWN beat-to-beat interval measurement
    (they never carried a tempo value of their own before, just "another
    pulse happened"), or tap tempo's already-averaged interval, or a
    500ms (120bpm-equivalent) placeholder before either is ever
    established. `background_pattern_pulse_level()` reads this as its
    period instead of a fixed constant.
  - **Quantized starts always waited for the NEXT beat, even landing
    one pulse past the last one.** "quantize is off, its always
    waiting for the next beat, it should measure if it can snap to the
    last beat as well so its accuarte similar to how other devices do
    it." Same nearest-boundary measurement `seq_capture_handle_taps()`
    already used for which STEP a captured note targets (its own
    "Nearest-step quantization," predating this fix) now also applies
    to which BEAT a pending sequencer/capture start resolves against,
    in both `seq_advance_clock()` and `seq_capture_advance_clock()`:
    past the halfway point of the current beat, still waits for the
    next one; within the first half, snaps to the one that just passed
    and starts immediately instead of sitting through most of a beat
    of dead air first.
  - **A step's remembered pitch was getting silently discarded every
    time it was turned back on.** Real feedback, a separate message in
    the same testing round: "when steps are turned off they are not
    saving the asigned pitch. they should always save the pitch they
    lasrt had when on in case of retrigger." `step_pitch_override[]`/
    `step_note[]` were never actually cleared when a step turned off --
    that data already survived untouched -- but the plain tap-to-arm
    toggle unconditionally re-resolved and overwrote it on every re-arm
    regardless, discarding whatever pitch the step remembered from
    before. Now only resolves+freezes a fresh pitch the first time a
    step is EVER armed (no override yet at all); an already-programmed
    step keeps its pitch across as many off/on toggles as it goes
    through. The pitch-edit view (hold a step) remains the deliberate
    way to actually change an already-frozen step's pitch; capture
    mode's own commit is unaffected -- playing a step live is
    intentionally always "what you just played," not frozen history.
- **A 16-step pattern now snaps its step grid to a clean 4x4 in the
  top-left instead of spilling awkwardly across 2.67 rows of 6.** Real
  feedback: "if the sequenfcer is reduced to 16 steps then auto align
  the layout of the steps to the left meaning a 4x4 grid[,] anything
  else still ads or reduces steps in the curent full layout. thats a
  signle snap layout change." Exactly one special case, at length ==
  16 -- every other length keeps the existing plain linear pad==step+1
  mapping, filling however many of the 6 columns per row it happens to
  (a partial last row when it doesn't divide evenly, same as always).
  Three new small helpers (`seq_uses_4x4_layout()`/`seq_pad_for_step()`/
  `seq_step_for_pad()`, each taking the pattern explicitly rather than
  assuming the viewed lane -- `seq_fire_note()` needs a background
  lane's OWN length, not necessarily s_seq_edit_lane's) are now the only
  place that decides step<->pad, replacing every direct `pad-1u`/
  `step+1u` computation across rendering (`render_sequencer()`,
  `render_seq_capture()`, `render_pitch_edit()`), touch handling
  (`seq_handle_step_taps()`, `handle_edit_mode()`'s edit_pad), the
  playback engine's own live-note-fallback and haptic-kick pad
  (`seq_fire_note()`), and the three "resync touch tracking so a
  still-touched pad doesn't misread as fresh" loops (`edit_exit()`,
  `seq_start()`, `pattern_bank_exit()`) that used to assume pad number
  and array index were always the same value. Caught auditing this
  before it ever shipped: `seq_pad_for_step()` guards `step < 16u`
  explicitly -- a couple of those resync loops walk all 24 steps
  unconditionally regardless of length, and without the guard, a step
  in the 16-23 range would compute a nonexistent row 5 with no bounds
  checking at all, handing an invalid pad number (25+) to a caller that
  trusts 1..24. Since `render_sequencer()` reads the pattern's length
  fresh every single frame, the layout snaps immediately the instant
  length crosses to or from 16 -- no separate "on length change"
  trigger needed at all.
- **First real validation of the `sleep_us()` -> `busy_wait_us()` fix:
  hours of active use, across two further flashes on top of it, zero
  new crash reports.** Real feedback: "there hasnt been a crash in a
  bit since you implemented that fix its been hours and a new flash
  even." Checked against the actual log rather than taken on trust
  alone: the newest crash-report offset is still the exact same one
  that led to the fix in the first place -- nothing new since, through
  ~77MB of subsequent healthy trace activity and two more reflashes (the
  16-step 4x4 layout change included, itself a real, separate edit to
  the same file). Not proof it's gone for good -- every prior fix this
  session that LOOKED like progress (I2C timeouts, the SK6805 PIO
  timeout, the crash-report snapshot timing) also survived some amount
  of testing before the next real data point complicated the picture --
  but this is the longest clean stretch since the investigation began,
  on the fix most directly backed by reading the actual mechanism start
  to finish rather than a bounded-but-still-hanging guess. Worth
  treating as genuinely promising, not yet as closed.
- **Sequencer steps can hold more than one note now -- capped at 2,
  not because 2 is the musically right number, but because 2 is the
  actual flash-capacity ceiling.** Real feedback: "sequencer real time
  and note select should allow for multiple notes per step so if i
  play a cluster of notes we should be able to save those in that
  single step." `op_seq_pattern_t`'s `step_note[]` (one note) became
  `step_notes[][2]` + `step_note_count[]`, touching every layer: the
  playback engine (`seq_fire_note()`/`seq_end_current_note()` now loop
  over a lane's whole cluster, one Note-On/Off per note but still one
  haptic kick per STEP, not per note -- a chord is one physical
  strike), capture mode (a NEW touch now sounds ALONGSIDE whatever's
  already held instead of cutting it off first -- `seq_capture_end_
  sounding_note()` split into `_end_one_sounding_note(pad)`/`_end_all_
  sounding_notes()` so lifting one finger only ends that one note),
  and the manual per-step pitch picker. That last one needed its own
  small interaction rewrite: a plain single-tap-commits-and-closes
  gesture can't build a multi-note chord, but real feedback had
  ALREADY explicitly rejected a hold-to-close gesture ("it should be a
  toggle... not a momentary thing") for the original single-note
  version. Resolved by repurposing a gesture this view's own header
  comment already anticipated as a harmless no-op -- tapping the
  step's OWN pad again -- into the explicit close a cluster genuinely
  needs; tapping any OTHER pad now adds it to (or, if already present,
  removes it from) a growing cluster instead, closing nothing.
  The flash-capacity story is the real story here, and worth being
  honest about: `tiles_pattern_store_t` (all 24 patterns across 4
  lanes x 6 alternatives) has to fit in exactly one 4096-byte flash
  sector -- the erase+program sequence runs with interrupts disabled
  and nothing able to pet the watchdog partway through (flash-resident
  code, including `watchdog_update()` itself, can't execute while
  flash is mid-erase/program), so a bigger region risks that whole
  window exceeding the watchdog timeout on a slow chip, not something
  to gamble on mid-way through a session about hunting exactly this
  class of timing bug. Packed the store's own `slot_saved` bool[4][6]
  down to a bitmask specifically to claw back header room, and even
  then, 2 notes per step is the actual largest value that fits -- 3
  overflows by 572 bytes, confirmed empirically. A `_Static_assert`
  right after the store struct now makes any future overflow a hard
  compile error instead of the compiler warning that was the only
  thing catching THIS one while building the feature.
  `TILES_PATTERN_STORE_VERSION` bumped to 2 for the layout change --
  existing saved patterns are lost across this specific update, not
  corrupted, same safe fallback a first-ever boot already gets.
- **Real feedback: "for concistency i wanna swap in sequencer mode the
  diamond with shift to capture and the diamond alone to pattern
  selectror."** Shift+diamond now means capture, plain diamond means
  the pattern bank -- the reverse of before. Motivation stated
  directly: consistency with a NEW cross-mode "capture into lane 3"
  feature, requested in this same testing round and not yet built as
  of this entry, which will also use shift+diamond -- one gesture, one
  meaning, everywhere, rather than capture meaning shift+diamond
  outside sequencer mode but plain diamond inside it.
- **New feature: capture into lane 3 from melodic, chord, or guitar
  mode, without leaving that mode.** Real feedback: "i also want to add
  a feature that captures from melodic mode or chord mode or any mode
  into lane 3 sequencer on command and each new capture from each mode
  goes into a different bank of lane 3 effectively making it possible
  to run multiple sequences for each lane at once... for trigger
  capture mode lets use a push of shift and diamond if not in use
  already by another function. this will make the steps start counting
  like in sequencer flashing under the current layout and the playing
  gets saved." Shift+diamond (free to claim outside sequencer mode --
  it used to just fall through to the same play/stop/record toggle a
  plain click already does, never its own distinct gesture) toggles
  this on; the current mode's pad grid stays exactly as it already
  renders, untouched, with a new amber underglow pulse (services/
  lighting.c's write_cross_capture_underglow(), same "bypass standby
  ownership, write straight to hardware" pattern the crash/debug
  indicators already use, since melodic/chord/guitar mode never claim
  standby_active in the first place) the only visible sign anything
  changed.
  Deliberately built almost entirely out of REUSE rather than a
  parallel system: cross_capture_enter()/_exit() just save/repoint
  s_seq_edit_lane at lane 3 (index 2 -- "3 is now capture" per this
  same message's own lane-numbering reminder) and set that lane's
  active alt to a fixed per-mode bank (melodic->0, chord->1, guitar->2
  -- "each new capture from each mode goes into a different bank", a
  fixed mapping so re-capturing from the same mode later deliberately
  overwrites that same bank rather than accumulating endlessly), then
  call the EXACT same seq_capture_mode_enter()/seq_capture_handle_taps()/
  seq_capture_advance_clock() this file already has for sequencer mode's
  own plain-diamond capture -- every one of those already reads s_seq_
  edit_lane/active_pattern() internally, so none of them needed to
  change to work here too, and lane 3's own playback afterward is just
  the existing "every lane runs in the background regardless of mode"
  behavior already established, not new code at all.
  What genuinely IS new: tiles_op_mode_scan()'s own dispatch runs
  capture's scan functions but, when this is the cross-mode variant,
  deliberately does NOT call render_seq_capture() or return early the
  way sequencer mode's own capture does -- control falls through to
  the current mode's own normal rendering instead, unmodified.
  Two real correctness gaps caught auditing this before it ever
  shipped, not found by testing: first, a defensive check elsewhere in
  this file already existed for "exit capture mode if shift+triangle
  opens the scale picker on top of it" -- calling the bare seq_capture_
  mode_exit() there (and in set_active_mode()'s own mode-change safety
  net) would have left s_seq_edit_lane and a new is-cross-capture-active
  flag stuck pointing at lane 3 forever, since only cross_capture_exit()
  itself clears those; both sites now check which variant is active and
  call the right one. Second, and more serious: chord mode's own pads
  never resolve notes through tiles_note_map_get_note() at all (they
  bypass it entirely for build_chord_voicing()'s own 4-voice output --
  see that function's own section) -- without an explicit check,
  capturing from chord mode would have silently recorded whatever
  melodic note that pad's position implies instead of an actual chord,
  defeating the entire reason chord mode is one of this feature's own
  named capture sources. seq_capture_handle_taps() now detects a chord-
  region pad while chord mode is active and captures build_chord_
  voicing()'s first 2 voices (bass + root -- the two most foundational,
  and OP_SEQ_MAX_NOTES_PER_STEP's own flash-capacity cap leaves no room
  for the full 4 anyway) as two notes from that one touch, instead of
  the single plain-melodic note it would otherwise have captured.
  Genuinely new to real playing time as of this change, built on top of
  the multi-note-per-step work above in the same pass -- worth watching
  closely on the next real-hardware round, same as everything else
  reintroduced or newly built this session.
- **Two LED fixes, both real feedback caught right after the previous
  round shipped:**
  - **The pattern save/delete confirmation's underglow half was never
    actually reaching hardware, for almost this entire session.** "the
    led indication is not working there is no underglow and no pad
    flash confirmation either" (the pad half turned out fine on
    inspection -- likely just hard to notice with a finger physically
    covering the one pad that's flashing during the gesture itself).
    Root cause, found by re-tracing `tiles_lighting_service()`'s own
    priority chain rather than the save/delete logic itself (which was
    already correct): debug mode's own magenta underglow override --
    armed for nearly this entire session specifically to catch crash
    reports -- unconditionally owns underglow whenever it's on, and the
    plain `write_underglow()` path this confirmation normally shows
    through never got a chance to run underneath it. New `tiles_op_
    mode_pattern_flash_underglow_color()` (`services/op_mode.h`/`.c`)
    exposes the confirmation's already-resolved color (reusing
    `render_pattern_bank()`'s own two-blink timing, not a second copy of
    that math), checked in the priority chain ABOVE debug mode -- a
    brief (600ms), directly-caused-by-what-you-just-did confirmation
    outranks an ambient "recording is on" pulse for that short window.
  - **Moved the four-state transport indicator off diamond, onto "-"/
    "+".** "the +- transport controls shoudl be the ones with the
    flashing logic while in sequencer mode." Diamond's role in
    sequencer mode is triggering capture/pattern-bank access now (this
    same session's earlier diamond/shift swap), not passively
    displaying transport state -- it reverted to a plain "capture pulse
    while active, dark otherwise" indicator, the same shape the
    pattern-bank branch right next to it already uses. The four states
    (solid/pulse x play/stop) moved to `render_sequencer()`'s own "-"/
    "+" LEDs and `render_transport_toggle_leds()` (shared by the pitch/
    probability/ratchet edit views, needed a new `now_ms` parameter to
    carry the pulse timing) instead: "+" keeps its established "lit
    means running" role (solid when this lane's genuinely playing,
    pulsing when the clock's going but this lane isn't part of it yet),
    "-" keeps "lit means stopped" (solid when fully stopped, pulsing
    when this lane wants to run but has no clock to advance against --
    nothing's actually audible in that state either, matching "-"'s own
    stopped-ish role).
- **In-sequencer capture mode was recording nothing at all, despite
  sounding like it worked while you played.** Real feedback: "the live
  capture mode within the sequencer mode is still not sending the midi
  signals, it is capturing but its not live playing looping." Root
  cause: `seq_capture_advance_clock()` (the function that actually
  commits a captured note into the pattern once its target step's
  boundary arrives) used to early-return on `!clock.running`, mirroring
  `seq_advance_clock()`'s own identical check -- correct THERE (an
  already-recorded pattern genuinely should pause when the transport
  stops) but wrong here, where it silently blocked the commit on every
  single scan whenever real clock bytes were arriving but `tiles_midi_
  clock_is_running()` itself still read false. That's exactly what
  happens whenever `seq_capture_mode_enter()`'s own `tiles_midi_clock_
  set_running(true)` call turns out to be a no-op -- `services/midi_
  clock.c`'s own "real clock always wins" guard against a live external
  source, regardless of whether THAT source's transport is actually
  playing yet. Live-preview notes in `seq_capture_handle_taps()` fire
  purely from touch events, completely independent of any of this,
  which is exactly why capturing could sound like it was working (you
  hear yourself playing) while nothing ever actually landed in the
  pattern. Fixed by removing the check from this function specifically
  -- `clock.pulse_count` itself keeps advancing on every real Clock
  byte regardless of `clock.running` (confirmed reading `services/
  midi_clock.c`'s own `MIDI_REALTIME_CLOCK` case directly, not
  assumed), and capture only ever needs that raw progression for its
  own quantization, never a belief about whether the DAW considers
  itself "playing."
- **The pattern save/delete underglow flash still didn't show, even
  after the previous round's debug-mode-priority fix.** Real feedback:
  "the save pattern and dleete patter still do not do the pulse
  underglow," re-confirming the exact gesture is otherwise right (enter
  the bank via diamond in sequencer mode; shift+tap-and-release a cell
  to save it, green flash twice on the pad and underglow; shift+hold
  the same cell 3 seconds to delete it, red flash twice). The previous
  fix (giving `tiles_op_mode_pattern_flash_underglow_color()` priority
  over debug mode inside `tiles_lighting_service()`) was necessary but
  not sufficient -- it fixed the priority ORDER of the one writer that
  goes through that function, but missed that `render_pattern_bank()`
  itself was ALSO writing the flash color straight to hardware, on its
  own, every single `tiles_op_mode_scan()` call (via `tiles_lighting_
  set_standby_underglow_rgb()`'s own immediate write-on-change). `main.c`
  calls `tiles_lighting_service()` again right after every scan, and
  with debug mode armed for nearly this entire session, its own
  override wrote a few instructions later, every iteration, unconditionally
  clobbering whatever `render_pattern_bank()` had just put on the strip
  before a single frame of it could ever reach anyone's eyes. Pads never
  had this problem because nothing else competes for them the way crash/
  debug/pattern-flash/cross-capture all fight over underglow. Fixed by
  making `render_pattern_bank()`'s own underglow loop stop writing the
  flash color at all -- it now only ever writes the plain off/idle case,
  and defers the flash entirely to `tiles_lighting_service()`'s own
  priority chain, so there is exactly one writer for that state and
  nothing left to race.
- **"-"'s own solid/pulse split, corrected: it now reflects step
  position, not `lane_running`.** Real feedback: "when the sequence is
  stipped but not brought back to the start make the - pulse." The
  previous round's four-state "-"/"+" LED logic used `s_seq_lane_
  running[lane]` to distinguish "-"'s solid case from its pulsing case
  -- but `handle_transport_and_length()`'s own double-stop gesture
  (first "-" press pauses in place, leaving `s_seq_current_step`
  untouched; a SECOND press while already stopped is what rewinds it to
  0) sets `lane_running` false in BOTH cases, so that condition could
  never actually tell "paused mid-pattern" apart from "stopped at the
  head" -- it was testing a variable that's already false either way.
  Fixed by keying the split on `s_seq_current_step[s_seq_edit_lane]`
  instead: solid when stopped AND at step 0, pulsing when stopped but
  the playhead is still sitting wherever the first "-" press left it.
  Applied to both copies of this logic (`render_sequencer()`'s own
  inline "-"/"+" LEDs and the shared `render_transport_toggle_leds()`
  used by the pitch/probability/ratchet edit views) for consistency.
- **Pattern save/delete underglow (and pad flash): the real root cause,
  found via the diagnostic prints above.** Real feedback after two
  rounds of fixes that didn't help: "i did two saves and two delete but
  no light confirmations on either ... but when i did it without debug
  mode there was no underglow at all either" -- the "without debug
  mode" half was the key data point, since it ruled out any debug-mode
  race once and for all (nothing else was active to compete with it,
  and it still didn't show). The diagnostic prints confirmed it: `[op_
  mode] saved lane N pattern N to flash` fired every time (the trigger
  was never the problem), but `render_pattern_bank()`'s own "pattern
  flash showing" print never fired ONCE, and `tiles_lighting_service()`'s
  override never transitioned to `2` (pattern_flash) either -- so
  `s_pattern_flash_active` was reading false to EVERY consumer, despite
  being set true moments earlier. Root cause: `render_pattern_bank(uint32_t
  now_ms)` takes `now_ms` as a PARAMETER, captured once at the top of
  `tiles_op_mode_scan()` -- before `handle_pattern_bank_taps()` (called
  earlier in that same scan) runs `pattern_store_write_all()`, the flash
  erase/program with interrupts disabled that always takes some real,
  nonzero wall-clock time. `s_pattern_flash_start_ms` is captured via a
  FRESH `get_absolute_time()` call AFTER that write returns, so it's
  always >= the stale `now_ms` this function was handed. `now_ms -
  s_pattern_flash_start_ms` (both `uint32_t`) underflowed to a huge
  number on the very FIRST check every single time, tripping the expiry
  branch immediately and clearing `s_pattern_flash_active` before a
  single frame could ever reach the pad, the underglow, or `tiles_
  lighting_service()`'s getter -- which is exactly why the previous
  two fixes (debug-mode priority, removing the redundant underglow
  writer) were both real but insufficient: they fixed how the state
  would have been CONSUMED, while the state itself was already dead on
  arrival. Fixed by capturing a fresh timestamp for this one check
  instead of trusting the passed-in parameter, exactly like `tiles_op_
  mode_pattern_flash_underglow_color()` already correctly did -- which
  is also why that getter's OWN math was never the suspect here; it was
  never the one racing against the flash write. The two diagnostic
  prints stay in permanently (cheap, transition-only, and this is
  exactly the kind of bug they're for).
- **Game mode's entry combo was permanently blocked for as long as
  sequencer happened to be the displayed mode.** Real feedback: "Cant
  access game mode anymore, probably because of button confirmation
  combo logic for debug mode, that logic should be progressive for all
  combo types not just debug." `game_mode.c`'s own `gm_combo_held()`
  guarded its 4-button (triangle+diamond+square+circle) entry combo
  with `tiles_op_mode_owns_pad_grid()`, which answers true for the
  ENTIRE time sequencer is simply the active/displayed mode, sub-view
  open or not -- not just while a genuine sub-view (the mode menu,
  scale menu, pattern bank, per-step edit, capture mode) is actually
  showing. `services/debug_mode.c`'s own combo, by contrast, has no
  such gate at all -- it just trusts its own 8-second hold timer,
  regardless of whatever mode is displayed, and that's held up fine
  across this entire session's testing. None of game mode's four
  buttons collide with anything sequencer's own plain step-view uses
  them for on a sustained 700ms hold (only quick clicks: triangle opens
  the mode menu, diamond toggles capture/pattern-bank), so the real
  accidental-trigger risk the original guard was written for only ever
  applied to the actual sub-views. Narrowed `gm_combo_held()`'s guard
  to `tiles_op_mode_has_menu_open()` instead -- the same accessor
  standby's idle timeout already uses for exactly this "which sub-views
  genuinely need protecting" question -- so the combo now works
  whenever the sequencer's plain grid is showing, matching debug mode's
  own "just trust the hold timer" posture, while still blocking during
  the sub-views that actually need it. Deliberately did NOT extend this
  to `octave_control.c`'s own "-"/"+" transpose-combo guard even though
  the same real feedback calls for "all combo types" -- that guard is
  broader on purpose (also blocks during guitar mode, where "-"/"+"
  are already individually live for fret control), and unlike game
  mode's four buttons, "-"/"+" ARE already individually meaningful in
  sequencer's own plain step-view (play/stop/rewind, length change) --
  narrowing it the same way would risk the transpose combo firing by
  accident during ordinary sequencer transport use, a real regression
  this fix doesn't need to risk to fix the one thing that broke.
- **Cross-mode capture (melodic/chord/guitar) fixes, three issues from
  one real-feedback message: "when capture is triggered shift becomes
  tap tempo and we can see the sequencer running automatically under
  the melodic layout and it loops live playing what was recorded on
  the previous pass without erasing what was already written."**
  Follow-up answers narrowed each one down:
  - **Previously-recorded content was silent while capturing, only
    audible again once capture ended.** "its not audible until capture
    is off" -- confirmed by reading `seq_capture_advance_clock()`: it
    only ever committed a NEWLY touched note into the step just left
    and advanced `s_seq_current_step[lane]` directly, never calling
    anything that would fire a note for whatever was ALREADY armed at
    the step being entered. Correct on the "doesn't erase" half (an
    earlier round's own fix), silent on the "loops live playing" half.
    `seq_advance_clock()` (every OTHER lane's own normal playback) uses
    `seq_enter_step()` for exactly this -- ends the current note,
    advances the step, and fires whatever's armed there (probability/
    ratchet included) -- so `seq_capture_advance_clock()` now calls
    that same function instead of assigning the step directly. A
    capture pass now sounds like layering a new take over the loop
    that's actually playing, not silence with only your own new notes
    floating on top of it.
  - **The sequencer becoming visible on the pad LEDs, not just
    underglow.** "im asking for the sequencer to be visible on the pads
    on the leds" -- the amber underglow pulse (built earlier this
    session) wasn't enough on its own. New `tiles_op_mode_cross_
    capture_is_note_sounding(uint8_t note)` (op_mode.c/.h) reads the
    exact same `s_seq_sounding_notes[]`/`s_seq_note_sounding[]` state
    `seq_end_current_note()` itself uses -- a read-only peek at what's
    genuinely sounding, not a separate tracked copy. `services/
    lighting.c`'s `pad_desired_rgb()` checks this, per pad, against
    that pad's own currently-mapped note (`tiles_note_map_get_note()`),
    ahead of guitar/chord-region/melodic idle coloring but AFTER the
    active-touch check -- so whichever pad the loop is currently
    playing flashes amber right on the melodic/guitar grid, without
    ever overriding a pad you're actually touching. Chord-region pads
    are excluded (they don't resolve through `tiles_note_map_get_note()`
    at all, so checking it there risks a coincidental, meaningless
    match) -- this can only ever highlight melody-region/guitar-neck
    pads, matching where the doc comment says so.
  - **"Shift becomes tap tempo" while capture is on -- turned out to be
    a feature request, not a bug report.** The diagnostic print added
    to chase this as a bug never had anything to catch: real feedback
    corrected the misread -- "youre missunderstanding tap tempo, thats
    a feature i want in melodic modes inspired by the sequencer but i
    only want it active when captuire mode is active. so reach into
    sequencer mode and copy that feature into the other modes but only
    activate it when the capture mode is on." `handle_circle_tap()`'s
    own `mode_ok` (tap-tempo candidacy) widened from `s_active_mode ==
    OP_MODE_SEQUENCER` to also allow `s_cross_capture_active` --
    that flag is only ever true outside sequencer mode in the first
    place (see `set_active_mode()`'s own defensive exit), so this
    doesn't touch sequencer mode's own existing rule at all, it just
    adds the one specific state that was asked for. Cross-capture
    already REQUIRES a tempo to exist before it can even be entered
    (`cross_capture_enter()`'s own gate), but until this fix there was
    no way to tap a NEW one once you were actually in melodic/chord/
    guitar mode with it running -- shift's tap-tempo role was
    sequencer-only, full stop. The diagnostic print itself was removed
    (nothing left to diagnose); the combo-conflict cancellation that
    already protects every other tap-tempo press from being confused
    with a combo (diamond/triangle/square joining mid-hold) applies
    here unchanged, so the shift+diamond gesture that ENTERS cross-
    capture still can't also register as its own tap.
- **Cross-capture step guide on the pad grid, plus confirmation of the
  previous two fixes.** Real feedback: "on boot capture mode is not
  working, the shift diamond combo dosnt do shit, it requeres
  sequencer to be starter at least once meaning tempo initialized.
  after tjhat it does work and nown we do have the loop playing
  inmendiately but i still need the guide curent step on light
  visible, we're missing that still." Three things in there:
  - The boot-time gap was by design, not a bug -- real feedback asked
    for it removed anyway, then explicitly rejected the FIRST attempt
    at that: a round that made shift+diamond register a plain tap-
    tempo tap whenever no tempo existed yet (so repeating the gesture a
    few times would bootstrap one). "no dumb shit. i dont need shit
    diamond to register tap tempo, delete that, i need shift diamond
    to enter capture and once in capture we can start playing it by
    tap tempo with the shift button only like in the sequencer." Both
    that workaround AND the original tempo-exists gate are gone now --
    `cross_capture_enter()`'s call site in `handle_diamond_transport()`
    calls it unconditionally on a fresh shift+diamond release, no
    check at all. With no tempo yet, `seq_capture_advance_clock()`
    simply sits pending (`s_seq_pending_start` stays true, nothing
    commits or loops) while live touches still sound normally through
    `seq_capture_handle_taps()` -- the exact same inert-but-harmless
    state entering sequencer mode itself with no tempo already
    tolerates, not a new failure mode. Once inside, plain shift alone
    taps out a tempo the same way it always has in sequencer mode --
    `handle_circle_tap()`'s own `mode_ok` already treats `s_cross_
    capture_active` as sequencer-equivalent for tap-tempo purposes (an
    earlier round in this same session), so nothing there needed to
    change again.
  - "the loop playing inmendiately" confirms the previous round's real
    fix (`seq_capture_advance_clock()` now routing through `seq_enter_
    step()`) actually works on real hardware, not just on paper.
  - The actual ask: `tiles_op_mode_cross_capture_is_note_sounding()`
    from that same previous round only ever lit up while something was
    both armed AND audibly sounding -- a silent step (nothing recorded
    there) showed nothing, so there was no visible sense of the
    playhead actually moving through the pattern between hits, unlike
    sequencer mode's own step-view, which real feedback already
    established needs "cuentet stept to be lit up always." New `tiles_
    op_mode_cross_capture_current_step_pad(uint8_t *out_pad)` (op_mode.c/
    .h) returns true on EVERY step, armed or not, via `seq_pad_for_
    step()` -- the exact function the sequencer's own step-view uses for
    step<->pad mapping, 16-step 4x4 remap included -- applied to the
    cross-capture lane's own current step and pattern. `services/
    lighting.c`'s `pad_desired_rgb()` shows this as a dim (0.15 level,
    matching `OP_SEQ_CURSOR_LEVEL`'s own established "unarmed cursor"
    brightness) amber marker, checked right after the brighter note-
    sounding flash so an actually-sounding pad always wins if the two
    ever land on the same one.
- **Cross-capture step marker brightness, and the real chord-capture
  bug: wrong velocity (fixed), incomplete voicings (a hard ceiling,
  not a bug).** Real feedback: "need the sequencer marquer to be
  slightly brighter in capture mode, aditionally we need to debug the
  chord capture, its not capturing exactly whats being performed wioth
  chords, its having lots of issues like incomoplete voicings and
  wrong velocity."
  - The dim step-position marker added last round bumped from (0.15,
    0.09, 0) to (0.35, 0.21, 0) -- same hue, brighter.
  - "wrong velocity" was a real bug: `seq_capture_handle_taps()`'s
    chord-region branch used to resolve its own bass+root straight
    from `tiles_note_map_get_chord_notes()`/`build_chord_voicing()` on
    the RAW touch-down edge -- before `handle_chord_pad_taps()` (the
    function that actually PLAYS that pad, on `services/expression.c`'s
    own measured-strike timing, not raw touch-down) had measured a
    strike or decided a velocity at all. Firing that early meant every
    captured chord used a flat `OP_SEQ_VELOCITY` guess, never the real
    one -- a soft chord and a hard chord captured identically. Fixed by
    triggering on `s_chord_pad_sounding[]` going true instead (the
    moment `handle_chord_pad_taps()` itself actually fires that pad),
    reading its already-resolved `s_chord_pad_notes[]` and a new
    `s_chord_pad_last_velocity[]` (set inside `chord_pad_strike()`, the
    one and only place a chord pad's velocity is ever decided) instead
    of re-deriving anything. New `s_seq_capture_prev_chord_sounding[]`
    tracks that edge the same way `s_seq_capture_prev_pad_touched[]`
    already tracks the raw one, reset on capture entry the same way too
    (so a chord pad already sounding when capture starts doesn't
    retroactively read as a fresh strike).
  - "incomplete voicings" is a hard ceiling, not a bug: `OP_SEQ_MAX_
    NOTES_PER_STEP` is capped at 2 by real flash capacity (see that
    constant's own comment, from earlier this session's own empirical
    N=1/2/3/4/6 testing), so a real 4-voice chord (bass/root/fifth/
    open-third) can only ever keep 2 of them in a single step -- still
    bass+root, the same "two most foundational" choice as before.
    Raising the cap isn't safe to revisit without redoing that same
    capacity work; left unchanged.
- **Steps now capture the full 4-voice chord, not just bass+root --
  the flash-capacity ceiling from earlier this session got reworked,
  not lifted.** Real feedback rejected the "hard limit" explanation
  outright: "i told you steps should be able to capture chords, can
  you rework it to 4 voices max per step?" `OP_SEQ_MAX_NOTES_PER_STEP`
  really was capped at 2 by the pattern store's one-sector budget (3
  confirmed, empirically, to overflow by 572 bytes) -- but that budget
  had more give left in it than the earlier round found, once the SAME
  packing trick already used for `slot_saved_mask` (a bitmask instead
  of a `bool[4][6]`) got applied further. New `tiles_pattern_flash_t`
  is the on-flash layout `pattern_store_write_all()`/`_load_all()`
  actually read and write now (via new `pack_pattern_to_flash()`/
  `unpack_pattern_from_flash()`), separate from `op_seq_pattern_t`
  itself -- every one of the hundreds of call sites that already read/
  write that struct directly needed zero changes, exactly the same
  "pack only at the storage boundary" precedent `slot_saved_mask`
  already established. Two things changed in the on-flash copy only:
  `step_armed[]`/`step_pitch_override[]` (24 bytes each as `bool[24]`)
  each become a 4-byte bitmask, and the separate `step_note_count[]`
  byte-per-step array is dropped entirely in favor of a `0xFF` sentinel
  marking an unused slot in `step_notes[][]` (0-127 covers every real
  MIDI note, so `0xFF` is never ambiguous). Measured, not estimated:
  `tiles_pattern_flash_t` is 156 bytes/pattern now; the whole store
  (`24 patterns * 156 + 12-byte header`) is 3756 bytes, 340 bytes under
  the 4096 budget -- smaller than the OLD 2-notes-per-step design was
  (4092 bytes, a 4-byte margin), despite doubling the note capacity.
  `TILES_PATTERN_STORE_VERSION` bumped 2->3 (a real layout change, same
  "existing saved patterns are lost, not corrupted" precedent as the
  1->2 bump). `seq_capture_handle_taps()`'s own chord-region branch
  needed one small follow-up: it now takes `min(OP_SEQ_MAX_NOTES_PER_
  STEP, OP_CHORD_NUM_VOICES)` instead of assuming they're equal, so it
  can't silently read past `s_chord_pad_notes[]`'s own `OP_CHORD_NUM_
  VOICES`-wide rows if either constant ever changes again -- with both
  at 4 right now, every real chord voice fits in a single step.
- **Song mode (new 5th top-level mode) -- foundation pass: data model,
  flash storage, channel reservation, menu integration. No rendering
  or gestures yet.** Real feedback: "lets implement another sequencer
  mode know as song mode as the default capture modes instead of
  regular sequencer... this operates like ableton live scene trigger
  or session view meaning we can assign a costume midi channel for
  each bank kinda like a looper." A long round of follow-up questions
  (asked because real feedback explicitly requested it: "ask as many
  questions as necesarely so no ambiguity") settled the actual shape,
  including two real technical conflicts worth recording since they
  shaped the design directly:
  - **MIDI channel budget.** Of the 16 standard channels, channel 1 is
    the MPE master/zone channel; channels 2-16 are the member pool live
    melodic/chord/guitar touches dynamically claim from
    (`services/expression.c`'s own `claim_mpe_channel()`). Of those 15,
    4 are already permanently reserved for the regular sequencer's own
    4 lanes, and 1 more (nibble 10) is separately, permanently used by
    chord mode's own fixed `OP_CHORD_CHANNEL` -- leaving 10 genuinely
    free before Song mode existed at all. Confirmed real feedback: "9
    song tracks, 1 channel stays free for live MPE" -- Song mode's own
    pool (`s_song_channel_pool[]`) claims 9 of those 10, working from
    the top down, same convention the 4 lanes already use.
  - **Channel permanence vs. pattern count.** Real feedback wanted a
    channel that "follows the pattern" (survives reordering) AND up to
    24 real independent patterns -- but those can't both hold with a
    PERMANENT per-pattern channel, since there are only 9 exclusive
    channels and up to 24 patterns. Resolved by making channel
    assignment dynamic instead: `song_claim_channel()`/`song_release_
    channel()` claim from the 9-slot pool the moment a pattern starts
    PLAYING and release it the moment it stops -- the same mechanism
    `claim_mpe_channel()` already uses for live MPE, just a separate,
    smaller pool. `tiles_op_mode_sequencer_channel_is_reserved()`
    (the one function `claim_mpe_channel()` already calls per
    candidate channel) got extended to also check Song's pool, so live
    touches still can't steal a channel a song track is using -- no
    new call site needed anywhere.
  - **Data model**: `op_song_pattern_t` -- 128 steps (`OP_SONG_STEPS_
    PER_PAGE` 16 x `OP_SONG_NUM_PAGES` 8), up to `OP_SONG_MAX_NOTES_
    PER_STEP` (4) notes each via the same 0xFF-sentinel-for-"unused
    slot" convention the regular sequencer's own multi-note rework
    just established, doing double duty as "is this step armed" (no
    separate `step_armed[]`/`step_pitch_override[]`/probability/
    ratchet at all -- real feedback: "plain armed/notes only" for this
    first version) -- plus a `hue_byte` for this pattern's own random
    color (see below), not yet assigned anywhere since nothing creates
    a pattern yet. `OP_SONG_NUM_SLOTS` (24, one per pad) of these,
    each independently real -- real feedback, after an earlier round
    proposed collapsing the count to match the channel budget: "i want
    up to 24 real independent patterns."
  - **Flash storage**: unlike the regular sequencer's `op_seq_
    pattern_t`/`tiles_pattern_flash_t` split, Song mode's runtime copy
    IS its own on-flash layout -- no packing needed, since the 513-
    byte-per-pattern footprint fits its own reserved region with real
    margin. New `tiles_song_store_t`/`TILES_SONG_FLASH_OFFSET`, a
    dedicated `TILES_SONG_NUM_FLASH_SECTORS` (4) region reserved
    immediately below the regular sequencer's own single sector. Flash
    SPACE was never the constraint (this board's 4MB vs a firmware
    image under 128KB) -- only the SIZE OF ONE ERASE+PROGRAM OPERATION
    is, since that runs with interrupts disabled and nothing able to
    pet the watchdog mid-operation. `song_store_write_all()` (not yet
    called from anywhere -- nothing can create/edit a pattern yet)
    writes its 4 sectors as 4 separate, independent erase+program
    calls, each individually as safe as the regular sequencer's own
    single-sector save, just repeated -- confirmed acceptable to real
    feedback ("save to flash, like the existing pattern bank") even
    knowing the whole save now takes noticeably longer overall.
    Measured, not estimated: 12324 bytes against a 16384-byte budget,
    4060 to spare -- see `tiles_song_store_t`'s own `_Static_assert`.
  - **Menu integration**: `OP_MODE_SONG` added to the mode enum,
    `OP_MENU_COL_SONG` (column 5, the next free slot) wired into
    `col_is_available()`/`render_menu_col_color()`/`col_is_current_
    mode()`/`handle_menu_taps()`. Selectable now, but has no rendering
    of its own yet -- falls through to the generic "background pattern
    playing" triangle-pulse branch every other non-sequencer mode
    already uses, which is harmless (checks the regular sequencer's
    `any_lane_running()`, unrelated to Song mode) but not yet
    Song mode's own real screen.
  - **Still to come, in later passes**: the two screens (24-pad track-
    overview: tap=start/stop with the 9-concurrent cap and red-flash-
    blocked feedback, hold 2s=edit; per-pattern step-edit: 16 steps in
    columns 1-4, 8 pages row-major in columns 5-6, diamond=back to
    overview), the reorder gesture (shift+tap to pick up pulsing green,
    plain tap elsewhere to move/swap, 2x green flash confirm) and
    delete gesture (shift+hold 5s, 2x red flash confirm), manual per-
    step pitch editing, capture integration (both cross-capture from
    melodic/chord/guitar and a capture gesture from within Song mode
    itself, both always creating a new pattern in the next empty slot,
    blocked+red-flash if all 24 are full), and the actual per-track
    HSV hue-to-RGB rendering for `hue_byte`.
- **Song mode stage 2: the 24-pad track-overview screen -- rendering,
  tap-to-start/stop, reorder, delete.** Still no step-edit screen, no
  manual editing, and no capture -- so nothing can actually become
  occupied yet, meaning start/stop/reorder/delete are all fully wired
  but only testable in their "nothing here yet" shape until a later
  pass adds a way to create a pattern.
  - `handle_song_overview_taps()`/`render_song_overview()`, dispatched
    from `tiles_op_mode_scan()` alongside the regular sequencer's own
    branch (forward-declared, since they're defined down in this
    file's "Song mode" section, well after that call site -- same
    "declare here, define later" precedent already used elsewhere).
  - Tap a stopped, occupied pad to start it (`song_toggle_start_
    stop()`, always from step 1); tap a playing one to stop it. Start
    claims a channel from the 9-slot pool (`song_claim_channel()`);
    if all 9 are already claimed, blocked with a single brief red
    flash (`song_flash_error()`, `OP_SONG_ERROR_FLASH_MS`) -- confirmed
    real feedback for exactly this case.
  - Reorder: shift+tap an occupied pad (`song_pick_up()`) pulses it
    green (`menu_selected_pulse_level()`, reused as-is); a later plain
    tap elsewhere (`song_place()`) moves it there, or swaps if that pad
    is also occupied, confirmed with a double green flash on whichever
    pad(s) actually changed. Tapping the picked-up pad again cancels.
    `song_place()` moves the WHOLE slot's worth of state (pattern,
    running, channel, current step, sounding notes), not just the
    pattern struct, so a playing pattern keeps playing correctly
    through a move/swap -- though this needed no special handling at
    all, since channel reservation is keyed by channel number against
    `s_song_channel_pool[]`, never by slot index in the first place.
  - Delete: shift+hold 5 seconds on an occupied pad (`OP_SONG_DELETE_
    HOLD_MS`) clears it, ending its note and releasing its channel
    first if it was playing, confirmed with a double red flash.
  - Move/delete confirmation reuses the exact two-blink shape and
    timing the regular sequencer's own pattern-bank save/delete flash
    already established (`OP_SONG_FLASH_BLINK_MS`/`_COUNT`/`_TOTAL_MS`
    -- separate constants with the same values, this file's own
    "same convention, separate copy" precedent, not shared code).
  - Color: occupied+stopped shows a dim version of the pattern's own
    random hue (`song_hue_to_rgb()`, a real HSV->RGB conversion mapping
    `hue_byte` linearly across the 30-90 degree orange-to-yellow-green
    band real feedback asked for); occupied+playing shows it at full
    brightness. Underglow is plain steady yellow throughout, matching
    "this mode is characterized by the color yellow like the underglow
    of capture." No hue_byte is ever actually assigned yet (nothing
    creates a pattern), so every slot would currently show the same
    warm end of the band if one somehow existed.
- **Song mode stage 3: playback engine + capture from within Song mode
  -- Stage 2's track-overview screen is now actually testable end to
  end.** Real feedback confirmed capturing from within Song mode
  itself is wanted (not just cross-capture, still unrewired -- see
  below), so this pass builds a way to actually create a pattern and
  hear it loop, closing the gap Stage 2 shipped with.
  - **A real, and non-obvious, bug found before this ever reached
    hardware**: sequencer mode is the ONLY mode that claims `standby_
    active` for its entire duration, and `tiles_lighting_set_standby_
    pad_rgb()`/`_underglow_rgb()` (which `render_song_overview()`
    entirely depends on) are silent no-ops whenever it's false. Song
    mode never claimed it, so Stage 2's whole screen would have
    rendered nothing at all on real hardware. New `mode_owns_standby_
    grid()` is now the one place that decides this (used by `set_
    active_mode()`, `menu_exit()`, and `scale_menu_exit()` -- the
    latter two needed it too, since the top-level mode picker and the
    scale picker are both reachable from Song mode and have to know
    whether to release standby on close or leave it claimed). Also
    extended `tiles_op_mode_owns_pad_grid()` the same way, since
    `services/expression.c` defers to it before processing a touch as
    a live melodic note -- without this, capturing from within Song
    mode would have double-fired every note (this function's own
    `tiles_midi_note_on()` calls, plus expression.c's independent
    pipeline reacting to the same touch).
  - **Playback engine**: `song_advance_clock()`/`song_enter_step()`,
    a deliberate parallel copy of the regular sequencer's own `seq_
    advance_clock()`/`seq_enter_step()` shape rather than a shared/
    parameterized version -- Song's data (128 fixed steps, no
    probability/ratchet/length) is different enough that unifying them
    would mean threading "does this even apply here" branches through
    code that's supposed to stay simple. Same `OP_SEQ_CLOCKS_PER_STEP`
    timing as the regular sequencer, so both stay in sync with the
    same tempo for free. Called for every non-empty slot every scan
    (mirroring the regular sequencer's own per-lane background loop),
    except whichever ONE slot is currently being captured into.
  - **Capture**: `song_capture_enter()`/`_exit()`, wired to shift+
    diamond while `s_active_mode == OP_MODE_SONG` (a new branch in
    `handle_diamond_transport()`, checked before the existing cross-
    capture branch). Always claims the next empty library slot and a
    channel from the 9-slot pool; silently no-ops (nothing to flash a
    red error on yet -- there's no single pad this gesture points at)
    if either is unavailable. Assigns `hue_byte` via `get_rand_32()`
    right here, the moment a pattern is actually created. `song_
    capture_handle_taps()`/`_advance_clock()` mirror the regular
    sequencer's own capture shape closely (nearest-step quantization,
    a pending-cluster accumulator, live-preview notes independent of
    commit timing) with one real difference: no chord-region special
    case at all, since capturing from within Song mode always reads a
    pad through the plain melodic `tiles_note_map_get_note()` mapping
    -- there's no chord-region concept in Song mode itself. The
    captured slot is marked running immediately, before a single note
    is even played, so the track-overview shows it as occupied+playing
    right away, and it keeps looping seamlessly once capture ends
    (same channel, same running state, same current step -- capture
    just stops being the thing driving it forward).
  - **Not done in this pass, flagged explicitly**: cross-capture
    (shift+diamond from melodic/chord/guitar) still targets the
    regular sequencer's lane 3, not Song mode -- the original ask was
    "song mode as the default capture mode instead of regular
    sequencer," which means rewiring that too, just not yet.
- **Song mode stage 4: cross-capture rewired from the regular
  sequencer's lane 3 onto Song mode's own library -- closing the
  original ask ("song mode as the default capture mode instead of
  regular sequencer") that stages 1-3 deliberately deferred.** The
  entire "Cross-mode capture into lane 3" section (`cross_capture_
  enter()`/`_exit()`, `OP_CROSS_CAPTURE_LANE`, `cross_capture_bank_
  for_mode()`, `s_cross_capture_active`, the three `tiles_op_mode_
  cross_capture_*()` public accessors) is deleted outright, not kept
  around unused -- `handle_diamond_transport()`'s shift+diamond branch
  outside sequencer mode collapses to one unconditional `song_capture_
  enter()`/`_exit()` call, since `song_capture_enter()` always targets
  the next empty library slot regardless of which mode triggered it,
  so there's no longer a real difference between "capturing from Song
  mode" and "capturing from melodic/chord/guitar" worth branching on.
  `handle_circle_tap()`'s `mode_ok` and the various defensive mode-
  switch-ends-capture guards (`set_active_mode()`, `handle_triangle_
  click()`'s shift+triangle escape hatch) all now check `s_song_
  capture_active` instead of the retired flag.
  - **Chord-region voicing came along for the ride**: `song_capture_
    handle_taps()` gained the exact same chord-region special case
    `seq_capture_handle_taps()` already has (trigger on `handle_chord_
    pad_taps()`'s own real strike, `build_chord_voicing()`'s full 4
    voices, real velocity) -- needed now that capturing from chord
    mode is one of the ways to reach this function. Capturing from
    within Song mode itself never hits this case, since Song mode has
    no chord region of its own.
  - **services/lighting.c**: `write_cross_capture_underglow()` ->
    `write_song_capture_underglow()`, `tiles_op_mode_cross_capture_
    is_active()`/`_is_note_sounding()` -> `tiles_op_mode_song_capture_
    is_active()`/`_is_note_sounding()`. The old "current step pad"
    marker (`tiles_op_mode_cross_capture_current_step_pad()`) has no
    replacement -- it relied on the regular sequencer's 24-step
    pattern mapping naturally onto the 24-pad grid 1:1; Song mode's
    128 steps have no equally natural single-pad mapping while
    melodic/chord/guitar's grid, not Song's own step-edit screen, is
    what's actually showing. Deferred, not forgotten.
  - **A real bug caught during the rewire, not just a rename**: the
    previous round's own justification for excluding Song mode from
    `tiles_op_mode_owns_pad_grid()`'s capture case was wrong -- it
    reasoned that letting `services/expression.c` also process a
    touch during capture would "double-fire" every note. It doesn't:
    expression.c's own live note goes out on its own dynamically-
    claimed MPE channel, while the capture engine's copy goes out on
    the dedicated slot channel -- two genuinely separate, non-
    colliding outputs, the exact same "live feel stays intact, a
    separate channel also gets recorded" shape cross-capture always
    had. The REAL fix needed was narrower: `mode_owns_standby_grid()`
    now excludes Song mode's OWN capture specifically (`mode ==
    OP_MODE_SONG` returns `!s_song_capture_active`, not a blanket
    true) -- without it, capturing from WITHIN Song mode itself (the
    one case where `s_active_mode` genuinely IS `OP_MODE_SONG` during
    capture, unlike triggering from melodic/chord/guitar) would have
    kept `standby_active` claimed and expression.c suppressed for the
    entire session, contradicting `song_capture_enter()`'s own
    explicit release of both. `tiles_op_mode_owns_pad_grid()` reusing
    this same function needed no further change once it was fixed at
    the source.
- **Two real-hardware fixes to Song mode's track-overview, requested
  together**: "the playing pads should pulse and they should be not
  subtile hue shift from pad to pad they should be defined different
  colors following a hue shift."
  - **Playing pads now pulse.** A running pattern was previously just
    a flat, steady full-brightness pad -- identical in *behavior* (if
    not color) to a stopped-but-saved pattern's own flat dim level,
    giving no "this one's actually running" signal beyond color and
    brightness alone. `render_song_overview()` now reuses the exact
    same pulse this screen already draws for the picked-up-for-reorder
    pad (`menu_selected_pulse_level()`), just applied to the running
    pattern's own hue instead of a fixed green -- the same file-wide
    "standardize the pulsing" convention this file's own `OP_MENU_
    SELECTED_PULSE_*` comment already establishes, not a new pulse
    shape invented for this one case.
  - **Hue assignment is no longer random.** `song_capture_enter()`
    used to pick `hue_byte` via a plain `get_rand_32() & 0xFF` -- a
    uniform random pick over the full range has no floor on how close
    two picks can land, so two patterns created back to back could
    (and on real hardware, did) get nearly the same color, exactly the
    "subtle hue shift" complaint. Replaced with a deterministic
    sequence instead: each new pattern's `hue_byte` is the previous
    one's plus a fixed step (`OP_SONG_HUE_STEP = 97`), wrapping via
    `uint8_t` overflow. 97 is odd, and `gcd(97, 256) == 1`, so
    repeatedly adding it visits all 256 possible values before ever
    repeating (an even step would only ever reach half the range) --
    no two of Song mode's 24 patterns can land on the same hue by this
    sequence alone. It's also close to 256 * (1 - 1/phi) (~97.8), the
    "golden angle" fraction generative art already uses for the
    identical problem of assigning a growing series of colors so every
    new one reads as clearly distinct from every one already assigned,
    not just from its immediate predecessor. The running counter (`s_
    song_next_hue_byte`) is persisted in `tiles_song_store_t` (new
    field, version bumped 1 -> 2 -- an old v1 image is a different byte
    layout, not just missing a field, so it's treated as never-saved
    rather than misparsed; any patterns saved during Song mode's own
    bring-up are lost on this first boot, which is fine, they were this
    feature's own test data) so the sequence survives a reboot instead
    of restarting from 0 and risking an early repeat against colors
    already on other saved patterns. `get_rand_32()`/`pico/rand.h` are
    no longer used anywhere in this file and were removed.
- **Song mode stage 5: the step-edit screen and manual per-step pitch
  editing** -- the two pieces explicitly deferred when stage 1 first
  scoped this feature ("Include manual step editing now" was already
  confirmed back then; building it just hadn't happened yet). None of
  this screen's actual interaction had been specified beyond "hold for
  2 seconds is open edit for pattern," so it got its own real-hardware
  Q&A round before implementation:
  - **Entry**: real feedback confirmed the original 2-second-hold spec
    verbatim. `handle_song_overview_taps()`'s plain (non-shift) branch
    used to fire `song_toggle_start_stop()` immediately on touch-down;
    it now defers that to release, and fires `song_edit_enter()`
    instead if the same touch is still down past `OP_SONG_EDIT_HOLD_MS`
    (2000ms) -- the same "measure held_ms, fire once, suppress the
    plain action on release" shape the shift-branch's own 5-second
    delete hold already used (`s_song_edit_fired[]` mirrors `s_song_
    delete_fired[]`).
  - **Layout**: "columns 1-4... the current page's own 16 steps... the
    right two columns are the 8 pages" -- confirmed row-major for both
    (already documented, unbuilt, when `OP_SONG_STEPS_PER_PAGE` was
    first defined in stage 1). `song_edit_step_pad()`/`song_edit_page_
    pad()` compute each pad from its logical step/page index via
    `board_pad_for_row_col()`, the same row-major numbering every other
    grid in this file already uses, rather than a lookup table.
  - **Manual pitch entry** ("How do you set a step's pitch manually?"):
    "Select step, then tap grid to pick note" -- tapping a step enters
    pitch-pick, where the WHOLE grid becomes a chromatic note surface
    (same "release standby, let melodic-style coloring and live MPE
    sound through" mechanism `song_capture_enter()` already
    established for its own capture -- `mode_owns_standby_grid()`
    gained the identical exclusion for `s_song_edit_pick_active`).
    Tapping the SAME step pad again commits whatever was picked,
    REPLACING the step's old notes wholesale -- including clearing it
    if nothing was picked, since there's no separate clear gesture,
    this doubles as one. Diamond click instead cancels, leaving the
    step exactly as it was (`handle_diamond_transport()` gained a new
    branch, checked ahead of its existing plain-click play/stop/record
    toggle, since that toggle was otherwise this button's only
    behavior in Song mode and would otherwise fire a transport Stop/
    Start instead of backing out of the edit screen).
  - **Chords, corrected mid-Q&A**: first proposed as "tap multiple pads
    in sequence, each addition/removal toggling the chord" -- real
    feedback rejected this outright: "tap multiple notes together but
    they have to be played together or arpegiated quickly within the
    step, we cant have it glitch with one at a time aditions." Built
    instead as a strike window (`OP_SONG_EDIT_PICK_WINDOW_MS`, 200ms,
    a first-attempt guess not yet verified against real hardware
    feel): any pad touched within the window of the FIRST pad in a
    fresh strike joins the same chord (up to `OP_SONG_MAX_NOTES_PER_
    STEP`); a touch arriving after the window closes starts an
    entirely new chord instead of silently appending to the old one,
    so a stray later tap can never quietly graft itself onto an
    already-intended chord -- directly addressing the "glitch" this
    feedback was about.
  - **Page-occupancy indicator**: "dim vs. lit distinguishes empty vs.
    occupied pages" -- confirmed. `render_song_edit()`'s page loop
    shows the currently-viewed page as a white `menu_selected_pulse_
    level()` pulse (the same "selected" signal this file's menus
    already standardize on) regardless of content, a non-current
    occupied page at the pattern's own hue dimmed to `OP_SCALE_
    AVAILABLE_LEVEL`, and a non-current empty page fully off.
  - **Not built in this pass**: no visual marker for the currently-
    playing step on this screen even when the pattern being edited is
    also running in the background -- not asked for, and this screen
    already has a natural, unclaimed use for it (unlike the old cross-
    capture's marker, deliberately dropped in stage 4, which had no
    natural home left anywhere); flagged as a plausible future
    addition, not built speculatively.
- **The regular sequencer's own per-step chord editing had the exact
  same "many notes ruined the mechanism" problem Song mode's step-edit
  screen was just fixed for, and got the identical fix**: "fix the
  same chord thing on the sequencer, the many notes posibility ruined
  the mechanism." `handle_edit_mode()`'s `OP_SEQ_EDIT_PITCH` branch
  used to let tapping any pad other than the edited step's own toggle
  that pad's note into or out of a cluster with NO time bound
  whatsoever -- a step's chord could be built (or silently mutated) by
  taps seconds or minutes apart, with no way to tell "still building
  the same chord" apart from "starting a completely different one."
  Replaced with the identical strike-window shape Song mode's step-
  edit screen just introduced (its own separate copy, `OP_SEQ_EDIT_
  PITCH_STRIKE_WINDOW_MS`, same 200ms first-attempt value, same "same
  convention, separate copy" reasoning so tuning one screen's timing
  can't retune the other's): any pad struck within the window of the
  first pad in a fresh strike joins the same chord (up to `OP_SEQ_
  MAX_NOTES_PER_STEP`); a touch arriving after the window closes
  starts an entirely new chord instead of appending to the old one.
  Tapping the step's own pad still closes and commits -- now REPLACING
  the step's old content wholesale with whatever was freshly struck
  (if anything was), rather than incrementally toggling it -- and the
  two pre-existing no-touch cases are both preserved exactly as they
  were: a fresh, never-armed step still falls back to its own live-
  resolved note, and an already-armed step closed without striking
  anything new is left completely untouched (the ORIGINAL single-note
  design's own "harmless no-op" close, from before multi-note existed
  at all).
- **The sequencer's own strike-window fix above didn't survive real
  hardware either -- multi-note manual pitch selection is now removed
  entirely, back to single note only**: "the note select for
  sequencer is not workig. remove mitiple note feature from that.
  just do single note in seqwuencer mode." Two different multi-note
  designs were tried here (an open-ended add/remove toggle, then a
  Song-mode-style strike window) and both got rejected on real
  hardware -- `handle_edit_mode()`'s `OP_SEQ_EDIT_PITCH` branch is now
  back to the exact shape this section had before either attempt: any
  touched pad commits that pad's own note as the step's ONE note and
  closes immediately, no accumulator, no window, no separate close
  gesture. This is a manual-edit-screen-only revert, not a data-model
  rollback -- `step_notes[][]`/`step_note_count[]` keep their multi-
  note shape (`OP_SEQ_MAX_NOTES_PER_STEP` stays 4) since live
  capture's own chord-region recording (a separately-requested
  feature, "steps should be able to capture chords") still needs up to
  4 notes per step and this feedback never mentioned it. `render_
  pitch_edit()` is untouched for the same reason: it just displays
  whatever a step's actual note count is, whether 1 (always true for
  a manually-edited step now) or up to 4 (still possible for a
  captured chord) -- not part of "the note select" mechanism that was
  actually broken. Song mode's OWN step-edit screen strike window
  (`OP_SONG_EDIT_PICK_WINDOW_MS`) is a separate, untouched mechanism
  and is not affected by this -- this feedback named "sequencer"
  specifically.
- **A proactive bug-hunt pass ("look for bugs elsewhere... and lets
  optimize stuff"), not from a specific real-hardware report** --
  8 parallel review passes across the files this session hadn't
  already been staring at, verified by hand before fixing. Five real
  ones fixed:
  - **MPE channel double-assignment (`services/expression.c`,
    `claim_mpe_channel()`)**: the voice-steal path (used once all 15
    Member Channels are claimed) called `end_held_note()` on the
    stolen channel, which correctly frees it (`in_use = false`), then
    reassigned `owner_pad`/`claim_seq` to the new pad -- but never set
    `in_use` back to `true`. The very next claim (any other pad) would
    see that index as free and hand out the SAME channel a note was
    still actively sounding on, so two pads would share one MIDI
    channel: pitch-bend/pressure from either would bend the other's
    note, and a note-off from either could strand or kill the other's.
    This is the exact "stuck note" bug class this codebase has fought
    all session, just in a code path (heavy polyphony forcing a steal)
    nothing had specifically exercised yet. One-line fix: set `in_use
    = true` right alongside the existing reassignment.
  - **Triangle/diamond exited game mode instead of steering Snake or
    controlling Tetris (`services/game_mode.c`,
    `gm_override_button_pressed()`)**: this function's own comment
    claimed "Triangle/diamond are never a live control in ANY of the
    five games," which is simply false -- `gs_handle_input()` (Snake)
    uses them as up/down, `gt_handle_input()` (Tetris) uses them as
    rotate/hard-drop. Since this function is checked before either
    game's own input handler runs, every steer/rotate/drop press
    silently exited game mode instead. Fixed the same way circle/
    square already are just below it (their own Pong-paddle carve-out)
    -- excluded from the override specifically while `GM_STATE_
    PLAYING_SNAKE`/`_TETRIS` is the active state, not menu-scoped like
    circle/square (a blanket menu-only rule would also have blocked
    triangle/diamond from exiting Pong/BreakoutBlocks/Simon Says
    mid-game, which never used them live and lost nothing under the
    old unconditional rule).
  - **Song mode never got the sequencer's own "don't blank the board
    over a running pattern" fix (`services/op_mode.c`,
    `tiles_op_mode_is_sequencer_active()`)**: this predicate (which
    `services/standby.c` uses to pick the long 20/30-minute idle/deep-
    sleep timeout instead of the short default) only ever checked the
    regular sequencer's 4 lanes. Song mode's own per-slot background
    loop keeps advancing every scan regardless of what's displayed too
    (the identical "keeps running regardless of what's shown" property
    the sequencer's lanes already have), so a Song pattern looping
    unattended got no timeout extension at all -- the board would
    eventually blank over it, the exact regression this function's
    sequencer-specific fix was originally built to prevent. Extended to
    also check `s_active_mode == OP_MODE_SONG` and a new `any_song_
    slot_running()` (mirroring the existing `any_lane_running()`);
    `s_song_slot_running[]` itself had to move earlier in the file
    (same "declare the specific array early" precedent as `s_song_
    capture_active`) since this predicate is defined before Song
    mode's own playback-state block.
  - **Exiting Song capture while the step-edit screen's pitch-pick was
    still open desynced the grid (`services/op_mode.c`, `song_capture_
    exit()`)**: this function unconditionally re-claimed `standby_
    active(true)` on exit. Harmless when capture was triggered from
    melodic/chord/guitar (nothing reads that flag while those modes
    are active anyway), but genuinely wrong for a case that didn't
    exist when this was written: capture is a global shift+diamond
    gesture reachable from anywhere, including while Song's own step-
    edit screen has a pitch-pick session open on a DIFFERENT slot --
    forcing standby back on mid-pick left the pads dark/stale instead
    of the live melodic note-picking surface pick mode depends on.
    Fixed by gating the restore on `mode_owns_standby_grid(s_active_
    mode)` -- the exact same check `set_active_mode()` itself already
    uses on every mode switch -- instead of assuming "true" is always
    the right answer.
  - **Step-edit's per-step commit rewrote all 4 flash sectors every
    single step (`services/op_mode.c`, `song_edit_pick_commit()`)**:
    unlike every other Song-mode save site (place/delete/capture-exit,
    each writing once per discrete user action), committing one step's
    pitch triggered a full `song_store_write_all()` -- a 4-sector
    erase+program, interrupts disabled for tens of ms, immediately
    after every individual step. Programming a 16-32 step pattern
    manually meant 16-32 full flash rewrites instead of one. Fixed by
    deferring the actual write to `song_edit_exit()` (a new `s_song_
    edit_dirty` flag, set on any committed step, checked once on
    leaving the screen) -- one write per editing session, and none at
    all if nothing was ever committed.
  - **Five more flagged, not fixed yet** (lower confidence or narrower
    reach, worth a second look before touching): `end_held_note()`
    frees an MPE channel by index with no ownership check (compounds
    the channel bug above if it's ever hit on a stale/reassigned
    channel); `services/lighting.c`'s underglow setter can race the
    debug/crash/song-capture override priority chain (same bug class
    as two already-fixed races this session, unconfirmed on real
    hardware); `services/hall.c`'s `tiles_hall_init()` doesn't mark a
    pad's calibration bad if its baseline-seeding read fails during
    boot; `services/haptics.c`'s voice-ceiling check in `trigger_
    kick()` treats a pad mid-touch-pulse as neither idle nor counted,
    potentially bypassing `max_haptic_voices`; `services/midi_clock.c`
    doesn't resync its virtual-pulse timer when an external clock
    disconnects and tap tempo resumes, risking a burst of pulses fired
    in one scan.
- **Non-MPE pitch-bend-wheel compatibility mode**, `services/
  expression.c`/`expression_control.c`: "lets make sure the pitch bend
  works with non mpe layouts meaning pitch bend wheel." Today, every
  note claims its own dynamic MPE Member Channel and pitch bend/
  channel pressure are genuinely per-note -- a receiver that isn't
  MPE-aware (a plain single-channel synth, or a DAW track set to an
  ordinary channel instead of an MPE zone) only listens on ONE
  channel and would silently ignore bend/pressure sent on any of the
  other 14. Asked what "most compatible" should mean concretely: "look
  for the max most combaptible and standardized version" -- the answer
  is MIDI's own original Basic Channel mode (all notes on one channel,
  pitch bend and channel pressure as ordinary CHANNEL-WIDE messages),
  the layout virtually every synth supports by default, so that's what
  `!tiles_expression_is_mpe_enabled()` now switches to: every note
  goes out on `TILES_MIDI_MPE_MASTER_CHANNEL` instead of a claimed
  Member Channel, bypassing `claim_mpe_channel()`'s whole pool
  entirely (nothing to steal-evict when every note already shares one
  channel).
  - **Multi-pad bend ownership**: a single shared channel has exactly
    one live bend/pressure value, but this is a 24-pad polyphonic
    controller -- asked who should win when more than one pad is held
    and tilting: "most recently touched/bent pad wins." Each pad now
    stamps a `touch_claim_seq` (from the same monotonic counter
    `claim_mpe_channel()` already used for its own steal-priority) at
    every note-on, regardless of mode. `s_non_mpe_owner_pad` tracks
    whichever held pad currently drives the shared channel: set to the
    newly-struck pad on every note-on (always the most recent by
    construction), and hunted for again among the REMAINING held pads
    (`find_most_recent_held_pad()`) when the current owner releases --
    never just dropped to "nobody" while another pad is still
    genuinely held. Both the note-on claim and the release hand-off
    force-resend the (new) owner's actual current bend value rather
    than waiting for it to naturally "change" from its own last-sent
    value, since the shared channel could be sitting wherever a
    DIFFERENT pad left it -- the exact "note lands in the wrong pitch"
    failure this whole feature exists to prevent, just at the channel
    level instead of the per-note level. Channel pressure gets the
    same ownership gate (without it, multiple held pads would fight
    over the shared channel's pressure value every scan) but
    deliberately no forced resync on hand-off -- stale pressure for a
    scan or two is a much smaller, more cosmetic problem than a
    mis-pitched note.
  - **A real bug this addition would otherwise have introduced,
    caught before it shipped**: `end_held_note()`'s existing MPE-
    channel-release line computes `idx = midi_channel -
    TILES_MIDI_MPE_FIRST_MEMBER_CHANNEL`. On the shared master channel
    (0) that's `0 - 1`, which underflows a `uint8_t` to 255 --
    `s_mpe_channels[255]` is 240 bytes past the end of a 15-entry
    array. Guarded with an explicit `if (midi_channel ==
    TILES_MIDI_MPE_MASTER_CHANNEL)` branch instead of changing that
    subtraction, since master-channel notes have no per-channel pool
    slot to release in the first place.
  - **The toggle gesture replaces expression mute's old one**: "replace
    haptic mute combo to the mpe vs regular mode selector standard is
    mpe, isntead of flashing the light should do a soft pulse." The
    circle+square 2-second hold that used to call `toggle_mute()` now
    calls `toggle_mpe_mode()` instead, reusing the exact same combo/
    threshold rather than inventing a new one. `tiles_expression_set_
    muted()` itself is untouched and still fully callable -- only its
    one gesture in `expression_control.c` was reassigned, so mute is
    currently unreachable from any button until/unless it's given a
    new one. The square LED's old two-blink mute pattern is replaced
    with a slow breathing pulse (same shape as `services/standby.c`'s
    own deep-sleep pulse, a separate copy per this codebase's own
    convention), shown whenever the non-default (non-MPE) mode is
    active -- MPE stays the default with no ambient indicator, per
    "standard is mpe."
- **Pedal: sustain and expression reworked into a real mode select**
  (`services/pedal.h`/`.c`). Asked whether sustain and expression were
  working: sustain (fully wired, on by default) should work as coded,
  though not confirmed against a real pedal on record; expression was
  built but genuinely unreachable -- disabled by default, and nothing
  anywhere called its one enable toggle. Real feedback: "enable those
  two as how they would work standard and lets keep sustain pedal as
  the defoult but we can edit this in control software later."
  - Both are now implemented to their own real MIDI standard: sustain
    unchanged (CC64, debounced hysteresis); expression (CC11) linear
    across the ADC's full range, heel-down = 0, toe-down = 127, the
    conventional TRS expression-pedal wiring -- "implemented to the
    standard," still not confirmed against a real expression pedal on
    this circuit specifically, same open item as before.
  - **The old plain boolean toggle was replaced with a real
    `tiles_pedal_mode_t` (`TILES_PEDAL_MODE_SUSTAIN`/`_EXPRESSION`),
    not just flipped on alongside sustain** -- this is a single
    physical jack with one signal wired to the ADC, so it can do
    sustain OR expression, never genuinely both: leaving both always
    computed-and-sent (the previous boolean's own shape, just
    defaulted true) would mean a real sustain footswitch's rail-to-
    rail swing also spamming spurious CC11 messages, and a real
    expression pedal's sweep also spuriously toggling CC64 sustain
    every time it crossed the hysteresis band. `tiles_pedal_set_mode()`
    is the new switch -- sustain stays the default, per real feedback,
    with no on-device gesture calling it yet (same "companion app hook
    for later" reasoning the old toggle already had).
  - **A real stuck-state bug this rework fixes in passing**: switching
    modes now cleanly winds down whichever one is being LEFT --
    releasing sustain with a genuine CC64=0 if it was currently held
    (a synth has no idea this jack changed function; without this,
    switching away mid-hold would leave its dampers stuck down
    forever, the pedal equivalent of a stuck note), and resetting
    expression to CC11's own MIDI-spec default of 127 (full
    expression) rather than leaving playback quietly capped at
    whatever level the pedal happened to be sitting at.
- **CV/gate, built for the first time this session** (`drivers/
  dac80502.h`/`.c`, `services/cv_gate.h`/`.c`). Real feedback: "lets
  implement cv/gate functionality as it would work standard but with
  modifiable standard controls for the control software down the
  line."
  - **"As it would work standard"**: a classic monophonic MIDI-to-CV
    converter -- 1V/octave pitch CV (DAC VOUTA), a pressure CV channel
    (VOUTB, matching what the board map itself already labels that
    channel), and a single active-high Gate high for as long as ANY
    note this instrument is currently playing is held. Last-note-
    priority when more than one note is held (an immediate legato
    pitch jump, gate never dropping while at least one note stays
    held) -- the same "most recently touched/bent pad wins" rule this
    session's non-MPE pitch-bend mode already established for its own
    shared-channel arbitration, reapplied here to a genuinely single-
    voice CV output. Deliberately note-based, not pad/lane/slot-based:
    `tiles_cv_gate_note_on()`/`_note_off()`/`_channel_pressure()` are
    called explicitly alongside the matching MIDI send at every one of
    this instrument's ~20 note-on/off/pressure sites across
    `expression.c`, `game_mode.c`, and `op_mode.c` (live touch, the
    regular sequencer's playback and capture preview, chord mode,
    Song mode's playback and capture preview, the hidden game
    melody) -- the same "explicit call at each site" shape
    `services/haptics.h`'s own `trigger_kick()`/`stop()` already use
    throughout those exact files, rather than a hidden hook inside
    `midi/` (which has no business knowing about note priority or any
    other `services/`-level concept, per the recommended module
    boundary in `docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md`'s own
    "Firmware structure" section).
  - **"Modifiable standard controls for the control software down the
    line"**: both CV channels' scaling are runtime-settable structs
    (`tiles_cv_gate_set_pitch_calibration()`/`_set_pressure_
    calibration()`), not fixed constants -- pitch defaults to the
    standard convention (1V/octave, MIDI note 0 = 0V); pressure
    defaults to the jack's own nominal 0-10V full range at
    pressure=127. A separate, later zero+gain trim layer on each
    (defaulting to identity, per `docs/architecture/defaults-and-
    safeguards.md`'s own "CV range" section: "Build the per-channel
    zero+gain trim slot into the calibration store from the start...
    defaults to identity until measured") is meant for real hardware
    correction once measured. Not yet persisted to flash -- same as
    `pedal.c`'s mode/polarity and `expression.c`'s pitch-bend
    sensitivity, this resets to standard defaults on every reboot;
    real persistence belongs in a `storage/` module that doesn't exist
    in this codebase yet, not invented here just for this one value.
  - **Safety, both already-established hardware facts, neither
    re-decided here**: hard-disabled unless `services/power.h`
    reports `cv_gate_permitted` (external 12V confirmed via GP22) --
    checked on every note event AND reacted to instantly via `tiles_
    power_register_callback()` the moment power disappears mid-hold,
    not on this module's next poll, exercising that callback path for
    the first time since `power.h` was built specifically anticipating
    it. Defaults **off** even with valid external power -- `tiles_cv_
    gate_set_enabled()` is the separate, explicit software switch
    (defaults false, no on-device gesture calls it yet, same
    "companion app hook for later" shape pedal mode and MPE mode
    already have). Both gates must hold before anything is ever
    driven; either dropping forces gate low and both DAC channels back
    to 0 immediately (`force_safe_off()`), clearing all tracked notes
    so a later stray note-off can't act on stale state.
  - **`drivers/dac80502.c` is a brand-new, unverified driver** -- unlike
    every sensor driver this session already confirmed against real
    hardware (Hall, touch), this chip has never been touched by this
    codebase before. The 24-bit SPI register protocol (GAIN register
    forcing the internal 2.5V reference undivided at 1x buffer gain,
    matching the external OPA2990's own fixed gain-of-4 stage) is
    implemented from the DAC8050x family's own documented protocol as
    understood at write time, not confirmed against a logic analyzer
    on this specific board. Worst-case failure mode if a register
    value or timing detail is wrong is "outputs nothing, or the wrong
    voltage within the chip's own bounded 0-2.5V rail" -- not a
    destructive one, since a garbled SPI frame can't exceed the DAC's
    own supply rails -- but real hardware bring-up (a multimeter on
    the CV jack, at minimum) is still needed before trusting it the
    way this codebase's other drivers now are.
- **USB vendor control protocol, first version** (`firmware/src/
  usb_vendor/usb_vendor.c`, new module outside `services/` proper --
  documented here anyway to keep this session's own running log in one
  place). Real feedback: "keep cv gate implemented but off rn. we need
  the control software." Scoped in Q&A to firmware + protocol only,
  proven over a plain script -- not the real Electron companion app
  yet, and not the fuller protocol (`docs/protocol/README.md`'s own
  pad remap/calibration/streaming/profile/firmware-update design)
  either. What's built: a new TinyUSB vendor-class interface
  (`CFG_TUD_VENDOR`, previously 0) alongside the existing CDC+MIDI
  composite descriptors, carrying a deliberately simple plain-text
  `GET <key>` / `SET <key> <value>` / `LIST` line protocol -- see
  `shared/protocol/README.md` for the full key catalog and
  `tools/tiles_control.py` for the test script that exercises it.
  Covers every runtime setting this session already built with a
  companion-app hook in mind: `pedal.mode`/`pedal.polarity`,
  `expression.mpe_enabled`, `expression.pitch_bend_sensitivity`/
  `aftertouch_sensitivity` (gained real getters here -- they never had
  one before, since the on-device sub-menu only ever wrote a fresh
  value, never read one back), and all of `cv_gate`'s enable +
  both calibration structs. `cv_gate.enabled=1` over this protocol
  still doesn't drive anything unless `services/power.h` also confirms
  external power -- that hardware-enforced gate has no override here,
  by design. Nothing persists to flash yet, same as every setting this
  protocol exposes already didn't before it existed.
- **Scene Launch mode -- a 6th top-level mode, triggering Ableton Live
  scenes/clips.** Real feedback: "lets implemebt a new mode that
  triggers scenes in ableton live keep it simple for now, push
  triggers it. 4 vertical and the 6 horizontal and the 6th is full row
  trigger as usual. the -+ browse left and right on the visible
  scenes, can we pull the colors of the scenes from ableton ? and
  light behaviour to feel intuitive? also in that mode the underglow
  must do fun stuff, keep it white and when we trigger any scene it
  flashes onece in sentia color."
  - **Layout**: rows 1-4 = Ableton's first 4 scenes, always (no scene
    paging in this version -- confirmed in Q&A, "-"/"+" pans TRACKS
    instead). Columns 1-5 = 5 consecutive tracks' clip slots for that
    row (`s_scene_track_offset`, panned by "-"/"+", one step per
    press, no shift-combo, same convention guitar mode's own fret-
    shift already established in `handle_transport_and_length()`).
    Column 6, one pad per row, is that row's Scene Launch button --
    fires the WHOLE scene (every track's clip in that row at once),
    the same convention real Launchpad-style controllers already use.
  - **A real incoming-MIDI capability this codebase never had before**:
    pulling real scene/clip colors needed reading SysEx from USB MIDI
    IN, and `services/midi_clock.c` already owned the ENTIRE RX FIFO
    (silently discarding every byte that wasn't one of 4 Real-Time
    bytes it cared about) -- two independent readers can't both drain
    one shared FIFO without racing for bytes. Fixed with a new shared
    parser, `midi/midi_in.c`, that becomes the ONE owner of `tud_midi_
    stream_read()`; `midi_clock.c` was refactored to register a
    callback with it instead of reading MIDI itself (byte-for-byte
    the same Real-Time-byte logic it already had, just moved into a
    callback -- see `midi_clock.c`'s own comment). `midi_in.c` also
    recognizes SysEx frames (`0xF0`...`0xF7`) and dispatches complete
    ones to registered callbacks, which Scene Launch mode uses for its
    own protocol. `midi/midi_out.c` gained a matching `tiles_midi_
    send_sysex()` for the outgoing half.
  - **Wire protocol**: manufacturer ID `0x7D` (MIDI Association's own
    reserved "non-commercial/educational use" ID -- the correct choice
    for DIY hardware with no registered ID, unlike `usb_descriptors.c`'s
    own borrowed-but-documented Raspberry Pi USB VID, this one is
    actually reserved for exactly this situation). Full catalog:
    `shared/protocol/README.md`'s own "Scene Launch" section. Colors
    are real Ableton `Clip.color`/`Scene.color` values, not a fixed/
    quantized palette older controllers needed -- this hardware
    already has real per-pad RGB.
  - **"Intuitive" light behavior**: no clip ever reported = fully off;
    has a clip, not playing = lit at its own color dimmed to
    `OP_SCALE_AVAILABLE_LEVEL` (this file's own established "available
    but not selected" level); playing = full brightness, pulsing with
    `menu_selected_pulse_level()` (the same "this one's active" pulse
    `render_song_overview()` already established for a running Song
    pattern); triggered (queued, Ableton's own `is_triggered`) = a
    faster, plainer on/off blink -- deliberately a DIFFERENT shape
    from the smoother "currently active" pulse, so "about to change"
    reads apart from "already changed" at a glance, the same
    distinction real Launchpad-family scripts already draw. Column 6
    mirrors the same three levels off the Scene's own color/
    `is_triggered` instead of any one clip's (a Scene has no
    "is playing" of its own).
  - **Underglow**: steady white at rest; "any scene" (real feedback's
    own phrase) scopes the single Sentia-magenta flash to the column-6
    Scene Launch gesture specifically, not every individual clip fire.
  - **Ableton side**: `daw-integration/ableton/TILES/scene_launch.py`,
    a new module alongside the already-real-hardware-tested transport-
    remote code in `TILES.py` (untouched by this addition). Pushes
    every tracked clip/scene's state once on connect, then again on
    every real Live API change, via `add_*_listener` -- not yet
    confirmed against a real Ableton session, see that file's own
    module docstring for the honest confidence level (same spirit as
    `drivers/dac80502.c`'s own "new, unverified driver" framing).
    Existing installs need to re-copy the `ableton/TILES/` folder to
    pick up the new file -- see `daw-integration/README.md`'s own
    "Scene Launch mode" section.
  - **First real-hardware round found colors genuinely not showing --
    root-caused, not just worked around.** Real feedback: "colors ar[e]
    not showing." `scene_launch.py`'s own `_on_clip_slot_changed()` was
    monkey-patching an identifying attribute directly onto Ableton's
    native `Clip` object (`clip._tiles_slot_key = ...`) to recognize
    which listener belonged to which slot when a clip changed -- Live
    API objects aren't guaranteed to support arbitrary attribute
    assignment, and if that ever raised, the exception propagated all
    the way up through `TILES.__init__()`'s own `component_guard()`,
    silently aborting the ENTIRE script (the already-working transport
    remote included) rather than just this one feature. Fixed two ways:
    (1) replaced the monkey-patch with a plain dict this object owns
    itself, keyed by `(track_index, scene_index)`; (2) `is_playing`/
    `is_triggered` moved from the `Clip` object onto the stable
    `ClipSlot` itself, which never needs re-subscribing when a clip is
    added/removed (only `color`, a `Clip`-only property, still does);
    (3) `SceneLaunch.__init__()` now wraps `_connect()` in its own
    try/except, logging any failure (`self.log_message()`, visible in
    Ableton's own Log.txt) instead of ever letting a Scene-Launch-
    specific bug take the transport remote down with it -- the same "a
    failed subsystem disables itself, it never blocks or takes other
    subsystems down with it" rule this session's own firmware work
    already follows, just applied on the Ableton-script side for the
    first time. Every real action (connect, each state push, every
    SysEx received) now logs one line -- see `daw-integration/
    README.md`'s own "Debugging" note for where to actually look.
  - **Second real-hardware round: colors STILL not updating, and clip
    fires not reaching Ableton either.** Real feedback: "colors are not
    updating in the instrument or ableton, the indicators are not
    working well." Two more real bugs, both root-caused against
    Ableton's own bundled Remote Script source (`_APC/APC.py`,
    `_Framework/ClipSlotComponent.py`) rather than guessed at a third
    time:
    1. `handle_sysex(midi_bytes)` does NOT receive the `0xF0`/`0xF7`
       framing -- Ableton's framework strips both before calling back
       (confirmed: `APC.py`'s own real `handle_sysex` indexes
       `midi_bytes[3]`/`[4]` directly, no offset for a leading status
       byte). `scene_launch.py` assumed the framing was still present,
       so its manufacturer/sub-ID check was reading one byte too far
       right and silently rejected every Fire Clip/Launch Scene message
       TILES ever sent -- the whole reason clip fires never reached
       Ableton.
    2. `ClipSlot.add_is_playing_listener` isn't a real method. The real
       listener for playing-state changes is `add_playing_status_
       listener` (confirmed against `ClipSlotComponent.py`'s own
       `@subject_slot('playing_status')`); `is_playing` itself is only
       ever a plain, non-listenable property you re-read inside that
       callback. Calling the nonexistent method raised on the very
       first clip slot in `_connect()`'s loop, which the previous
       round's own try/except then swallowed and logged -- meaning
       `_connect()` aborted before registering a single listener or
       pushing a single state update, on every run since the feature
       was built, regardless of the first round's monkey-patch fix.
    Both fixed in `scene_launch.py`; see that file's own `handle_sysex()`
    and `_connect()` comments for the corrected indices/method name.
  - **Master stop**: real feedback, "a master stop in this app should
    be shift diamond. we dont use or have access to song mode when
    ableton mode is on" -- explaining why shift+diamond (which
    otherwise universally means `song_capture_enter()`/`exit()`, see
    that feature's own section) was free to repurpose for Scene Launch
    mode specifically: Song mode's capture feature isn't reachable/
    wanted from Ableton mode anyway. New SysEx message `0x03` (no
    payload) triggers Ableton's own real "stop all clips" action
    (`self.song().stop_all_clips()`, confirmed against `_Framework/
    SessionComponent.py`'s own stop-all-clips button) -- distinct from
    the diamond's plain-click transport Stop, which still works
    unchanged in this mode. See `handle_diamond_transport()`'s own
    Scene Launch branch, checked ahead of the generic shift+diamond
    branch so it wins over it for this one mode only. Also printf-traces
    itself (`scene_send_stop_all()`'s own line) over the USB CDC console
    as a debugging aid, since this specific gesture was reported not
    firing in Ableton with no other lead yet to root-cause it against.
  - **Stop one clip**: real feedback, "re pushing a playing clip pad all
    the way down or close to that stops the individual clip." Checked
    every scan while a pad is held (not just the touch edge, unlike
    fire), scoped to a clip already reported `is_playing` and a deep
    Hall-depth press (`OP_SCENE_STOP_CLIP_DEPTH_THRESHOLD`, deliberately
    higher than the menu picker's own 50% select depth -- unmeasured,
    a first guess). New SysEx message `0x04` calls the real per-clip
    `Clip.stop()` (confirmed against AbletonOSC's own `clip.py`, which
    wires the same method to its own `/live/clip/stop` handler) --
    distinct from `ClipSlot.fire()`, which retriggers rather than stops
    an already-playing clip.
  - **Session-ring outline**: real feedback, "the box was from my
    novation. i need that outline for tiles as well tho" -- the red box
    the user saw was their OTHER (Novation) controller's own Ableton
    session-ring overlay in Session View, not anything TILES's script
    drew. `scene_launch.py` now owns a plain `_Framework.SessionComponent`
    (Ableton's own framework class for exactly this box), sized to the
    same 5-track x 4-scene window this file's own grid shows and kept
    in sync via a new SysEx message `0x05` (sent once on Scene Launch
    mode entry and again on every "-"/"+" pan, see `scene_send_track_
    offset()`'s own call sites in `set_active_mode()`/`handle_transport_
    and_length()`). This module's own first use of `SessionComponent`
    rather than raw Live API listeners -- genuinely unconfirmed whether
    Ableton draws the ring with no `ButtonMatrixElement` ever bound to
    it, since this script keeps its own SysEx-based color feedback
    instead of handing that job to the component; wrapped in the same
    try/except `_connect()` already runs under, so a wrong guess here
    can't take clip fires/colors down with it.
  - **First real-hardware round of the ring found it genuinely not
    showing.** Real feedback: "ableton is not showing ring." Root
    cause: constructing a `SessionComponent` alone doesn't do anything
    -- a `ControlSurfaceComponent` only gets pulled into the framework's
    own per-tick update cycle (which is what actually pushes its state,
    including the ring paint, out to Live's UI) once it's passed to
    `register_components()`, confirmed against `_Framework/
    SceneComponent.py`'s own real use of that call to wire its child
    `ClipSlotComponent`s in. Fixed: `_connect()` now calls
    `self._control_surface.register_components(self._session)` right
    after creating it. Same real feedback also reported the shift+
    diamond master stop and the deep-press individual stop not firing
    -- no bug found in either path by inspection against the real Live
    API (both re-verified line by line), so both gained a debug trace
    instead (`scene_send_stop_all()`/`scene_send_stop_clip()`'s own
    `printf`s over the USB CDC console, plus a `self._log()` line on
    the Ableton side for each) to actually observe which side of the
    wire, if either, the gesture reaches next time.
  - **Diamond transport LED found dark specifically in this mode.**
    Real feedback: "for the diamond transport controls ive noticed it
    behaves properly in all modes except for ableton clip mode."
    Genuinely root-caused, not a guess: `tiles_buttons_set_override_led()`
    (what `handle_diamond_transport()` uses to draw diamond's four-state
    transport LED) is a transparent no-op for EVERY button, diamond
    included, the entire time `mode_owns_standby_grid()` is true --
    which it is for Scene Launch. `render_scene_launch()`'s own button-
    column loop was ALSO blanket-zeroing all 6 columns including
    diamond's every single scan, so nothing else ever wrote a real
    value there either -- diamond simply stayed dark for the whole time
    this mode was on screen. Same latent gap found in Song mode's
    `render_song_overview()`/`render_song_edit()` (invisible there only
    because 0.0f happened to already be correct in Sequencer mode's own
    equivalent, which reroutes diamond to a capture indicator instead).
    Fixed by extracting the four-state computation into a shared
    `transport_led_level()` and having all three render functions write
    it through their own `tiles_buttons_set_standby_led()` calls for
    diamond's column specifically -- the one path that actually lands
    while they own the grid.
  - **Playing-clip pulse strengthened.** Real feedback: "only the
    playing pad should pulse and should pulse more strongly." Was
    reusing `menu_selected_pulse_level()` (0.5-1.0, deliberately subtle
    for the mode picker's own "selected" indicator) -- too weak a swing
    to read as "this one is live" next to a steady dim clip. New
    `scene_playing_pulse_level()`: near-off to full (0.15-1.0) at a
    faster 600ms period, a much more pronounced breathing pulse,
    matching real Launchpad-family convention for a playing clip
    specifically (confirmed the general listener-driven design, and
    Ableton's own `stop_all_clips()`/`handle_sysex` reception mechanism,
    against Ableton's real bundled `_Framework` source and the
    community AbletonOSC project -- both match established, working
    patterns, not an invented architecture).
  - **Whole TILES -> Ableton direction rearchitected off SysEx.** Real
    feedback, after none of the above ever had one confirmed
    successful delivery: "master stop doesnt work at all, individual
    start and stop doesnt work and hasent for the past few pushes. i
    need you to look at how a lounchapd works or abletoun push works
    to pull the exxact same standardizre behaviour." Fire clip, launch
    scene, stop all, stop one clip, and the track-offset sync all
    moved from this file's own custom SysEx sub-protocol onto plain
    CC, sent via `tiles_midi_send_cc` -- the exact mechanism this
    file's own transport CCs already use, with actual confirmed
    real-hardware delivery. The Ableton -> TILES color-feedback SysEx
    (`scene_on_sysex()`) is unchanged -- that direction was never
    reported broken, and real per-pad RGB has no equivalent in a
    single CC value anyway.
  - **First attempt used Note-On, not CC -- real feedback found the
    real flaw.** "you fully broke how clip lounching works now its
    just sending regular midi notes for me to map. thats not how this
    feature operates ever in any device." The first version above
    sent Note-On (matching how a REAL Launchpad sends its own grid,
    confirmed against Ableton's bundled `Launchpad.py`), reasoning
    that `TILES_MIDI_MPE_MASTER_CHANNEL` carries no real note content
    of its own to collide with. That reasoning missed the actual
    conflict: a real Launchpad is a dedicated grid controller that
    never sends musical notes at ALL, so nobody ever enables that
    port's "Track" MIDI input in Ableton's Preferences -- TILES is not
    that. This exact same USB-MIDI port also carries real musical
    Note-On for melodic/chord/guitar/sequencer play, so the user's own
    instrument track almost certainly already has this port's Track
    input enabled (typically "All Channels," required for real MPE
    playback across the member-channel pool) -- meaning a Scene
    Launch "button" Note-On, on ANY channel, was ALSO delivered to
    that track as ordinary playable/recordable content, on top of
    whatever the Remote Script's own `ButtonElement` did with it.
    Being claimed by the Control Surface's Remote path and reaching a
    Track's input are not mutually exclusive in Ableton. A CC never
    has this problem -- Ableton never treats a CC as note/audio
    content for an instrument regardless of Track/Remote routing,
    exactly why the transport CCs have always been safe on this same
    port. Fixed by moving grid-touch (`OP_SCENE_CC_GRID_BASE`, 10) and
    stop-touch (`OP_SCENE_CC_STOP_BASE`, 40) off Note-On entirely, onto
    CC, matching everything else in this section. `tiles_midi_note_on`/
    `_off` are no longer used anywhere in Scene Launch mode.
- Everything else (per-pad Hall calibration, DIN MIDI) is not built
  yet.
