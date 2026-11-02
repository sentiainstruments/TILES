# protocol/

The **first, deliberately narrow version** of the USB vendor protocol --
a plain-text settings GET/SET, covering only the
runtime toggles the firmware already built with a companion-app hook in
mind. It is **not** the fuller protocol `docs/protocol/README.md`'s own
design notes describe (pad remap, guided per-pad calibration, live 24-pad
XYZ streaming at ~120Hz, profile read/write, firmware update) -- those
are real, still open, and this version deliberately doesn't try to
answer their harder questions (binary framing, sequence IDs/streaming,
versioning, schema-based encoding). Scoped and confirmed this way on
purpose: prove the USB vendor interface and a real settings round-trip
work at all, over the simplest protocol that could possibly work, before
investing in the bigger design.

## Transport

A dedicated USB vendor-class interface (interface string "SENTIA TILES
Control"), separate from both the CDC diagnostics console and the MIDI
interface -- implemented in `../../firmware/src/usb_vendor/usb_vendor.c`.
Full-speed bulk endpoints, 64-byte packets.

The device's USB ID is `firmware/src/midi/product_identity.h`'s: the
pid.codes TEST ID `0x1209:0x0001` while TILES is pre-production (in-house
use only -- it must become a real allocated ID before any unit is given
out; that file says how). Firmware before 2026-09-29 used `0x2E8A:0x100A`,
a Raspberry Pi product ID that turned out to be allocated to another
company's board. A host should identify TILES by the interface string, not
the ID alone. On Windows the interface binds to Microsoft's WinUSB driver
automatically (Microsoft OS 2.0 descriptors, device interface GUID
`{8cfdc7ad-aecd-411d-9610-df8c9929713c}`) -- no driver install; not yet
tested on a Windows machine.

## Wire format

Plain ASCII text, one command per line, newline-terminated (`\n`; `\r`
is also accepted and treated the same as `\n`, so a naive line-based
terminal on either line-ending convention works unmodified):

```
LIST\n                    every setting: one <key>=<value> line each, then OK
GET <key>\n               the value as one line, or ERR unknown-key
SET <key> <value>\n       OK, or ERR unknown-key / bad-value / out-of-range
RESET <key>|ALL\n         one setting (or every one) back to its default: OK / ERR unknown-key
SAVE\n                    write unsaved changes to flash now: OK / ERR save-failed-<why>
REBOOT BOOTSEL\n          reboot into the ROM bootloader for reflashing (no reply -- see below)
REBOOT APP\n              plain warm restart back into this firmware: OK
SCHEMA\n                  one line per setting describing it, then OK (see below)
INFO\n                    unit=, firmware=, power, settings-store and content-store status, as key=value lines, then OK
SCALES\n                  every custom scale on the device, one line each, then OK (see "Custom scales")
SCALE GET <slot>\n        one custom scale's line, then OK; or ERR bad-slot / empty
SCALE PUT <slot> <name> <intervals> [pack=<id>] [item=<id>] [version=<n>]\n
                          save a custom scale into slot 1-9 (saved before OK)
SCALE DELETE <slot>\n     empty a slot: OK (also when already empty)
CONTENT LIST\n            every record in the content store, one line each, then OK
CONTENT CLEAR\n           wipe the content store (every custom scale): OK
TEST LEDS <0-100>\n       bench only: every LED white at that % (current measurement)
TEST MOTORS <n> [duty%]\n bench only: pads 1..n's motors for 8 s
TEST MODE <name>\n        bench only: switch mode (melodic duet sequencer bass song ableton drums volca prism bounce ripples sand orbits cradle)
TEST OFF\n                bench only: back to normal
```

`SET`/`RESET` apply **immediately** and are saved to flash **automatically** ~2 s
after the last change, only while no pad is being touched (a flash write stalls
the firmware for tens of ms). `SAVE` skips the wait -- don't send it mid-phrase.

Errors: `bad-value` = not parseable as the setting's type (`1.5x`, `true` for a
bool); `out-of-range` = parsed but outside the setting's range, or an unknown
enum name. A value never partially applies.

