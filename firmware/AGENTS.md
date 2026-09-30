# firmware/AGENTS.md

Raspberry Pi Pico 2 (RP2350) firmware, Pico SDK C + TinyUSB. Nearly all
active work in this repo happens here, iteratively against real
hardware. Start with `README.md` (module map, non-negotiables, status).

## Build

```bash
cmake -S firmware -B firmware/build -DPICO_BOARD=pico2
cmake --build firmware/build -j4
```

**`-DPICO_BOARD=pico2` is required.** Without it cmake targets the
RP2040, the build still succeeds, and the RP2350 bootrom rejects the
image: it looks like "won't boot", not a build error. If an old
`firmware/build/` might be misconfigured, delete it and reconfigure.

A clean build has **zero warnings**. Run the host tests with
`firmware/test/run.sh` (no hardware or SDK needed).

## Flash

Use `picotool` (`brew install picotool`), not drag-and-drop UF2 (a bad
UF2 copied in Finder fails silently). With the board running the app:

```bash
tools/flash.sh                                   # one board connected
TILES_SERIAL=F1A60E66E44C9D4B tools/flash.sh     # pick a board by chip ID
```

It runs `picotool load -f --vid 0x1209 --pid 0x0001 -x -v
--ignore-partitions firmware/build/src/sentia_tiles_firmware.uf2`:
`-f` reboots the board into BOOTSEL over the USB reset interface (no
button), flashes, verifies and reboots. That interface is pico-sdk's
`TUD_RPI_RESET_DESCRIPTOR`, added by hand in
`src/midi/usb_descriptors.c` because this project owns its USB
descriptors. The `--vid/--pid` are required for a non-stock USB ID;
without them picotool reports no device. It also tries the old ID
(`0x2e8a 0x100a`), and a board already in BOOTSEL is flashed as is.
`picotool info -a` shows the mode.

One build runs on every board (settings are saved in flash and survive
reflashing). The only per-unit value is the "Unit N/M" label in
`src/board/unit_id.h`, shown in the CDC interface name and the INFO
reply; set it before building for a specific unit.

Other ways in: `tools/tiles_control.py reboot bootsel` / `reboot app`
over the settings interface (`tools/README.md`), or holding BOOTSEL at
power-up. The manual route, confirming which board is in BOOTSEL:

```bash
~/.venvs/tiles-tools/bin/python tools/tiles_control.py reboot bootsel
sleep 2 && picotool info -a | grep chipid
picotool load -x -v --ignore-partitions firmware/build/src/sentia_tiles_firmware.uf2
```

Give a freshly booted board a couple of seconds before talking to its
settings shell.

**Checking MIDI on real hardware.** When a bug could be in the firmware
or the host (DAW, plugin, routing), look at the bytes on the wire first:
a small CoreMIDI monitor on the "SENTIA TILES MIDI" / "SENTIA TILES DAW"
sources is what separated a host bug from a firmware bug in the sustain
pedal investigation. Temporary `printf()` traces on the USB-CDC console
are fine for this, only on rare events (never per scan), with a host
draining the port, and removed before committing.

## Working with feedback from hardware

1. Implement the change.
2. Build clean (zero warnings) and run `test/run.sh`.
3. Update the docs (see below).
4. Commit (repo-root `AGENTS.md` has the commit-trailer rule) and push
   when asked.
5. Flash, confirm it boots.
6. Someone plays it on the real board and reports back. Repeat.

## Comments and history

**Code comments describe the code as it is now**, briefly:

- What a function, constant or block does, and **why** when it isn't
  obvious: the hardware constraint, the measured data a constant came
  from, the invariant that must hold, the approach that was tried and
  failed (in one line, so nobody tries it again).
- Say when a constant is a first guess vs. measured.
- No change narration ("used to", "now", "this round"), no quoted
  feedback, no references to "the previous version". That belongs in
  the commit message and the module's `HISTORY.md`.
- Keep lines under ~80 columns; don't write `*/` or `/*` inside a
  comment.

**READMEs describe the current design.** Each module README is a map of
its files plus the rules that aren't obvious from one file.

**History is kept, not deleted:**

- `src/services/HISTORY.md`, `src/midi/HISTORY.md`,
  `firmware/HISTORY.md`: the chronological development logs, with the
  hardware findings, tester feedback and dead ends behind each change.
  Grep them for the service, constant or gesture you're about to touch;
  many thresholds exist because of a specific bug fixed on real
  hardware. Append new entries there when a change has a story worth
  keeping.
- The full, long-form comments from before the 2026-09-30 cleanup are
  at git tag `archive/full-comments-2026-09-30`:

  ```bash
  git show archive/full-comments-2026-09-30:firmware/src/services/expression.c
  ```
- `git log -p` on a file shows every change with its message.

## Non-negotiables

`README.md` has the list (hardware safety, no unbounded waits, no
periodic printing). Hardware ground truth: `../docs/hardware/`.
