# protocol/

Real feedback: "keep cv gate implemented but off rn. we need the control
software." This is the **first, deliberately narrow version** of the USB
vendor protocol -- a plain-text settings GET/SET, covering only the
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

A dedicated USB vendor-class interface (VID `0x2E8A`, PID `0x100A`,
interface string "SENTIA TILES Control"), separate from both the CDC
diagnostics console and the MIDI interface -- implemented in
`../../firmware/src/usb_vendor/usb_vendor.c`. Full-speed bulk endpoints,
64-byte packets.

## Wire format

Plain ASCII text, one command per line, newline-terminated (`\n`; `\r`
is also accepted and treated the same as `\n`, so a naive line-based
terminal on either line-ending convention works unmodified):

```
GET <key>\n
SET <key> <value>\n
LIST\n
```

Responses, one line each:

- `GET`: the current value as text, or `ERR unknown-key`.
- `SET`: `OK`, or `ERR unknown-key` / `ERR bad-value`.
- `LIST`: one `<key>=<value>` line per known key, then a final `OK`.

No sequence IDs, no binary framing, no capability negotiation -- a
command is answered by exactly the response(s) it produces, in order,
before the next command is read. Good enough for occasional, human- or
script-driven settings changes; genuinely insufficient for high-rate
telemetry (live sensor streaming) or anything needing to stay correct
under request/response interleaving -- both explicitly deferred to
whatever protocol version eventually covers that.

## Key catalog

Booleans are the literal text `0` or `1`. Floats round-trip through
`%.6f`/`strtof` (six decimal digits -- comfortably more precision than
any of these values are actually calibrated to yet).

| Key | Type | Values | Default |
|---|---|---|---|
| `pedal.mode` | enum | `sustain`, `expression` | `sustain` |
| `pedal.polarity` | enum | `normally_open`, `normally_closed` | `normally_open` |
| `expression.mpe_enabled` | bool | `0`, `1` | `1` |
| `expression.pitch_bend_sensitivity` | float | max cosine deviation | `0.065` |
| `expression.aftertouch_sensitivity` | uint | full-scale depth | `1450` |
| `cv_gate.enabled` | bool | `0`, `1` | `0` |
| `cv_gate.pitch.volts_per_semitone` | float | volts/semitone | `0.0833333` (1V/oct) |
| `cv_gate.pitch.reference_note` | int | MIDI note at 0V | `0` |
| `cv_gate.pitch.zero_trim_volts` | float | additive trim at the jack | `0.0` |
| `cv_gate.pitch.gain_trim` | float | multiplicative trim | `1.0` |
| `cv_gate.pressure.full_scale_volts` | float | volts at pressure=127 | `10.0` |
| `cv_gate.pressure.zero_trim_volts` | float | additive trim at the jack | `0.0` |
| `cv_gate.pressure.gain_trim` | float | multiplicative trim | `1.0` |

See each setting's own real definition (`firmware/src/services/pedal.h`,
`expression.h`, `cv_gate.h`) for what it actually controls -- this table
is the wire-format contract, not a re-explanation of the feature.

`cv_gate.enabled` being true here does **not** mean CV/gate is actually
driving anything -- `services/power.h`'s own external-power confirmation
is a separate, hardware-enforced gate this protocol has no ability to
override, by design (see `services/cv_gate.h`'s own header comment).

## Persistence

None of these settings are saved to flash yet -- every one resets to its
default on reboot, same as before this protocol existed. Real
persistence belongs in a `storage/` module that doesn't exist in this
codebase yet (see `docs/hardware/SENTIA_TILES_FIRMWARE_HANDOFF.md`'s own
recommended module boundary); a future version of this protocol, once
that exists, should write through to it rather than this document
inventing its own separate persistence scheme.

## Testing

`../../tools/tiles_control.py` -- a plain Python script (`pyusb`) that
connects to the vendor interface and exercises `LIST`/`GET`/`SET`
end-to-end. See that script's own header for setup (on macOS, `pyusb`
needs `libusb` installed, and vendor-class devices sometimes need the
terminal running it to have been granted the OS's own USB-device
permission prompt the first time).

## Not built yet

Everything `docs/protocol/README.md`'s own design notes list beyond
settings: pad remap, guided calibration capture, live sensor streaming,
profile read/write, firmware update, and the real framing/versioning/
schema decisions those need. This document's own scope will need
revisiting, not just extending, once any of those get designed --
they're likely to need actual binary framing and sequencing this
version deliberately does without.
