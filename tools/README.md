# tools/

Codegen and helper scripts. Primary planned job: take
`shared/board-map/` and `shared/protocol/` as the single authored source
and generate both the firmware's C headers (`firmware/src/board/pad_config.*`)
and the companion app's TypeScript types (`companion-app/src/shared/`), so
the two sides can't drift out of sync. Also home for calibration-jig or
batch-programming scripts once manufacturing tooling is needed.

## bootloader_watchdog.sh

Host-side (Mac) script, not firmware -- watches for a TILES board that has
spontaneously landed in the RP2350's own ROM USB bootloader instead of a
normal watchdog reboot back into the app, and kicks it back into the app
automatically via `picotool reboot -a`. See the script's own header comment
for the full context: this is a real, confirmed RP2040/RP2350 hardware
failure mode (current board REV:01 has no stronger pull-up on the RUN pin,
a documented mitigation for exactly this) that no firmware fix can touch,
since the app isn't running at all while the chip sits in the ROM
bootloader.

Run it manually before an actual playing/practice/performance session:

```
./tools/bootloader_watchdog.sh
```

Stop it (Ctrl+C) before deliberately reflashing a board -- it will otherwise
race with `picotool load`, kicking the board back into the app before the
flash tool gets to touch it.

## tiles_control.py

Host-side, not firmware -- exercises the USB vendor control protocol's
first version end-to-end (real feedback: "keep cv gate implemented but off
rn. we need the control software"). See `../shared/protocol/README.md` for
the wire format and the full settings catalog.

**Setup (macOS).** Needs `pyusb` and the `libusb` library. Homebrew's
Python refuses a system-wide `pip install` (PEP 668, "externally managed
environment"), so install it in a virtualenv once:

```
brew install libusb        # usually already there -- picotool pulls it in
python3 -m venv ~/.venvs/tiles-tools
~/.venvs/tiles-tools/bin/pip install pyusb
```

Then run the script with that interpreter:

```
~/.venvs/tiles-tools/bin/python tools/tiles_control.py list
~/.venvs/tiles-tools/bin/python tools/tiles_control.py get cv_gate.enabled
~/.venvs/tiles-tools/bin/python tools/tiles_control.py set cv_gate.enabled 1
```

Settings apply immediately and are saved to flash automatically. Ones
worth knowing about:

- `pedal.sustain_style` -- `synth` (default: the pedal sends standard
  CC 64 and the synth sustains) or `hold` (TILES keeps real notes on
  itself until the pedal lifts and sends no CC 64, so harmonic plucks
  ring out instead of being sustained). See `daw-integration/README.md`'s
  "Sustain pedal and MPE".
- `features.harmonics.press_depth` / `.confirm_ms` / `.arm_ms` -- the
  balance between playing chords and plucking harmonics (defaults 128 /
  40 / 150, set from a recorded playing session). Raise `press_depth` if
  light harmonic touches get missed, lower it if chords leak harmonics;
  see `firmware/src/services/README.md` for the data behind the defaults.

**Rebooting into the bootloader for flashing**, no BOOTSEL button:

```
~/.venvs/tiles-tools/bin/python tools/tiles_control.py reboot bootsel
sleep 2 && picotool load -x -v --ignore-partitions firmware/build/src/sentia_tiles_firmware.uf2
```

Each command first discards any reply an earlier, interrupted run left
queued on the device -- a `schema` piped through `grep` right after a
reboot once left ~2 KB there, which the next command read as its own
reply and the one after timed out on (the device takes no new command
while its reply buffer is full). If the board has only just booted, give
it a couple of seconds before the first command.

Not the real companion app (`companion-app/` -- not built yet) -- this is
the plain script that proves the protocol and USB vendor interface
actually work before investing in that.
