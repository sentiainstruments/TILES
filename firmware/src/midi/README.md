# midi/

Musical output only — never carries config/calibration traffic (that's
`usb_vendor/`).

Contents: USB-MIDI + MPE channel allocation (dynamic, 15 lower-zone member
channels across 24 pads, deterministic voice-steal policy), and DIN MIDI IN
(GP1 UART, 31,250 baud) / DIN MIDI OUT (polarity-selectable via GP0/GP2, PIO
UART) with rate limiting for continuous expression data — see Status below.

## Status

- `tusb_config.h`, `usb_descriptors.c`, `usb_device.{h,c}` — done. A
  composite CDC (diagnostics console) + MIDI USB device, modeled on
  pico-sdk's own `pico_stdio_usb` reference config and TinyUSB's
  `cdc_msc`/`midi_test` example descriptor patterns rather than
  hand-built from scratch. `firmware/src/CMakeLists.txt` links
  `tinyusb_device` explicitly, which makes `pico_stdio_usb` defer both
  `tusb_init()` and USB descriptor provision to us
  (`LIB_TINYUSB_DEVICE`-gated, pico-sdk's own documented mechanism for
  this) -- see the comment in `tusb_config.h` for the full reasoning.
  That same gate also disables pico_stdio_usb's automatic background-IRQ
  `tud_task()` servicing, and a real bug slipped through as a result:
  nothing anywhere in this firmware called `tud_task()`, so the USB
  stack was never actually serviced past whatever the low-level
  enumeration ISR handles on its own -- found while chasing why the
  USB-CDC debug console printed nothing at all on real hardware. Fixed:
  `main.c` now calls `tud_task()` at the top of every main-loop
  iteration; see its call site and `usb_device.h`'s updated header for
  the full explanation. This plausibly also explains why USB MIDI below
  has never been confirmed working in a DAW (queued
  `tud_midi_stream_write()` bytes need `tud_task()` to actually reach
  the host) -- not proven yet, but a real hardware test of MIDI note
  output is now worth retrying specifically because of this fix.
