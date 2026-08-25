# services/

The behavioral layer: turns raw driver data into musical/expressive
events, and turns high-level intent into driver commands. Depends on
`drivers/` and `board/`; knows nothing about USB/MIDI transport.

Built: Hall scan, touch, lighting, buttons, pedal, note mapping (with
octave shift), the SW1/SW2 octave-shift button controller, real
player-controlled minigames (snake, brick breaker), touch+Hall
expression fusion (velocity/aftertouch), power source state, standby
idle animations (plus a power-saving state after 15 minutes), a
power-on boot animation, and per-pad haptic feedback -- see Status
below. Still planned: per-pad Hall calibration, X/Y tilt -> pitch/
timbre, storage glue.

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
  rode down with the USB-only ceiling), all 24 pads solid white at idle
  baseline by default, brightening toward the ceiling when `touch.c`
  reports that pad touched -- written immediately on a press-value
  change rather than waiting for the round-robin, so touch reads as
  responsive rather than laggy. Pad brightness ceiling reads live from
  `power.h`'s `tiles_power_get_state().led_brightness_ceiling_percent`
  (37% on USB-only, 75% once external power is confirmed) instead of a
  hardcoded constant -- underglow does not use this ceiling at all.
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
- `octave_control.h`/`.c` — done for V1: the default function of SW1
  ("-") and SW2 ("+") is octave shift down/up, one octave per press
  (rising edge, not while held), driving `note_map.c`'s shift above.
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
  plain amber "+"-shaped cross (a 2-column vertical bar crossed with a
  1-row horizontal bar, both centered) as a second flash, since a 4-row
  glyph has no room to draw "#" -- the letter always shows first for a
  moment (re-anchored on mode entry and on every key change) so the
  flash is never caught mid-cross. Underglow goes dark while this is
  showing. `tiles_octave_control_is_transpose_active()` lets `main.c`
  skip `standby.c`'s idle scan while this owns the pad grid, mirroring
  the existing `game_mode.c` gate.
  **Defers to game mode:** this module's scan runs unconditionally every
  tick with no gate of its own, and `game_mode.h`'s minigames (below)
  reuse SW1/SW2 as their own left/right controls -- without a check
  here, every in-game press would *also* silently fire an octave or
  transpose step underneath the game. `tiles_game_mode_is_active()` is
  checked at the top of the scan: while a game owns the buttons, this
  module only keeps its press-edge tracking current and does nothing
  else (button-LED writes were already a no-op in that state, see
  `game_mode.h`'s entry below for why).
  **Not hardware-verified:** the combo-hold threshold, both flash
  durations, the cross's row/column placement, and the amber accent
  color are all first-pass judgment calls, not measurements.
- `pixel_font.h`/`.c` — done for V1: a shared tiny pixel font, 4 rows
  tall (one pixel per pad row 1-4), used by both `standby.c`'s scrolling
  marquee animation and `octave_control.c`'s transpose key-letter
  display above -- pulled out of `standby.c` (where the glyphs used to
  live as a one-off, hand-guessed set with at least one real mistake: E
  and F were nearly indistinguishable, E was missing its bottom bar) so
  both callers share one already-checked font instead of each guessing
  its own. Format: one byte per glyph column, bit0 = row 1 (top) ...
  bit3 = row 4 (bottom). Covers exactly the letters needed -- A-G (the
  seven natural note names) plus I/L/N/S/T (for "SENTIA - TILES -"), a
  dash, and a space -- not a full alphabet, since nothing else uses this
  yet. `tiles_pixel_font_glyph_for_note_letter()` is the runtime lookup
  `octave_control.c` needs for a variable key letter; `standby.c`'s
  marquee references the glyphs directly since its message is fixed.
  **Not hardware-verified:** every glyph is hand-designed specifically
  for 4 rows (there's no established "4-row font" to have copied
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
  steps it down automatically (`GT_STEP_MS`). Standard tetromino
  shapes/colors; `gt_clear_lines()` shifts everything above a full row
  down (handles multiple simultaneous clears in one bottom-up sweep) and
  returns how many rows cleared, so `gt_lock()` can trigger a brief,
  fast-toggling white underglow strobe (`GT_LINE_CLEAR_FLASH_MS`/
  `_TOGGLE_MS`) only when something actually cleared -- real feedback:
  "the underglow must flash white dramatically when a line is cleared."
  Topping out (a freshly spawned piece already collides) ends the round
  with a plain red blink instead of the usual red/purple (real feedback:
  "when game is lost it should flash red") -- `gm_start_round_end()`
  grew a `red_only` parameter for this, since Tetris is the only game
  needing a different round-end color than snake/brick breaker.
  Pong: two players, one board -- column 1 is the left paddle (SW1 up,
  SW2 down), column 6 is the right paddle (SW5 up, SW6 down -- the
  mirror pair to SW1/SW2; **unverified** whether "square" is actually
  the button the user meant by "the other one next to circle"). Both
  paddles 2 pads tall and white; the ball is a single blue dot, checked
  before either paddle in the render order so it draws on top during a
  bounce (same precedent as brick breaker's ball). Deliberately does
  NOT go through the shared round-end flow below -- a rally on a board
  this small can end in a couple of seconds, so bouncing back to the
  menu on every missed point would be far more disruptive than useful.
  A miss instead flashes underglow white briefly (`gp_point_scored()`)
  and re-serves immediately, staying in `GM_STATE_PLAYING_PONG` the
  whole time; the only way out is the standard 4-button hold every game
  shares.
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
  the authoritative note on/off timing gate (more reliable to detect
  than inferring press/release from Hall depth alone); Hall supplies
  velocity (from peak acceleration observed during a short
  strike-detection window right after touch-down) and ongoing
  aftertouch (from press depth while held), sent as MIDI poly key
  pressure. Per-pad state machine: IDLE -> AWAITING_STRIKE (touched, not
  yet committed) -> NOTE_ON, with AWAITING_STRIKE cancelling back to
  IDLE (no note sent) if released before enough samples arrive.
  **Explicitly unmeasured, needs real-hardware tuning:** the
  acceleration->velocity scale, the depth->aftertouch full-scale range,
  and the strike-detection window durations -- all flagged as
  placeholders in `expression.c`, none derived from a calibrated
  mT/LSB relationship since that doesn't exist yet. Also drives
  `haptics.c` at the same three points it drives `midi_out.c` (note-on
  -> kick, note-off -> stop, aftertouch change -> sustain level) with
  the exact same velocity/aftertouch values, so haptic and MIDI output
  never disagree.
- `haptics.h`/`.c` — done for V1: per-pad feedback driven entirely by
  `expression.c`'s calls (not touch/Hall directly). Envelope: KICK
  (opens with a brief overdrive spike at max duty regardless of
  velocity, to overcome the motor's static friction/inertia fast, then
  settles to the velocity-mapped duty for the rest of the window) ->
  GAP (hard zero) -> silence. A kick may sit briefly in an internal
  PENDING state first if another kick started too recently -- see
  `KICK_STAGGER_MIN_GAP_MS` below.
  The continuous, aftertouch-mapped SUSTAIN phase (the motor ramping
  with press depth while held) is built but currently **disabled**
  (`TILES_HAPTICS_SUSTAIN_ENABLED 0`): on real hardware it read as
  continuous buzzing rather than the single, clean click the user
  actually wants, and the magnets aren't in their final position yet
  (same root issue as `standby.c`'s disabled Hall wake path -- see that
  entry above), so the aftertouch value it would ramp against is
  currently against meaningless Hall data. Every pad's envelope is
  currently just KICK -> GAP -> silence; re-enable SUSTAIN and
  re-evaluate once Hall is calibrated, rather than assuming uncalibrated
  data was the whole story.
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
  the ceiling is silently dropped, never blocks the MIDI note. Also
  enforces the hardware handoff's "stagger motor starts >= 15ms"
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
  **Not done:** every duty/timing constant (`KICK_DURATION_MS`,
  `KICK_OVERDRIVE_MS`, `KICK_GAP_MS`, `MIN_KICK_DUTY`,
  `MAX_SUSTAIN_DUTY`, `KICK_STAGGER_MIN_GAP_MS`) is an unmeasured
  placeholder -- no per-motor current/duty data exists yet (see the
  board map's `measured_current_required` TODOs). Not yet
  hardware-tested at all.
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
  9 rotating ambient animations, switching to a random one every 2
  minutes (also a starting guess, not tuned against how it actually
  feels to watch) -- excludes both the animation ending and the one
  before it, so a switch never immediately repeats itself and never
  bounces straight back to the animation two ago either:
  1. diagonal traveling wave
  2. a sharp, squared-contrast ring pulsing outward from center
  3. comet-tailed "shooting stars" with per-star randomized speed/tail/twinkle -- fall speed roughly halved (`STAR_SPEED_ROWS_PER_MS_MIN`/`_MAX`) from real feedback that it read as too fast
  4. an actual game of snake: starts 2 segments long (real feedback: 3 felt cramped on a board this small), a pulsing red food dot appears, the snake (green, brighter head) moves toward it a cell at a time and eats it (grows by one segment, a new dot appears) -- direction each step is greedy-toward-the-food with a randomized perturbation (`SNAKE_RANDOM_TURN_WEIGHT`) so the path varies run to run, and it resets to a short length at a randomized start position/direction whenever it grows past `SNAKE_MAX_LENGTH` or traps itself with nowhere to go, so it doesn't settle into one repeating pattern long-term either. Reworked from an earlier version that was just a fixed-length segment ping-ponging a deterministic path -- real feedback was that it didn't feel like an actual game.
  5. a blue/purple RGB showcase where underglow shows the same moving color as the pads -- the whole point being to show off both
  6. a graphic equalizer: each column is a fake EQ/VU bar, bottom-up blue/blue/yellow/red, with a per-column red "peak-hold" marker that sticks at the highest segment reached and falls over about 2 beats (`EQ_PEAK_DECAY_PER_MS`) independent of the bar's own motion -- classic hardware-equalizer behavior. Reworked twice: an early pass was too fast/continuously lit, the fix for that (a long, low-biased "phrase" envelope forcing multi-second silences) overshot into "too slow" with "almost no peaks"; the current version instead drives every column with a percussive, tempo-locked hit envelope -- instant peak then decay, at a per-column whole-number subdivision of one shared 127bpm beat (`EQ_BEAT_MS`) so bass columns (`s_eq_col_hits_per_beat` = 1/beat) ring out slowly and treble columns (4/beat) snap back fast, all still locked to the same underlying pulse ("should feel like a song at 127bpm pumping"). A deterministic golden-angle-stepped "miss" occasionally drops a hit to 0 for some breathing room, without the old envelope's long silences killing density. Function buttons are fully off; underglow is a constant blue accent unrelated to any one column.
  7. a circular underglow wave: only underglow moves, a wave traveling around the 4 pixels in their actual physical circular order (`g_tiles_underglow_circular_position` in `board/board_layout.h` -- chain order 0,1,2,3 zigzags diagonally, the real ring order is 0,1,3,2), each pixel rising and dimming significantly as the wave passes through, going around and around; pads/buttons sit at a flat, minimal, non-animated brightness.
  8. brick breaker: the function-button row is a wall of bricks, a 3-pad-wide cyan paddle (bottom pad row) tracks a warm-white/yellow ball with simple AI (moves at most one column per step toward the ball), the ball bounces around knocking orange bricks out until every brick is broken (won) or it gets past the paddle (lost) -- either way underglow flashes red and purple for a few seconds, then a fresh round starts. Ball checked before paddle in the render order so it draws on top during a bounce, when they briefly occupy the same cell.
  9. a scrolling marquee: "SENTIA - TILES - " scrolls across the pad grid using the shared font in `services/pixel_font.h`/`.c` (see its own entry above), with automatic inter-glyph spacing and seamless wraparound (`marquee_total_width()`) rather than a fixed animation; underglow and function buttons both stay off, keeping it purely a pad-grid text effect. Scroll speed slowed (`MARQUEE_MS_PER_COLUMN` 260ms -> 420ms per column) from real feedback, at the same time the font itself moved out to the shared module to fix a real mistake in the old one-off glyphs (E and F were nearly indistinguishable).
  10. bouncing glow: the "simple but elegant" one -- a single soft white point bounces diagonally around the pad grid like a screensaver ball, purely a closed-form position (a triangle wave per axis -- a bounce-off-the-walls reflection with no velocity/state to track) with a soft falloff around it, no particle array or game state at all. Row and col bounce at different, non-integer-ratio periods (`BOUNCE_ROW_PERIOD_MS`/`BOUNCE_COL_PERIOD_MS`) so the path slowly traces a Lissajous-like figure instead of repeating quickly. Function buttons stay off; underglow mirrors the pad field like animations 1-4, so the glow naturally spills into it near an anchor.
  11. Tetris: the AI-played autonomous counterpart to `game_mode.h`'s real Tetris (below) -- same standard tetromino set/colors, deliberately separate state and code from the interactive version, matching this file's existing snake/brick-breaker precedent. A lightweight greedy AI (`tetris_ai_place()`) picks each piece's rotation and column at spawn by simulating every fitting placement and keeping whichever lands the piece's topmost cell deepest (a cheap "keep the stack low" proxy, no real hole-counting), then the piece visibly falls one row at a time toward that spot. A line clear triggers a brief, fast-toggling dramatic white underglow strobe (`TETRIS_LINE_CLEAR_FLASH_MS`/`_TOGGLE_MS`); topping out instead blinks plain red (not the red/purple alternation brick breaker's flash uses), then the well clears and a new game starts. Function buttons stay off.
  12. Pong: the AI-vs-AI autonomous counterpart to `game_mode.h`'s real, two-player Pong (below) -- same court/paddle/ball layout and colors, deliberately separate state and code. Both paddles use the same "move at most one row per step toward the ball" simple AI animation 8's paddle already established, so rallies essentially never end on their own; on the rare miss, a brief white underglow flash plays and the ball re-serves immediately -- the same "stay in this animation and continue" behavior the interactive version uses instead of a win/lose round-end. Function buttons stay off.

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
  sensing, not confirmed.
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
  After `TILES_STANDBY_POWER_SAVING_TIMEOUT_MS` (15 minutes of *total*
  inactivity, not 15 minutes of animation specifically -- same
  `s_last_activity_ms` clock that gates entering standby in the first
  place, just a longer threshold checked while already in standby)
  standby's animations stop and the board drops to a third state,
  power-saving: everything dark except the circle button (SW6, the
  rightmost) pulsing gently to show how to wake it. Same wake conditions
  as standby.
  **Not done / not hardware-verified:** the button-column and
  underglow-anchor mappings in `board/board_layout.h` are based on the
  user's verbal description of the physical board, not a hardware doc
  (checked: not documented in `docs/hardware/`) -- easy to correct there
  if the real LED1-4 order or button alignment turns out different once
  seen lit. The animation frame rate (~25fps) and every animation's own
  timing constants (including the new power-saving pulse period and the
  15-minute timeout itself) are unmeasured against real I2C bus load /
  how it actually looks. Animations 1-3, 5-7 and the power-saving state
  have been seen on real hardware in some earlier form (several already
  reworked from that feedback, including animation 3's fall-speed halving
  above); animation 4's real-snake rework and animations 8 (brick
  breaker), 9 (marquee, including its font move to `pixel_font.h`/`.c`),
  10 (bouncing glow), 11 (Tetris, including its line-clear/loss flash
  colors), and 12 (Pong) have NOT been seen at all yet -- their
  AI/pathing/step timing, the shared pixel font (hand-designed, not
  measured against how legible it actually is at 4 pixels tall), brick
  breaker's/Tetris's/Pong's paddle-or-placement AI reaction, and bouncing
  glow's periods/radius are all first attempts.
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