No binary framing, no capability negotiation -- a command is answered by
exactly the response(s) it produces, in order, before the next command is
read (the device won't start the next command until the previous response
has been fully sent). Good enough for occasional, human- or script-driven
settings changes; genuinely insufficient for high-rate telemetry (live sensor
streaming), deferred to whatever protocol version eventually covers that.
Replies can be long (SCHEMA is
~5 KB; LIST, SCHEMA, SCALES and CONTENT LIST are streamed, so they have no
size limit) and arrive across many 64-byte USB packets: **read lines, not
packets**, and read until the final `OK` (or an `ERR` line).

### Request tags (firmware 0.3.7+)

A command may start with a tag, `@` plus 1-16 of `A-Z a-z 0-9 _ -`, and a
space. Every line of its reply then starts with the same tag:

```
@7 GET pedal.mode\n      ->  @7 sustain
@8 LIST\n                ->  @8 pedal.mode=sustain  ...  @8 OK
@9\n                     ->  @9 ERR missing-command
@a+b GET x\n             ->  ERR bad-tag   (untagged: the tag couldn't be read)
```

Untagged commands are answered untagged, exactly as before. Tags let a host
match every line to its request and skip lines that aren't its answer --
for instance the rest of a long reply that was still streaming when the
previous app quit (the device can't tell the interface was released, so
those lines wait for the next reader). Replies still come in command order;
a host that reads from several places at once should still send one command
at a time (the companion app queues them: `companion-app/src/main/usb.ts`).
Firmware before 0.3.7 answers a tagged command `ERR unknown-command`,
untagged; a host can fall back to plain commands on that.

Don't write a batch of commands without reading: the device stops reading
while a reply waits to be collected, so the write stalls and can time out
partway through a line. From 0.3.8 a partial line (no newline yet) is
dropped after 1 s with nothing arriving while the device waits for input,
so a half-sent command can't glue itself onto the next one.

### REBOOT

Real feedback: "will we be able to flash updates without putting the board in
bootloader mode" -> "yes add the software reboot command." `REBOOT BOOTSEL`
puts the device in the RP2350's ROM USB bootloader (for reflashing); the
device does its best to get an `OK` out first, but don't rely on seeing it --
it may vanish from USB before the reply arrives, and the correct client
behaviour is to treat that disconnect as success, not a failure, and wait for
a device in BOOTSEL mode to reappear. `REBOOT APP` is a plain warm restart
back into the same firmware and always replies `OK` normally.

This is the software/scriptable path. `picotool load -f` and `picotool
reboot -u` don't need either command -- the device also exposes a standard
USB reset interface (a fourth, separate composite interface; see
`firmware/src/midi/README.md`) that `picotool` already knows how to drive
without any app-level protocol at all.

### SCHEMA

Everything a UI needs to build a control for a setting, so an app never
hard-codes the list:

```
id=256 key=pedal.mode type=enum values=sustain|expression default=sustain
id=513 key=expression.pitch_bend_sensitivity type=float min=0.001 max=1 default=0.065000
id=514 key=expression.aftertouch_sensitivity type=uint min=1 max=65535 default=900
id=768 key=cv_gate.enabled type=bool default=0 persist=0
```
`type` is `bool|uint|int|float|enum|color`; `persist=0` marks a setting that is
never saved (it boots at its default every time). A `color` line has no
min/max: `id=1792 key=color.root type=color default=660066`. `id` is permanent (what a future
binary protocol will use); `key` is the human name.

## Key catalog

Booleans are the literal text `0` or `1`. Floats round-trip through
`%.6f`/`strtof`. Ranges are what the device accepts (a SET outside them is
refused, not clamped); the default is whatever the owning module boots with --
`SCHEMA` reports it.