- `midi_out.{h,c}` — done: real MPE (MIDI Polyphonic Expression), a
  single Lower Zone -- channel 1 is the Zone Master Channel (carries only
  the zone-configuration RPN messages `tiles_midi_mpe_init()` sends once,
  the first main-loop iteration `tud_midi_mounted()` reads true after
  boot or a remount, never note data itself), channels 2-16 are Member
  Channels, one per currently-held note. Added after real feedback: "we
  need to make sure we have individual per note pitch bend not just
  regular all key pitch bend. like the roli seaboard." Every function
  here (`tiles_midi_note_on/off(channel, ...)`,
  `tiles_midi_send_channel_pressure(channel, ...)`,
  `tiles_midi_send_pitch_bend(channel, ...)`,
  `tiles_midi_send_cc(channel, ...)`) takes an explicit channel -- this
  file is only the wire-protocol layer, it doesn't decide which channel
  a note gets. `services/expression.c` owns that: its per-pad MPE channel
  allocator (`claim_mpe_channel()`/`end_held_note()`) claims a free
  Member Channel the instant a strike commits and frees it (always
  centering pitch bend first, so a reused channel never inherits a
  stale bend) at note-off/retrigger, stealing the oldest-claimed channel
  -- forcibly ending THAT note cleanly first -- if all 15 are already in
  use (a real possibility on a 24-pad board, mirrors
  `services/haptics.c`'s own voice-stealing policy almost exactly).
  Pitch bend and pressure are now genuinely independent per note,
  each on its own channel -- no more single-"owner"-pad workaround; see
  `services/expression.c`'s "Pitch bend from sideways motion" section for
  the fuller history of what that workaround used to be and why MPE
  removes the need for it entirely.
  Real feedback after the first MPE flash: "you broke mpe preassure." This
  used to send Poly Key Pressure (0xA0, note-addressed) for the per-note
  pressure dimension; MPE's actual convention is Channel Pressure (0xD0,
  no note field needed, a 2-data-byte message unlike everything else in
  this file) since a Member Channel already is one note. Fixed by
  switching to `tiles_midi_send_channel_pressure()`.
  `tiles_midi_send_cc_broadcast(controller, value)` exists alongside the
  single-channel `tiles_midi_send_cc()` specifically for
  `services/pedal.c`'s sustain/expression CCs -- under MPE there's no
  single "right" channel for a pedal message that needs to reach every
  currently-sounding note, so it's sent to the Zone Master Channel and
  all 15 Member Channels at once.
  Declares a 12-semitone (one octave) Member Channel pitch bend range via
  RPN 0 -- purely a receiver-side interpretation setting, independent of
  `services/expression.c`'s own sensitivity tuning for how much raw wire
  value a given tilt produces. History: 48 (the MPE spec's own
  recommended default, what a real ROLI Seaboard ships with) -- real
  feedback: "the pitch bend is so extreme the glide in equator is too
  extreme." 48 semitones is 4 octaves of swing at full-scale wire value,
  which a comfortable deliberate tilt reaches; lowered to 12 as a more
  reasonable middle ground for an expressive per-note glide (see
  `midi_out.h`'s own comment).
  **Not hardware-verified at all** -- the whole MPE implementation
  (zone-config RPN messages, per-note channel allocation, channel
  stealing) has not been tried against a real MPE-aware DAW/synth yet.
- **Found investigating real feedback on the ongoing crash
  investigation: "i found what causes it, its when ableton is
  playing... i think it has to do with midi clock or tap tempo,
  something is clashing and causing those crashers only on play."**
  Traced `services/midi_clock.c`'s own receive loop and
  `services/op_mode.c`'s sequencer engine (`seq_advance_clock()`,
  ratchet handling, the tap-tempo generator) end to end looking for an
  unbounded loop or blocking wait that would only bite once a real
  clock is actually flowing -- all of it is correctly bounded (guard-
  counted ratchet/virtual-pulse loops, O(1) step-advance arithmetic,
  and the tap-tempo generator explicitly disables itself the instant
  `tiles_midi_clock_external_active()` is true, i.e. exactly while
  Ableton is playing) -- nothing there hangs.
  Did find a real, separate bug in this file while looking: every
  `send1()`/`send2()`/`send3()` call ignored `tud_midi_stream_write()`'s
  return value. Confirmed reading TinyUSB's own `midi_device.c` that
  this function only writes as many bytes as currently fit in its
  64-byte TX FIFO and returns early otherwise -- under any real
  backpressure (which "Ableton playing" plausibly causes indirectly:
  once its clock actually starts a pattern, THIS device's own outbound
  MIDI stops being occasional touch-driven notes and becomes
  continuous sequencer output, several lanes at once), a message could
  go out missing its tail bytes -- a Note-On with no velocity --
  silently corrupting the stream with nothing here ever aware it
  happened. Fixed by logging every truncated write (`warn_if_
  truncated()`) instead of retrying or pumping `tud_task()` to force
  room -- a retry-until-room loop is exactly the failure class this
  whole session's other fixes (I2C, CDC) have been about closing, not
  a shape worth reopening here. Explicitly NOT claimed as the crash's
  root cause -- a dropped byte corrupts output, it doesn't freeze this
  device by itself -- kept as its own real, independent fix.
  Current leading hypothesis, extending this session's own earlier,
  separately-confirmed E15 finding (see `services/README.md`'s own
  history) rather than a new, unrelated guess: E15 is specifically a
  Bulk-IN (device-to-host) race across SOF interrupts, and "Ableton
  playing" is precisely the condition under which this device's own
  Bulk-IN traffic goes from occasional (touch-driven) to continuous
  (sequencer output, once real clock actually starts it) -- more
  sustained Bulk-IN activity gives a rare per-transfer race far more
  chances per second to actually land. Not proven with the same kind
  of hard evidence the earlier USB-disconnect kernel-log finding had --
  the concrete next step is capturing a debug-mode trace (hold
  diamond+square+circle 8s BEFORE the next Ableton-playing test) from
  the next actual crash, so the auto-dumped last-known trace character
  says definitively where it hung rather than continuing to reason
  about it from code alone.
