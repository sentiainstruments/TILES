# tools/

Host-side helper scripts: `flash.sh` (flash a running board),
`tiles_control.py` (the settings CLI over USB), `current_test.py`
(guided current measurement) and `bootloader_watchdog.sh`. Planned: codegen from `shared/board-map/` and
`shared/protocol/` into the firmware's C headers and the companion app's
TypeScript types, and manufacturing scripts once they're needed.

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

## flash.sh

Flashes a board that's running the app -- no BOOTSEL button:

```
tools/flash.sh                                   # default build output, one board connected
TILES_SERIAL=F1A60E66E44C9D4B tools/flash.sh     # one of several boards (serial = chip ID)
```

With several boards running, picotool can't reboot just one, so given
`TILES_SERIAL` the script first sends that board to BOOTSEL through
`tiles_control.py` (needs its pyusb venv, below).

It's `picotool load -f` with the board's USB ID spelled out (`--vid/--pid`).
Without the ID, picotool only recognizes boards using Raspberry Pi's stock
product IDs, which is why a plain `picotool load -f` never found board 2. See
the script's header and `firmware/AGENTS.md`'s "Flash" section.

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
TILES_SERIAL=D8D37A03B3B6CE95 ~/.venvs/tiles-tools/bin/python tools/tiles_control.py info   # one of several boards
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
  see `firmware/src/services/HISTORY.md` for the data behind the defaults.

**Rebooting into the bootloader for flashing**, no BOOTSEL button --
`flash.sh` (below) is the usual way now; this still works as a fallback:

```
~/.venvs/tiles-tools/bin/python tools/tiles_control.py reboot bootsel
sleep 2 && picotool load -x -v --ignore-partitions firmware/build/src/sentia_tiles_firmware.uf2
```

The tool finds the board by its USB ID (`firmware/src/midi/product_identity.h`
-- the pid.codes test ID `1209:0001` while TILES is pre-production) and also
accepts the ID firmware used before 2026-09-29 (`2E8A:100A`, which turned out
to belong to another product), so an older board can still be updated.

Each command first discards any reply an earlier, interrupted run left
queued on the device -- a `schema` piped through `grep` right after a
reboot once left ~2 KB there, which the next command read as its own
reply and the one after timed out on (the device takes no new command
while its reply buffer is full). If the board has only just booted, give
it a couple of seconds before the first command.

Not the real companion app (`companion-app/` -- not built yet) -- this is
the plain script that proves the protocol and USB vendor interface
actually work before investing in that.

## current_test.py

Guided supply-current measurement with an inline USB-C meter between the
computer and the unit. It steps the unit through fixed states with the
settings shell's bench tests and asks for the meter reading at each,
saving a CSV:

```
TILES_SERIAL=D8D37A03B3B6CE95 ~/.venvs/tiles-tools/bin/python tools/current_test.py results.csv
TILES_SERIAL=D8D37A03B3B6CE95 ~/.venvs/tiles-tools/bin/python tools/current_test.py results.csv --recheck
```

The full pass covers LED brightness steps, 1 and 4 motors, and the USB
worst case; `--recheck` repeats just the combined steps, holding each 3 s
so a motor's start-up current is over before you read. Leave the pads
alone while it runs. The bench tests themselves (firmware 0.2.6+):

```
tiles_control.py test leds 50        # every LED white at 50% of full scale
tiles_control.py test motors 4 100   # pads 1-4's motors at 100% for 8 s (capped at the power mode's voices)
tiles_control.py test off            # back to normal
```

Results and what they changed: `docs/hardware/current-measurements.md`.

