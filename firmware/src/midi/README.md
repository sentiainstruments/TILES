# midi/

Musical output only — never carries config/calibration traffic (that's
`usb_vendor/`).

Contents: USB-MIDI + the MPE wire protocol (an MPE Lower Zone of up to 8
Member Channels, 2-9 -- the channel plan is `services/midi_channels.h`, the
per-note channel choice `services/mpe_alloc.h`), and DIN MIDI IN (GP1 UART,
31,250 baud) / DIN MIDI OUT (polarity-selectable via GP0/GP2, PIO UART) with
rate limiting for continuous expression data — see Status below.

## Status

- **USB reset interface (`usb_descriptors.c`, `tusb_config.h`)** -- a fourth
  composite interface (control transfers only, no data endpoints) that lets
  `picotool` reboot this board into the ROM bootloader over USB. Real
  feedback: "will we be able to flash updates without putting the board in
  bootloader mode" -> "yes add the software reboot command." Standard pico-sdk
  library (`pico_usb_reset`), not custom protocol: class 0xFF, subclass 0x00,
  protocol 0x01 -- the exact signature `pico_stdio_usb` sets up automatically,
  added here by hand because this composite device owns its own descriptors
  (see this file's own header comment on why). `picotool load -f`/`reboot -u`
  already know it with no configuration. Doesn't collide with the settings
  vendor interface just above it: TinyUSB matches interfaces to class drivers
  by exact subclass+protocol, and app-registered drivers (this one) are tried
  before the built-in vendor class, so a plain vendor interface (subclass 0,
  protocol 0) still falls through to the built-in driver untouched. Also
  reachable without `picotool` at all, over the settings shell (`usb_vendor/`,
  `shared/protocol/README.md`): `REBOOT BOOTSEL` / `REBOOT APP`.

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
  - **The note-off half of the above was later reversed** after the
    stick still reproduced in Equator and Serum ("there is a glitch in
    pedal release and youre nbot catchingit. look online and also look at
    the code"): per the MPE spec (MMA RP-053 v1.0, sections 1.3 and 3.3)
    the controller now sends Note-Off immediately on release and lets the
    synth sustain -- the deferral was the misread. Pedal CCs now go on
    the Master Channel only (plus the fixed non-zone parts) -- strict MPE,
    a deliberate decision ("yes go strict") after one round showed a rig
    not actually configured for MPE needs the broadcast to sustain at all
    ("weve fully lost pedal"); the host must now be set to MPE, or use
    non-MPE mode. `tiles_midi_send_cc_broadcast()` is panic-only. Full
    writeup: `services/README.md`'s "SUPERSEDES the two sustain entries
    above" entry and the two after it.
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
    polarity**. **Fixed at TRS Type A**, never switched automatically --
    real feedback: "dont auto flip select type A for now"
    (`TILES_DIN_MIDI_OUT_DEFAULT_TYPE` in `din_midi.h`), which is also what
    `docs/architecture/defaults-and-safeguards.md` already specifies ("Default:
    Type A TRS polarity... Selectable via profile (GP0/GP2 role swap), not
    auto-detected -- there's no way to sense polarity from the jack side").
    Only an explicit `tiles_din_midi_set_trs_type()` call changes it, and
    nothing calls that yet (it's for the future profile setting; `storage/`
    is unbuilt, its README lists "DIN MIDI OUT polarity" as a future
    setting). **One assumption to confirm on hardware:** the docs never say
    which of GP0/GP2 is physically Type A; the code follows the handoff's
    "line A / line B" naming (Type A = GP0). If a Type A receiver hears
    nothing, that mapping is the thing to check, not the transmitter.
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
  - **Running status on output -- built.** Real feedback: "before bnooting
    look into what actually is standardized or good practice in this
    industry that we havent implemented yet" -> "4. fix it." Standard MIDI
    1.0 bandwidth optimization, worth doing here specifically (not on
    USB -- see below) because 31,250 baud is the one output this firmware
    actually has a byte-rate to save. `din_midi_queue.c`'s `tx_push()`
    tracks the status byte of the last message it actually wrote to the
    ring and omits a repeated one -- correct because everything else this
    queue can enqueue there either can't cancel running status by spec
    (Real-Time bytes, routed through the entirely separate RT queue) or is
    already rejected before reaching it (SysEx, System Common). Applies
    per STATUS BYTE, not per message content -- two different CCs on the
    same channel (mod then slide, say) legitimately compress into each
    other, exactly as a real MIDI receiver expects. Deliberately NOT
    applied to USB: `tud_midi_stream_write()`'s own USB-MIDI class packets
    each carry an explicit Code Index Number regardless of the underlying
    byte stream, so there's no equivalent saving to make there, and a
    status-less send wouldn't even be a valid USB-MIDI Event Packet.
  - **Not built (raised, not asked for):** MIDI thru/merge (DIN in -> USB
    out or -> DIN out). (The channel-collapse gap once listed here is
    covered by the `expression.mpe_enabled 0` setting since: every note on
    channel 1.)
- **Three more items from that same "what's standardized that we haven't
  built" research, all now built:**
  - **Active Sensing on DIN output.** A DIN receiver has no way to know
    the cable itself didn't die -- MIDI 1.0's own answer is Active
    Sensing (0xFE): once a device has sent it at all, it commits to
    sending SOME byte at least every 300 ms, and a receiver that then
    hears nothing for ~300 ms is spec-licensed to assume the connection
    dropped and release every note it's holding, rather than a stuck
    note lasting forever. USB-MIDI doesn't need this (the class itself
    reports device presence), which is why this is DIN-only, same
    scoping logic as running status above. `din_midi.c` tracks the last
    time it actually put a byte on the wire (`din_note_activity()`,
    called from both `tiles_din_midi_send()` on a successful queue push
    and the new idle path) and `tiles_din_midi_service()` -- already
    polled every main-loop iteration for the queue drain -- pushes a
    single 0xFE once 250 ms have passed with nothing else sent, a safe
    margin under the 300 ms ceiling. Real-Time bytes bypass the reliable/
    coalesced ring entirely (see the rate-limiting section above), so
    this never competes with or gets stuck behind a note or CC.
  - **Identity Request/Reply (Universal SysEx, `midi/identity.{h,c}`,
    new files).** Part of MIDI 1.0 since the original spec: a host or
    DAW can send a Non-Realtime Universal SysEx (0xF0 0x7E 0x7F 0x06 0x01
    0xF7, General Information / Identity Request, unaddressed) and expect
    an Identity Reply naming the manufacturer, model and version back --
    what lets a DAW auto-detect "a SENTIA TILES is connected" instead of
    a human picking it from a device list by name alone. Uses the MMA's
    own reserved non-commercial manufacturer ID (0x7D), since this board
    has no registered commercial SysEx ID. Wired up as a new SysEx
    listener alongside the existing Scene Launch one (`tiles_midi_in_
    register_sysex_callback()`) -- `identity_on_sysex()` ignores anything
    that isn't exactly the 4-byte Identity Request header and only ever
    replies over USB (`tiles_midi_send_sysex()`); a DIN reply would need
    its own SysEx framing on `din_midi_tx`, not built, since nothing on a
    hardware synth jack is expected to probe a controller's identity the
    way a DAW does. `main.c` calls `tiles_midi_identity_init()` once at
    boot, right after `tiles_midi_in_init()`.
  - **MIDI panic (`tiles_midi_send_panic()`, `midi_out.{h,c}`).** Real
    feedback: "panic should be forced sleep with shift button. like that
    action sends a panic note off." All Notes Off (CC 123) then All
    Sound Off (CC 120), broadcast the same full 2-16 channel range as
    `tiles_midi_send_cc_broadcast()` above -- both are standard MIDI 1.0
    Channel Mode messages, sent together because CC 123 alone is only
    a polite "release your notes" that a synth with its own sustain/
    envelope can still choose to interpret loosely, while CC 120 is the
    harder "stop making sound now." Deliberately independent of this
    device's own per-pad/per-channel note-tracking state (`services/
    expression.c`'s MPE channel allocator, chord/sequencer/Song's own
    sounding-note bookkeeping) -- a panic exists precisely for the case
    where that bookkeeping and what the receiver is actually doing have
    diverged, so it broadcasts unconditionally rather than only for
    channels this device currently believes are sounding.
    `services/standby.c` fires it from the manual shift+circle-held
    forced-sleep gesture specifically (see that file's own entry in
    `services/README.md` for why not the automatic idle-timeout sleep
    too), right before `enter_deep_sleep()`.
- **MPE sender conformance (standardization round).** Real feedback: "what
  else does it look like we need to fix for standarization and
  cokmpatibility" -> "do all". The wire-level half of the change logged in
  `services/README.md` ("Standardization round: MPE sender conformance"):
  - New `tiles_midi_send_note_setup(channel)`: pitch bend center + Channel
    Pressure 0 right before a Note-On (MPE spec 3.3.1/3.3.4), each skipped
    if the last value this file sent on that channel is already the
    default. Replaces the old recenter-before-Note-Off, which snapped every
    release tail. `tiles_midi_send_pitch_bend()`/`_channel_pressure()`
    record the last value per channel; a zone declaration forgets them (a
    receiver resets a channel's controllers when it enters or leaves a
    zone).
  - `tiles_midi_mpe_init(n)` is now the ONLY way to send RPN 6: `n > 0`
    always carries the Pitch Bend Sensitivity RPN 0 with it (JUCE-based
    receivers reset the bend range to 48 on every RPN 6), `n == 0`
    withdraws the zone. The old RPN-6-only `tiles_midi_send_mpe_zone_size()`
    is gone -- the mid-session re-declaration that used it is what skipped
    RPN 0.
- **Product identity: USB ID, firmware version, SysEx ID in one place
  (`product_identity.h`), plus Windows driverless access.** Real feedback:
  "6-7 lets do whatever makes sense rn for a non registered product thats
  in pre production." Found doing it:
  - **The USB product ID belonged to someone else.** `0x2E8A:0x100A` was
    Raspberry Pi's vendor ID with a product ID this project picked itself
    -- and Raspberry Pi's own allocation list (github.com/raspberrypi/
    usb-pid) gives `0x100A` to Pimoroni's Plasma 2040. Now the pid.codes
    TEST ID `0x1209:0x0001`, which exists for exactly this: in-house
    testing, never on a unit that's given out or sold. The header says how
    to get the real one (Raspberry Pi hands out free product IDs to
    commercial RP2350 products). `tools/tiles_control.py` and
    `tools/flash.sh` still accept the old ID so an older board can be
    updated.
  - **`picotool load -f` never finding board 2, solved.** picotool only
    recognizes a running board by itself when it uses one of Raspberry
    Pi's stock product IDs; for any other ID it needs `--vid/--pid`, then
    finds it through the reset interface and follows it across the reboot
    by serial number. Confirmed on board 2 (`picotool info -f --vid 0x2e8a
    --pid 0x100a` rebooted it into BOOTSEL and back). `tools/flash.sh`
    wraps it; `firmware/AGENTS.md` has the details.
  - **Firmware version** `TILES_FW_VERSION_*` (0.1.0): USB `bcdDevice`
    (was a fixed 0x0100) and the MIDI Identity Reply's version bytes (were
    a hand-kept 0.0.0.1).
  - **SysEx manufacturer ID** `TILES_SYSEX_MANUFACTURER_ID` stays `0x7D`
    -- the MIDI Association's non-commercial/development ID is the right
    placeholder until SENTIA registers one -- but is now one constant; the
    Identity Reply and Scene Launch each had their own copy.
  - **Windows**: bcdUSB 2.1 with a BOS descriptor and a Microsoft OS 2.0
    descriptor set, so Windows loads its WinUSB driver for the settings
    interface (the future companion app's) and the picotool reset
    interface by itself, instead of showing "unknown device" and needing a
    Zadig driver install. Same mechanism pico-sdk uses for the reset
    interface. Verified on macOS (enumerates as 1209:0001, bcdDevice
    0.1.0, MIDI/console/settings all working after the flash); NOT yet
    tested on Windows.
- **Two USB MIDI ports: "MIDI" (the instrument) and "DAW" (the Ableton
  script's).** Real feedback: "do 8 as how standardized stuff works.
  production ready industry stuff" -- item 8 of the standardization audit
  being that DAW control shared the instrument's port and channel 1. Every
  controller that also drives a DAW (Launchkey, Push, KeyLab) gives the
  DAW's control-surface script its own port; TILES's shared one is how the
  script ended up owning the sustain pedal (daw-integration/README.md) and
  how Scene Launch/transport CCs could reach an instrument track.
  - `midi_ports.h` (new): MAIN = USB cable 0 "MIDI" (notes, MPE, pedals,
    clock/Start/Stop, Identity; mirrored to DIN), DAW = cable 1 "DAW"
    (`tiles_midi_send_daw_cc()` out; Scene Launch SysEx and the TILES
    DISPLAY echo notes in; never DIN).
  - `usb_descriptors.c`: the MIDI interface declares two cables with
    named jacks (TinyUSB's per-cable macros; static-asserted lengths).
    The PRODUCT name is now plain "SENTIA TILES" on every unit, since it
    names the ports ("SENTIA TILES MIDI"/"SENTIA TILES DAW" on macOS) and
    is what Ableton matches its script against; the unit label ("were
    moving to have identifiers") moved to the diagnostics interface's name
    and the settings shell's `INFO` (`unit=`, plus `firmware=`).
  - `usb_midi_packet.{h,c}` (new, native tests in `test/
    test_usb_midi_packet.c`): USB-MIDI 1.0 event packets. Output and input
    both use whole packets now, not TinyUSB's byte stream -- the stream
    API merges all cables on read and keeps ONE partial-message state
    across cables on write, so a message cut short on one port would be
    finished on the other. A packet is queued whole or not at all.
  - `midi_in.c`: one parser per port (MAIN, DAW, DIN); SysEx listeners
    get the port (`tiles_midi_in_sysex_callback_t`). Scene Launch
    (`op_mode.c`'s `scene_on_sysex()`) ignores clip/scene SysEx from any
    port but DAW; the Identity Reply goes back on the port the request
    came in on. Notes from every port still reach the melodic echo -- its
    notes arrive on DAW, because TILES DISPLAY sends them through the
    script's output. Tests in `test/test_midi_in.c` (USB input is fed as
    packets now).
  - MIDI FIFOs 64 -> 256 bytes each way (64 packets): the zone
    declaration alone is 60 CCs, and Live's clip-colour burst arrives on
    the DAW port.
  - Firmware 0.2.0 (the port layout is a USB interface change).
  - Ableton side: `daw-integration/ableton/TILES/__init__.py` declares the
    two ports to Live (`get_capabilities()`, the same layout Ableton's own
    Launchkey MK3 script declares), with auto-load; setup and the macOS
    stale-device cleanup are in `daw-integration/README.md`.