- **The truncation above turned out to be a real, separately confirmed
  bug of its own, not just a hypothesis.** Real feedback, precisely
  reproduced: "pedal only sticks when you release the note but hold
  pedal and then release it." `tiles_midi_send_cc_broadcast()`
  (`services/pedal.c`'s own sustain-off send) fires 17 back-to-back
  3-byte CC messages (51 bytes) in one call -- comfortably enough on
  its own to overflow the 64-byte TX FIFO if a note-off from releasing
  a pad moments earlier is still sitting in it, exactly the gesture
  that reproduces the stick: whichever Member Channel's CC64=0 landed
  on the truncated tail of that burst never reached the synth, so that
  one note stayed sustained even after the pedal genuinely released,
  while every other channel that fit released fine. Fixed with a
  bounded retry (`send_with_retry()`, `MIDI_SEND_RETRY_TIMEOUT_MS` =
  5ms) instead of the plain log-and-drop above -- still not an
  unconditional retry-until-room loop (the exact shape the original
  fix above was deliberately avoiding): it pumps `tud_task()` (the only
  thing that actually drains the TX FIFO to the host at all, see
  `main.c`'s own main-loop comment) and retries the remaining bytes,
  bounded by a real wall-clock deadline -- long enough to ride out an
  ordinary multi-message burst like the broadcast above, short enough
  that a genuinely absent/stalled host still returns promptly instead
  of hanging the main loop. `warn_if_truncated()` still logs anything
  that couldn't be recovered even after the retry window, so a
  genuinely stalled host stays visible rather than silently eaten.
- **Sustain pedal: the retry fix above still didn't resolve the real
  stuck-note reports** ("you literally killed all pedal functionality,"
  turned out to mean the same stuck-on-release behavior, unchanged).
  Asked to research how sustain is properly implemented rather than
  keep guessing from this codebase's own reading alone. Two real
  findings from that research, both against primary/authoritative
  sources, not forum speculation:
  - The MPE specification (`mpespec.pdf`, MIDI Association/ROLI, and
    corroborated by JUCE's own MPE documentation) says messages meant
    to affect every sounding note -- Damper Pedal/CC64 explicitly named
    -- "should be sent only on a Zone's Master Channel (not on Member
    Channels)," and a compliant MPE synth "must ignore" CC64 received
    on a Member Channel. `tiles_midi_send_cc_broadcast()`'s existing
    "broadcast to every channel, Master included" behavior (this
    file's own header comment) already covers a non-MPE-aware receiver
    too, so this wasn't itself changed -- an MPE-compliant receiver
    already gets the correct Master-Channel message and is spec-
    required to ignore the redundant Member-Channel copies.
  - The real, actionable finding: "a proper sustain implementation
    should prevent Note Off messages from being sent while Sustain
    (CC64) is held, but keep track of them so that when the Sustain
    pedal is released, all the pending Note Off messages get sent" --
    i.e. the CONTROLLER should defer the note-off itself, not send it
    immediately and trust the receiving synth to notice CC64 is still
    held and keep the note ringing on its own. This codebase's
    `services/expression.c` did the latter -- and "sticking midi
    notes"/hanging-note reports across many real DAWs and synths for
    this exact scenario (note-off arriving while sustain is held) are a
    well-documented, common failure mode, not something unique to
    whatever synth this board happened to be tested against. See
    `services/README.md`'s own "Real fix for the sustain-pedal stick"
    entry for the actual implementation (deferred note-off, flushed on
    pedal release) -- this file's own `tiles_midi_send_cc_broadcast()`
    and `tiles_midi_note_off()` needed no changes themselves; the fix
    is entirely in when `services/expression.c` chooses to call the
    latter.
- **`midi_in.c` gained a real, running-status-aware channel-voice
  parser** (Note-On/Off specifically dispatched; every other channel-
  voice type consumed correctly for byte alignment but not dispatched
  anywhere). Real feedback: "in midi melodic mode is there any way we
  could read the playing melody of the armed track and display it back
  on tiles?" -- exactly the "not-yet-built feature" this file's own
  header used to flag. `tiles_midi_in_register_note_callback()` mirrors
  the realtime/SysEx registration functions exactly; see
  `services/README.md`'s own entry for the one registered listener
  (`op_mode.c`'s melodic-mode "live echo" feature).
  Two real correctness pieces needed for this to actually work, not
  just the Note-On/Off dispatch itself:
  - Running status: a sender (Ableton's own MIDI output included) can
    legally omit a repeated status byte between consecutive messages of
    the same type/channel (e.g. a stream of Note-Ons) -- the parser
    tracks the current status byte and how many data bytes its message
    needs, dispatching a complete message each time enough data bytes
    arrive, with no repeated status byte required.
  - The System Real-Time check at the top of the scan loop only ever
    fired callbacks for the four bytes this file already cared about
    (Clock/Start/Continue/Stop), but silently let the other four
    (Undefined 0xF9/0xFD, Active Sensing 0xFE, Reset 0xFF) fall through
    into whatever state machine was active below -- harmless before
    (nothing was tracking state byte-by-byte outside SysEx), but wrong
    now: those four are still System Real-Time bytes, legally injected
    ANYWHERE in the stream without disturbing anything around them per
    the MIDI spec, and would otherwise have been misread as either a
    channel-voice status byte (silently canceling running status) or an
    abort of an in-progress SysEx frame. Fixed by widening the skip to
    the full 0xF8-0xFF range while still only firing a callback for the
    original four.
- **`tiles_midi_in_activity_count()`** -- monotonic count of meaningful
  MIDI events (Note-On/Off and the four Real-Time Clock/Start/Continue/
  Stop bytes; not SysEx, stray data bytes, or Active Sensing), polled by
  `services/standby.c` so incoming MIDI holds off / wakes an automatic
  screensaver. Real feedback: "no screensaver can activate if ableton is
  playing or midi is being recieved." Polled counter rather than a
  fourth registered callback -- the callback tables are a fixed 4 each
  and this needs no per-event payload.
- **DIN MIDI IN/OUT** (`din_midi.{h,c}`, `din_midi_queue.{h,c}`,
  `din_midi_tx.pio`) -- built, **never tried against real DIN/TRS gear**.
  Real feedback: "are midi plugs working?" -- no, honestly: docs and
  `board_init.c` had reserved GP0/GP1/GP2 (outputs parked high, RX a plain
  input) but nothing ever sent or read a byte -- then "yes build DIN MIDI."
  What it does:
  - **IN (GP1).** Hardware UART0 RX, 31,250 8N1, drained by an interrupt
    into a 256-byte ring (`din_midi_queue.c`); the UART FIFO stays on as
    ~10 ms of slack (interrupt threshold lowered to 4 bytes so short
    messages aren't left to the ~1 ms receive timeout). Framing/break/
    overrun errors discard the byte and flag a loss. `midi_in.c` parses it
    with its OWN per-source parser state (two cables interleaving into one
    running-status parser would splice half a Note-On onto another's data
    bytes) and fires the SAME callbacks as USB, so clock/transport, the
    melodic echo on the pads, and Scene Launch SysEx react to DIN exactly
    as to USB -- e.g. a hardware drum machine's clock drives the sequencer,
    and a keyboard on the jack lights the pads. On a reported loss the DIN
    parser drops any half-assembled message and ignores stray data bytes
    until the next status byte. **Clock ownership:** Real-Time bytes only
    reach the callbacks from the source that currently owns the clock (first
    to send one; kept while it keeps sending; released after 500 ms of
    silence) -- a DAW on USB and a drum machine on DIN both sending 24 PPQN
    would otherwise count 48 pulses/beat and double the tempo.
  - **OUT (GP0/GP2).** `midi_out.c` sends every performance message to DIN
    as well as USB: notes, pitch bend, channel pressure, CCs (sustain,
    expression, the MPE zone RPNs), Start/Stop. DIN works **with no USB
    host at all** (the handoff's external-power-only mode), and `main.c`
    also sends the MPE zone configuration once at boot for that case.
    **Not mirrored, USB only:** SysEx (the Ableton remote script's private
    protocol) and a new `tiles_midi_send_daw_cc()` that all 17 DAW-control
    CCs in `op_mode.c` (transport Play/Stop/Record, every Scene Launch grid/
    stop/offset/delete/capture CC) now use -- they ride channel 1 with
    controller numbers a hardware synth may have mapped, so mirroring them
    would have made pressing transport or a scene pad twiddle whatever is
    on the jack.
  - **Transmitter.** `din_midi_tx.pio`, the reference uart_tx shape (8 PIO
    cycles/bit, clkdiv = sys_clk / 250,000, exactly 600 at 150 MHz), fed
    from the PIO TX-FIFO-not-full interrupt so the wire runs at full speed
    regardless of main-loop timing. One pin carries the data while the other
    line is parked high through plain GPIO: MIDI's current loop only flows
    when the two lines differ, so **which line carries the data IS the TRS
    polarity**. `TILES_DIN_MIDI_OUT_DEFAULT_LINE` (`din_midi.h`) picks it --
    default line A (GP0). The hardware handoff doesn't say which line is
    physically Type A vs Type B, so the default is a first guess: **if a
    receiver hears nothing, flip that constant** (or call
    `tiles_din_midi_set_out_line()`). Not persisted yet -- `storage/` is
    unbuilt (its README lists "DIN MIDI OUT polarity" as a future setting).
  - **Rate limiting** -- the reason for `din_midi_queue.c`. DIN is 3,125
    bytes/s (~1 ms per 3-byte message); MPE expression and the expression
    pedal (one CC broadcast to 16 channels = 48 bytes per step) can out-run
    that easily. So Note On/Off, sustain and every other CC are *reliable*
    (queued in order, 512-byte ring, never merged); pitch bend, channel
    pressure and CC 1/11/74 are *coalesced* (latest value per channel/kind
    wins, sent only while the ring holds <= 24 bytes, so expression never
    queues in front of a note); pending coalesced values on a channel are
    flushed before any reliable message on that channel, so "bend to center,
    then Note-Off" arrives in that order; Start/Stop use a separate
    Real-Time queue that jumps ahead. A reliable message that doesn't fit is
    dropped whole (never half a message) and counted.
  - **Verification.** `din_midi_queue.c` and the two-source parser were
    compiled natively and tested off-target (ordering, coalescing,
    overflow, wraparound, Real-Time priority; interleaved partial USB/DIN
    messages, running status per source, clock ownership, loss recovery,
    SysEx from DIN). The assembled PIO program was run through a small
    instruction-level simulation and decoded as 8N1, LSB first, 8 cycles/
    bit, glitch-free, idling high. **Not verified: the electrical side** --
    the real jacks, the buffer/opto, TRS polarity, and the interrupt-fed FIFO
    under real load. A loopback (MIDI OUT cable into MIDI IN) is the
    quickest first test.
  - **Not built (raised, not asked for):** MIDI thru/merge (DIN in -> USB
    out or -> DIN out), a channel-collapse mode for non-MPE hardware (all
    15 member channels are sent as-is, so a single-channel synth only hears
    the notes that land on its channel), and running status on output.