| Id | Key | Type | Range / values | Default | Saved |
|---|---|---|---|---|---|
| 0x0100 | `pedal.mode` | enum | `sustain`, `expression` | `sustain` | yes |
| 0x0101 | `pedal.polarity` | enum | `normally_open`, `normally_closed` | `normally_open` | yes |
| 0x0102 | `pedal.sustain_style` | enum | `synth` (pedal sends CC64, the synth sustains -- standard), `hold` (no CC64; TILES keeps real notes on until the pedal lifts, so harmonic plucks still release) | `synth` | yes |
| 0x0200 | `expression.mpe_enabled` | bool | | `1` | yes |
| 0x0201 | `expression.pitch_bend_sensitivity` | float | 0.001 - 1.0 (max cosine deviation) | `0.065` | yes |
| 0x0202 | `expression.aftertouch_sensitivity` | uint | 1 - 65535 (full-scale depth) | `900` | yes |
| 0x0300 | `cv_gate.enabled` | bool | | `0` | **no** -- boots off, every time |
| 0x0301 | `cv_gate.pitch.volts_per_semitone` | float | 0.001 - 1.0 | `0.0833333` (1V/oct) | yes |
| 0x0302 | `cv_gate.pitch.reference_note` | int | 0 - 127 (MIDI note at 0V) | `0` | yes |
| 0x0303 | `cv_gate.pitch.zero_trim_volts` | float | -2.5 - 2.5 | `0.0` | yes |
| 0x0304 | `cv_gate.pitch.gain_trim` | float | 0.5 - 2.0 | `1.0` | yes |
| 0x0305 | `cv_gate.pressure.full_scale_volts` | float | 0.1 - 10.0 | `10.0` | yes |
| 0x0306 | `cv_gate.pressure.zero_trim_volts` | float | -2.5 - 2.5 | `0.0` | yes |
| 0x0307 | `cv_gate.pressure.gain_trim` | float | 0.5 - 2.0 | `1.0` | yes |
| 0x0400 | `look.idle_baseline_percent` | uint | 0 - 100 | `50` | yes |
| 0x0405 | `look.echo_sustain_tint_percent` | uint | 0 - 100 | `35` | yes |
| 0x0406 | `look.echo_secondary_g_percent` | uint | 0 - 100 | `18` | yes |
| 0x0407 | `look.echo_secondary_b_percent` | uint | 0 - 100 | `12` | yes |
| 0x0408 | `look.echo_flash_ms` | uint | 0 - 2000 (0 = no flash) | `180` | yes |
| 0x0409 | `look.pressure_hue_degrees` | uint | 0 - 90 (0 = off) | `40` | yes |
| 0x040A | `look.bend_dim_percent` | uint | 0 - 90 (0 = off) | `50` | yes |
| 0x040B | `look.led_intensity_percent` | uint | 0 - 100 (0 = off) | `100` | yes |
| 0x040C | `look.play_effect` | enum | `off`, `afterglow`, `ripple`, `cross`, `halo`, `warmth`, `prism_ripple`, `touch_colour`, `heatmap`, `fireworks`, `rise`, `ink` (the light effect while playing; Feel menu, circle + square) | `off` | yes |
| 0x0500 | `midi.din_trs_type` | enum | `a`, `b` (TRS polarity of DIN MIDI OUT) | `a` | yes |
| 0x0501 | `midi.clock_lead_ms` | uint | 0 - 50 (ms early that pulses come through when following a DAW's clock, so the notes they trigger land on its grid; 0 = off) | `15` | yes |
| 0x0502 | `midi.parts_port` | enum | `parts` (the sequencer, drums, Volca, Bouncing notes, Song tracks, chords and games on their own USB port, "Parts"), `main` (all on the MIDI port) | `parts` | yes |
| 0x0600 | `features.melodic_harmonics` | bool | | `1` | yes |
| 0x0601 | `features.harmonics.arm_ms` | uint | 0 - 1000 (ms a struck note is held alone before other touches may pluck) | `150` | yes |
| 0x0602 | `features.harmonics.confirm_ms` | uint | 0 - 500 (ms a touch waits before plucking) | `40` | yes |
| 0x0603 | `features.harmonics.press_depth` | uint | 1 - 1000 (Hall depth, rest 0 / strike 150, above which a touch counts as a real press, not a harmonic) | `128` | yes |
| 0x0700 | `color.root` | color | `RRGGBB` or `none` | `660066` (Sentia pink) | yes |
| 0x0701 | `color.third` | color | `RRGGBB` or `none` | `none` (no highlight) | yes |
| 0x0702 | `color.fifth` | color | `RRGGBB` or `none` | `240066` (violet) | yes |
| 0x0703 | `color.note` | color | `RRGGBB` or `none` | `363636` (dim white) | yes |
| 0x0704 | `color.accidental` | color | `RRGGBB` or `none` | `000000` (dark) | yes |
| 0x0705 | `color.custom_pads` | bool | | `0` | yes |
| 0x0710 - 0x0727 | `color.pad.01` ... `color.pad.24` | color | `RRGGBB` or `none` | `none` | yes |
| 0x0800 | `modes.duet` | bool | | `1` | yes |
| 0x0801 | `modes.sequencer` | bool | | `1` | yes |
| 0x0802 | `modes.drums` | bool | | `1` | yes |
| 0x0803 | `modes.song` | bool | | `1` | yes |
| 0x0804 | `modes.prism` | bool | | `1` | yes |
| 0x0805 | `modes.bass` | bool | | `1` | yes |
| 0x0806 | `modes.ableton` | bool | | `1` | yes |
| 0x0807 | `modes.volca` | bool | | `1` | yes |
| 0x0808 | `modes.bounce` | bool | | `1` | yes |
| 0x0809 | `modes.cradle` | bool | | `1` | yes |
| 0x080A | `modes.sand` | bool | | `1` | yes |
| 0x080B | `modes.ripples` | bool | | `1` | yes |
| 0x080C | `modes.orbits` | bool | | `1` | yes |
| 0x0900 | `layout.melodic` | enum | `rows` (the default), `columns`, `fourths`, `keys`, `fifths`, `tonnetz`, `quarter_tones` (melodic mode's note layout; Feel menu, circle + triangle; firmware/src/services/note_map.h) | `rows` | yes |
| 0x0A00 | `seq.lock` | enum | `off`, `bar`: a sequencer lane or the drums started while others play joins on the next step at the step it would be on had it played from the song's top, and stays there (16-step patterns share the bar line). No hardware gesture (firmware/src/services/op_mode.h) | `off` | yes |

Ids `0x0401`-`0x0404` (`look.natural_pad_percent`, `look.root_pad_percent`,
`look.fifth_pad_percent`, `look.fifth_red_tint_percent`) were retired in
firmware 0.2.7: the colour scheme below replaced them, with the same look as
its default. A saved value for one of them is ignored.

The `look.*` values are whole percent of the **fixed** LED brightness ceiling
(50% USB-only / 90% external power) -- no setting can raise the ceiling itself.
See `firmware/src/services/lighting.h` for what each tier is. Two act on
touched pads in every mode: `look.pressure_hue_degrees` turns a pad's colour
toward Sentia blue (#94ABF9) by up to that many degrees at full pressure (white
tints partway toward it instead), brightness unchanged, with MPE on only; `look.bend_dim_percent`
is how much a bending pad's brightness falls at full bend (a smooth swell).
`look.led_intensity_percent` scales the pads, underglow and button LEDs below the ceiling
(the Feel menu's row 4 sets 0, 4, 16, 36, 64 or 100; 0 = off).

`modes.*` add or remove modes from the mode menu (triangle), for the companion
app: `0` takes the pad off the menu (dark, can't be picked), `1` puts it back.
Melodic is always there; turning off the mode in use switches to melodic.
`modes.duet` is the chord + melody mode, `modes.prism` the chord machine,
`modes.bounce` Bouncing notes, `modes.cradle` Newton's cradle, and
`modes.sand`, `modes.ripples`, `modes.orbits` the other physics modes. The
menu itself is laid out by family, one column each: melodic / Duet / bass
(magenta, pink, lilac), sequencers (sequencer scarlet, drums orange, Song
amber), Prism (blue), hardware (Ableton teal, Volca cyan), then two columns of
physical modelling and experimental modes, each in the colour of what it
models: Bouncing notes (lime), Newton's cradle (steel), Sand (sand); Ripples
(water), Orbits (night sky). A mode's underglow and resting pads are always
its menu colour.

`cv_gate.enabled` being true here does **not** mean CV/gate is actually
driving anything -- `services/power.h`'s own external-power confirmation
is a separate, hardware-enforced gate this protocol has no ability to
override, by design (see `services/cv_gate.h`'s own header comment).

Ids are permanent: never reuse or renumber one. Grouped by hundreds (`0x01xx`
pedal, `0x02xx` expression, `0x03xx` CV/gate, `0x04xx` look, `0x05xx` MIDI,
`0x06xx` features, `0x07xx` colours).

### Colour schemes and pad colours

What melodic mode's idle pads look like (and chord mode's melody grid), by
the role of each pad's note in the current key and scale. Five colours make a
scheme; the app owns the schemes (names, presets, sharing) and writes the
five values to apply one:

| Role | Key | Lights | Default |
|---|---|---|---|
| Root | `color.root` | the key's root, every octave | `660066`, Sentia pink |
| Fifth | `color.fifth` | the perfect fifth (7 semitones) | `240066`, violet |
| Third | `color.third` | the scale's major third (4) if it has one, else its minor third (3) | `none` |
| Accidental | `color.accidental` | a sharp/flat pitch class (the "black keys"); mostly matters in chromatic | `000000`, dark |
| Note | `color.note` | every other scale note | `363636`, dim white |

- A pad with several roles takes the first in that order: root, fifth,
  third, accidental, note.
- `none` turns a role off: its pads fall through to the next role that
  applies (`color.third` defaults to `none`, so thirds show as notes).
- Values are what the pad shows **before** the power ceiling (50% on USB,
  90% on external power), so brightness is in the value: `363636` is a dim
  white, `FFFFFF` full. The ceiling still applies on top; no setting can
  raise it.
- Pressed pads (white), the echo after a note, and Song capture still draw
  over the scheme.
- Defaults are the device's original look: `RESET color.root` (or `RESET
  ALL`) brings a role back.

**Custom pad colours** (an advanced feature): set `color.custom_pads` to `1`
and any of `color.pad.01`...`color.pad.24` to a colour, and that pad shows
it in melodic mode instead of its role colour. Pads left at `none` keep the
scheme. Pad numbers are TILES' logical pads (1-24, top-left to bottom-right,
the same numbers as everywhere else in this protocol). Chord mode keeps the
scheme: its grid is laid out differently. Turning `color.custom_pads` off
keeps the per-pad values, so the user can switch back.

Writing a whole scheme is 5 `SET`s (a full pad map, 25); they apply at once
and are saved together ~2 s after the last one.

### Custom scales

The scale picker (pads 16-24 while choosing a scale) has 9 custom slots.
An empty slot is unavailable on the device; a filled one lights and plays
like a built-in scale (root and fifth colouring included). The app pushes
scales into slots and can pull back everything on the device.

```
SCALE PUT 1 Hirajoshi 0,2,3,7,8 pack=japan item=hirajoshi version=1
OK
SCALES
1 Hirajoshi 0,2,3,7,8 pack=japan item=hirajoshi version=1
4 My_Blues 0,3,5,6,7,10 version=0
OK
```

- `<slot>`: 1-9 (picker pads 16-24, in order).
- `<name>`: 1-16 printable ASCII characters, no spaces (show `_` as a
  space if you like).
- `<intervals>`: semitones above the root, comma-separated: 1-12 of them,
  starting at `0`, strictly increasing, each 0-11. The root comes from the
  device's key, as for built-in scales.
- `pack=`, `item=` (each 0-16 characters, no spaces) say which pack and which
  item in it the scale came from; leave them out for a user-made scale.
  `version=` (0-65535, default 0) is the item's version, so the app can tell
  an outdated copy. All three are stored and listed, never interpreted by
  the device (see `docs/architecture/content-packs.md`).
- A `SCALES` line is exactly a `SCALE PUT`'s arguments, so a pulled scale
  pushes back unchanged. Empty slots aren't listed.
- `PUT` replaces whatever was in the slot. It and `DELETE` are written to
  flash **before** the reply (the write pauses the device for tens of ms, so
  don't send them mid-phrase): `OK` means saved.
- Errors: `bad-slot`, `bad-name` (name/pack/item empty where required, too
  long, or with a space/control character), `bad-intervals`, `bad-field`
  (an unknown `key=` or a bad version), `content-full`, `no-storage`,
  `newer-format` (see below), `save-failed-<why>`. On any error nothing
  changes.

Built-in scales aren't listed: they're fixed per firmware version (pads
1-15 of the picker, `SCALE_GRID_ORDER` in `firmware/src/services/note_map.c`).

### Content store

Custom scales live in the **content store**: its own two-sector flash region
next to the settings, power-cut safe the same way, ~4 KB (all nine scales at
their largest take ~0.6 KB). It is built to hold more kinds of content
later (layouts, and data for modes and games) without a format change, which
is why `CONTENT LIST` names each record's type:

```
CONTENT LIST
scale 1 bytes=34 version=1 pack=japan item=hirajoshi
scale 4 bytes=20 version=0
OK
```

A record of a type this firmware doesn't know (written by a newer one) is
kept and listed as `type<N> <slot> bytes=<n>`, never applied. If the whole
store was written by a newer firmware's format, it's read-only
(`newer-format` on every change, `content.storage=newer-format` in INFO)
until `CONTENT CLEAR`, so a downgrade can't silently destroy it.
`CONTENT CLEAR` erases every custom scale; settings are separate and
untouched (`RESET ALL` is theirs).

### INFO

`key=value` lines, then `OK`: `unit=2/4`, `firmware=alpha.4` or `alpha.4.1` (stage.build[.point]; product_identity.h), `settings=<n>`,
`power=usb_only|external_only|usb_and_external|fault` and its
`power.budget_ma`, `power.led_ceiling_percent`, `power.haptic_voices`; the
settings store's `store.*` (loaded, restored, slot, seq, saved_bytes, writes,
last_result, pending, failures); the content store's
`content.storage=ok|none|newer-format`, `content.scales=<n>/9`,
`content.records`, `content.bytes=<used>/<capacity>`, `content.writes`,
`content.last_result`; main-loop timing `loop.avg_us`, `loop.max_us` (last
second) and `loop.max_ever_us`; sensor health `hall.rezeros`,
`hall.unconfirmed`, `hall.reinits`, `hall.dead`, `touch.recoveries`; the
sequencer's autosave `seq.storage`, `seq.saved_bytes`, `seq.saves`,
`seq.pending`, `seq.truncated`; boot timing `boot.ms` (power-on to the main loop) and
`boot.sensors_ms`; drum mode's store `drums.storage`,
`drums.saved_bytes`, `drums.saves`, `drums.pending`; the play state (mode,
scale, key, octave kept across power cycles) `state.storage`,
`state.saved=mode:<n> scale:<n> octave:<n> key:<n>` (once something is
saved), `state.saves`, `state.pending`. New keys may be added;
ignore ones you don't know.

## Persistence

Settings are saved to flash (`firmware/src/storage/`, two alternating 4 KB
sectors, CRC-protected, power-cut safe) and restored at boot, **sparsely**: only
values that differ from their default are written, so a setting you never
touched follows the firmware's default if a later build changes it. They
survive reboots and firmware updates (`picotool load` doesn't touch that
region). Design and rules: `firmware/src/profiles/README.md`. The content
store (custom scales) is a second, separate region with the same guarantees
(`firmware/src/profiles/content.h`).

## Testing

`../../tools/tiles_control.py` -- a plain Python script (`pyusb`) that
connects to the vendor interface and exercises every command above
(`list`, `schema`, `get`, `set`, `reset`, `save`, `info`, `scales`, `scale
get|put|delete`, `content list|clear`, `test`). See that script's own
header for setup (on macOS, `pyusb` needs `libusb` installed, and vendor-class
devices sometimes need the terminal running it to have been granted the OS's own
USB-device permission prompt the first time).

## Scene Launch (Ableton Live)

A second, separate protocol, on TILES's second USB-MIDI port, **"SENTIA
TILES DAW"** (`firmware/src/midi/midi_ports.h`) -- NOT the vendor-interface
settings protocol above (this one talks to Ableton Live's own Remote
Script, not a host control app). It used to share the instrument's port
("SENTIA TILES MIDI", notes/CC/clock); since the standardization round
(real feedback: "do 8 as how standardized stuff works. production ready
industry stuff") everything below travels on the DAW port only, the
firmware only accepts the clip/scene SysEx from that port, and nothing
here is ever mirrored to DIN. The history below is from the shared-port
days. Real feedback: "lets implemebt a new mode that triggers
scenes in ableton live keep it simple for now... can we pull the colors
of the scenes from ableton?"

**Architecture, rewritten twice after several real-hardware rounds
with no confirmed successful delivery**: "master stop doesnt work at
all, individual start and stop doesnt work and hasent for the past few
pushes. i need you to look at how a lounchapd works or abletoun push
works to pull the exxact same standardizre behaviour." The TILES ->
Ableton direction used to be a custom SysEx sub-protocol; despite
passing review against Ableton's own real Remote Script source
multiple times, it never had one confirmed successful round-trip on
real hardware.

First rewrite replaced it with plain Note-On, matching how a real
Launchpad sends its own grid (confirmed against Ableton's bundled
`Launchpad.py`). Real feedback caught the real flaw: "you fully broke
how clip lounching works now its just sending regular midi notes for
me to map. thats not how this feature operates ever in any device." A
real Launchpad is a dedicated grid controller that never sends musical
note content at all, so nobody ever enables that port's "Track" MIDI
input in Ableton. TILES is not that -- this exact same USB-MIDI port
also carries real musical Note-On for melodic/chord/guitar/sequencer
play, so the user's own instrument track almost certainly already has
this port's Track input enabled (typically "All Channels," required
for real MPE playback), meaning a Scene Launch "button" Note-On is
ALSO delivered to that track as ordinary playable/recordable content
on top of whatever the Remote Script's own `ButtonElement` does with
it -- being claimed by the Control Surface's Remote path and reaching
a Track's input are not mutually exclusive in Ableton. A CC never has
this problem: Ableton never treats a CC as note/audio content for an
instrument regardless of Track/Remote routing, exactly why the
transport CCs have always been safe on this same port. Second rewrite
moved grid-touch/stop-touch off Note-On entirely, onto CC, matching
everything else in this protocol.

Both rewrites bind real `ButtonElement`s via `add_value_listener()` --
the same mechanism this project's own transport-remote CCs
(`OP_TRANSPORT_PLAY_CC` etc.) already use, with actual confirmed
real-hardware delivery.

| Direction | Transport | Meaning |
|---|---|---|
| TILES -> Ableton | CC `CC_GRID_TOUCH` (108), value = pad, then 0 | Pressure click: fire that pad's clip (track columns 1-5) or launch that pad's whole scene (column 6); an EMPTY slot does nothing |
| TILES -> Ableton | CC `CC_STOP_TOUCH` (109), value = pad, then 0 | Pressure click on a clip that's already playing: stop that one clip (track columns 1-5 only) |
| TILES -> Ableton | CC `CC_RECORD_TOUCH` (111), value = pad, then 0 | Circle held + pressure click on an EMPTY track slot: arm the track and record a new clip into it (the script checks the slot is still empty) |
| TILES -> Ableton | CC `CC_MASTER_STOP` (105), 127 then 0 | Stop all clips (master stop) -- circle + diamond in Ableton mode, toggle transport layout only (the performance layout, on now, sends CC 103 Stop instead; `OP_TRANSPORT_SHIFT_STOP` in `op_mode.c`) |
| TILES -> Ableton | CC `CC_TRACK_OFFSET` (106), value = offset | Visible track window changed -- keeps the session-ring overlay and the pad-to-track mapping in sync |
| TILES -> Ableton | CC `CC_END_CAPTURE` (107), 127 then 0 | Circle + diamond during a live capture (melodic mode opened after circle + an empty slot started a new clip): end that recording, in either transport layout; the firmware returns to Ableton mode itself |
| TILES -> Ableton | CC `CC_DELETE_TOUCH` (110), value = pad, then 0 | Circle held + pad touched 3 seconds on a clip: delete that clip (track columns 1-5 only; the firmware times the hold) |
| Ableton -> TILES | SysEx `F0 7D 01 10 track scene flags r7 g7 b7 F7` | One clip slot's current state |
| Ableton -> TILES | SysEx `F0 7D 01 11 scene flags r7 g7 b7 F7` | One scene's current state |
| Ableton -> TILES | SysEx `F0 7D 01 12 F7` | A track was just armed for a new recording -- open melodic mode (firmware waits until every pad is released) |

A bare capacitive touch sends NOTHING to Ableton -- it's haptics-only on the hardware side (a strong "ready" click on a pad with a clip, a continuous buzz while that clip is playing). Only a pressure click (Hall depth past the same 50% "push to select" threshold the mode menu uses) sends the CCs below.

All TILES -> Ableton messages are plain CC on the DAW port,
`TILES_MIDI_MPE_MASTER_CHANNEL` (channel 1) -- the same channel the
transport CCs use, deliberately never Note-On (see above) -- and every
controller number is in the MIDI spec's "undefined" 102-119 range.
Grid/stop/delete used to be one CC PER PAD (10/40/70 + pad), which put
three of them on standard performance controllers on this same channel:
CC 64 (sustain) was pad 24's stop, CC 11 (expression) pad 1's launch,
CC 74 (MPE slide) pad 4's delete -- with the TILES control surface
active, Ableton routed the sustain pedal to this script instead of the
instrument. Real feedback that pinned it: "equator as strandalone dosnt
have the issues wirthg sustain, it wo4rks flawlesslyt." Now one CC per
action with the pad as its value; see `op_mode.c`'s
`OP_SCENE_CC_GRID_TOUCH` comment. `pad` is
1-24 (`TILES_NUM_PADS`); Ableton-side, `scene_launch.py` derives
`(column, row)` from `pad` exactly like `op_mode.c`'s own
`handle_scene_launch_taps()` does, and derives the real track index
from `column` plus whatever `CC_TRACK_OFFSET` last reported (it has to
track that itself now, mirroring `op_mode.c`'s own
`s_scene_track_offset`).

The Ableton -> TILES SysEx frames are unchanged: manufacturer ID
`0x7D` (MMA-reserved "non-commercial/educational use" -- the correct
placeholder for an unregistered, pre-production product; one constant,
`TILES_SYSEX_MANUFACTURER_ID` in `firmware/src/midi/product_identity.h`,
shared with the MIDI Identity Reply), sub-ID `0x01`,
parsed firmware-side by `firmware/src/midi/midi_in.c` and handled by
`op_mode.c`'s "Scene Launch (Ableton) mode" section. `track` is 0-based, up
to 63 (`OP_SCENE_MAX_TRACKS`/`MAX_TRACKS`, firmware/Python
respectively); `scene` is 0-based, up to 3 (only Ableton's first 4
scenes are ever tracked -- no scene paging in this version). `flags`
bit 0 = has_clip (clip state only), bit 1 = is_playing (clip state
only), bit 2 = is_triggered (both). `r7`/`g7`/`b7` are each 0-127 --
Ableton's own 0-255 color channel halved; the firmware doubles it back
toward 8-bit on receipt, losing the bottom bit, not the top.

Ableton pushes state for every tracked cell once on script connect
(not just whatever's currently scrolled into view on the hardware),
then again on every real change via Live API listeners.

**Confidence**: the Ableton -> TILES SysEx wire format is exact and
firmware-verified. The CC reception mechanism for the other direction
matches this project's own already-confirmed-working transport CCs
exactly, and avoids the real, confirmed flaw the Note-On version hit
(leaking through as playable/recordable note content on any track
with this port's Track input enabled). A real bug in the CC version's
own `_connect()` -- calling a `register_components()` method that
doesn't exist on `ControlSurface` -- silently aborted button binding
on every load until fixed (replaced with the real, public
`set_highlighting_session_component()`); see `scene_launch.py`'s own
module docstring for the full story. The Live API calls themselves
(`add_playing_status_listener`, `song().stop_all_clips()`,
`Clip.stop()`) are each individually confirmed against Ableton's own
bundled Remote Script source or the community AbletonOSC project.

## Not built yet

Everything `docs/protocol/README.md`'s own design notes list beyond
settings: pad remap, guided calibration capture, live sensor streaming,
profile read/write, firmware update, layouts and paid packs
(`docs/architecture/content-packs.md` is the plan), and the real framing/versioning/
schema decisions those need. This document's own scope will need
revisiting, not just extending, once any of those get designed --
they're likely to need actual binary framing and sequencing this
version deliberately does without. Scene Launch above is a separate,
narrower SysEx sub-protocol built alongside this one, not folded into
it -- it talks to Ableton's Remote Script, not the vendor-interface
settings channel this document is otherwise about.
