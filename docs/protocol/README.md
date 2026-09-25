# USB vendor protocol — design notes

> **Current state:** a text settings shell (GET/SET/LIST/RESET/SAVE/SCHEMA/INFO) over a
> settings table with flash saving is built -- see `../../shared/protocol/README.md`. The
> direction for the binary protocol, layouts, and the app stack is in
> `../architecture/control-software.md`. The open questions below still stand for the binary
> version.

Working notes ahead of formalizing the wire protocol in
`../../shared/protocol/`. The device exposes a custom USB vendor interface
(separate from USB-MIDI) for:

- Live pad remap / note / scale / MPE config edits.
- Guided per-pad calibration (rest/half/bottom/tilt capture, live raw and
  calibrated XYZ streaming).
- Profile/scene read, write, and switch.
- Diagnostics (I2C enumeration, per-key test, power state, pedal/button
  read, bounded motor pulse, single LED set).
- Firmware update.

Open questions to resolve before authoring `shared/protocol/`:

- Framing: fixed-size packets vs. length-prefixed vs. COBS-style framing
  over bulk endpoints.
- Request/response correlation (sequence IDs) vs. fire-and-forget +
  separate streaming endpoint for high-rate telemetry (24-pad XYZ at
  ~120Hz).
- Versioning/capability negotiation so an older companion app talking to
  newer firmware (or vice versa) fails safely instead of silently
  misinterpreting fields.
- Message encoding: hand-rolled binary structs vs. a schema
  (protobuf/flatbuffers/cbor) that can generate both the C and TypeScript
  sides from one definition.
