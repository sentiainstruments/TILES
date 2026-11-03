# Calibration studio

What each key's Hall sensor reads, what velocity, pressure and pitch bend
make of it, a guided calibration that suggests the numbers to change, and
live tuning of every feel constant. A desktop page in the browser.

```
~/.venvs/tiles-tools/bin/python tools/studio/studio.py      # then open http://localhost:8770
```

Needs a C compiler (`cc`) and, for a board, the `tiles-tools` venv with
pyusb (`tools/README.md`). Close the companion app or any other tool using
the settings interface first. `TILES_STUDIO_PORT` picks another port.

Every sample goes through the firmware's own `bend_core.c` and `tune.c`,
built for this computer (`firmware/test/bend_replay.c --pipe`, rebuilt
whenever they change), so the page shows all three bend modes side by side
with the board's live tuning, and the replay of the board's own mode
matches what it sent bit for bit.

## Sources

- **TILES board**: Alpha 6.8 or later (6.7 streams but can't tune).
  Streams the watched pad (`CAPTURE WATCH`), every pad's Hall reading and
  rest baseline (`HALL`), the learned press paths (`BEND PATHS`), board
  health (`INFO`) and the live tuning (`TUNE`): `shared/protocol/README.md`.
- **Simulated key** (`sim.py`): a dipole magnet over a TMAG5273 with
  misalignment, sensor offset, noise and tremor you can set, and a copy of
  the strike logic that follows the tuning. Play gestures, or hold the key
  drawing and drag. The dashed outline is what the key really did. Sizes
  are guesses until a real capture measures them.
- **Capture file**: anything in `firmware/test/captures/`, looped. Tuning
  changes replay it as a what-if.

## The page

Left, live: the key as the sensor reads it (pressed by depth, leaned by the
bend; magenta while the note is held), pressure against the strike, full
velocity and full pressure depths, each bend mode's delta against the
deadzone and full bend plus the vibrato energy against its floor, the last
5 seconds, the press path (X or Y against Z, the learned slope, and dashed
what `cosine` assumes), and the last note broken down: how depth and speed
added up to its velocity, pressure, release velocity, bend and vibrato
peaks.

Right, four tabs:

- **Calibrate**: five steps on the watched pad, about a minute: hands off
  (every pad's rest field and noise), strikes, straight presses, leans,
  vibrato. Each step records a capture and says what it found. Then
  suggested changes, each with its reason, to apply one by one or all at
  once, and a report. On the simulated key the sim plays every step.
- **Tune**: every `TUNE` key by group (strike and velocity, pressure and
  release, key and touch, bend, vibrato, press path learner) with its
  default and range, plus the three saved settings (bend mode, full bend,
  full pressure). Live tuning lasts until the board powers off. "Copy as
  code" gives the changed values as the `#define` lines to paste into
  `tune.h` and `bend_core.c`.
- **Pads**: the 24 pads lit by depth (click one to watch it) and a table of
  each pad's rest field, noise, rest lean and learned press path.
- **Board**: firmware, main-loop timing, Hall and touch health, power, how
  many tuning values are changed, and the files saved this session.

## Shared with scripts and agents

Everything the page shows is one state, also served as JSON:

```
curl -s localhost:8770/api/state      # source, board, settings, tune, pads, notes, calibration, files
curl -s localhost:8770/api/pads       # also: /api/notes, /api/tune, /api/calibration
curl -s -X POST localhost:8770/api/control -d '{"tune": {"key": "bend.deadzone", "value": 0.03}}'
```

`/api/control` takes what the page sends: `{"source": "device"|"sim"|"file"}`,
`{"pad": n}`, `{"tune": {"key", "value"}}`, `{"tune_reset": key|null}`,
`{"setting": {"key", "value"}}`, `{"calibrate": "start"|"record"|"next"|"redo"|"goto"|"stop"}`,
`{"apply": [keys]|"all"}`, `{"save": true}`, `{"report": true}`.

Files land in the repo, so they outlive the session and anyone can read
them:

- `firmware/test/calibration/<unit>-<time>.md` and `.json`: the report
  (README there).
- `firmware/test/captures/`: one capture per calibration step, and "Save
  last 8 s". Each starts with the `# bend` and `# tune` lines that let
  `bend_replay` start where the board was.
