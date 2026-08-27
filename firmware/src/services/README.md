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
animations (plus a deep sleep state after 15 minutes), a power-on boot
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
  USB-only, 75% once external power is confirmed) instead of a
  hardcoded constant -- underglow does not use this ceiling at all.
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
  (`TILES_LIGHTING_IDLE_BASELINE_PERCENT`, 10%); a sharp (black) key
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
  6% of ceiling) -- deliberately LOWER than the natural-key baseline's
  10%, a reversal of this feature's first pass (which had root brighter,
  reasoning that a single-channel color reads dimmer than three-channel
  white at the same level and wanted a clear landmark). Real feedback
  called for the opposite: a subtler root indicator, dimmer than the
  surrounding naturals rather than a bright highlight. Unmeasured -- a
  first attempt at "visibly dimmer, not so dim it disappears," not
  calibrated against real LED brightness/diffusion.
  Unmeasured -- root's brighter percentage and the natural/sharp
  distinction reading clearly at actual LED brightness/diffusion are
  both first attempts.
  **Not done:** standby animations (needs its own design pass — see the
  defaults doc), Hall-driven (as opposed to touch-driven) brightness.
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
  **Square alone, momentary preview + 3-second sticky lock: the
  expression sub-menu.** Real feedback, after the button-identity
  correction: "it should be when you hold shift and sentia the pads
  become sliders one for each of the four rows... row one is haptics,
  row two is pitch bend, 3 is the remaining axis and 4 is aftertouch,"
  then, after a combo-based version (holding circle+square together) was
  tried on real hardware: "lets change [that] to hold square for 3
  seconds alone to toggle that menu," followed by a second hardware pass:
  "momentary press should open menu as well but after 3 seconds the
  toggle should happen." The sub-menu is now visible from the instant
  square is held alone (`s_submenu_visible`, `set_submenu_visible()`) --
  a momentary preview that disappears again on release below
  `EXPRESSION_SUBMENU_TOGGLE_HOLD_MS` (3000ms). Reaching that threshold
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
  Single hardware axis (X) used as "sideways" -- no hardware doc exists
  for which local Hall axis maps to which physical direction on a
  mounted pad, and MIDI pitch bend is inherently one-dimensional
  regardless, so Y is left unused rather than guessing how to blend two
  axes into one bend value; trivially swappable for Y once seen on real
  hardware.
  **Real MIDI limitation, not a bug:** this project has no MPE (no
  per-note channel allocation -- see `midi/README.md`), and Pitch Bend
  Change is a channel-wide message with no per-note addressing in the
  spec itself -- there's no way to bend one held note without also
  bending every other note currently held on the same channel. Rather
  than send confusing output with a chord held, this module tracks a
  single "owner" pad (`s_pitch_bend_owner_pad`, 0 = none): only the most
  recently struck pad drives the shared channel's bend, and bend is
  explicitly reset to center (`PITCH_BEND_CENTER`, 8192) whenever
  ownership changes or the owner releases, so a new or still-held note
  never inherits a stale offset. Playing one pad at a time behaves
  exactly as expected; bending while a chord is held is a known,
  deliberate V1 simplification.
  Toggled via a genuine square ("sentia", SW5) short click -- real
  feedback: "when you press sentia button once it turns on and off the
  pitch bend" (see `expression_control.h`'s entry below for the
  button-side gesture handling -- an earlier pass wired this to circle
  by mistake before real feedback corrected which physical button
  "sentia" actually is: "our shift and power button is circle. sentia is
  square button"). `tiles_expression_toggle_pitch_bend()` immediately
  resets to center if a note currently owns the bend when disabled,
  rather than leaving it stuck. `tiles_expression_set_muted()`
  (`expression_control.h`'s expression-mute combo) does the same thing
  from the other direction -- see its own entry below. **Not yet
  hardware-verified at all** -- neither the physics reasoning nor the
  pitch-bend sensitivity has been tried on a real strike yet.
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
- `pedal.h`/`.c` — done: sustain (MIDI CC64) on by default, debounced
  with hysteresis, polarity defaults to the usual normally-open
  footswitch convention and is switchable at runtime
  (`tiles_pedal_set_polarity()`). Expression (CC11, continuous) is
  built but **disabled by default** -- `tiles_pedal_set_expression_enabled()`
  is the runtime toggle, meant as the hook the companion app will
  eventually control once `usb_vendor/` exists. Real auto-sensing of
  polarity/disconnected-pedal state is still a later layer, see
  `docs/architecture/defaults-and-safeguards.md` "Pedal polarity".
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
  **Not done:** `USB_DEMO_VALIDATED_1P5A` (a manual-only override, never
  auto-selected -- belongs to a future `profiles/` module, not this
  one), any real current measurement (every budget here is a governance
  ceiling, not a live current reading), and this hasn't been
  hardware-tested against an actual USB unplug/external-power-plug
  transition yet -- only reasoned through against the documented truth
  table and confirmed to build/pass host tests.
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
- Everything else (per-pad Hall calibration, DIN MIDI, CV/gate) is not
  built yet.
