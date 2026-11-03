# Calibration studio

What one key's Hall sensor reads, and what velocity, pressure and pitch
bend make of it, live in a browser. Every sample goes through the
firmware's own `bend_core.c` (built for this computer from
`firmware/test/bend_replay.c --pipe`), so the page shows all three bend
modes side by side, whichever one the board plays.

```
~/.venvs/tiles-tools/bin/python tools/studio/studio.py      # then open http://localhost:8770
```

Needs a C compiler (`cc`) and, for a board, the `tiles-tools` venv with
pyusb (`tools/README.md`). Close the companion app or any other tool using
the settings interface first. `TILES_STUDIO_PORT` picks another port.

## Sources

- **TILES board**: Alpha 6.7 or later. Streams the chosen pad with
  `CAPTURE WATCH`, every pad's depth with `HALL` and the learned press
  paths with `BEND PATHS` (`shared/protocol/README.md`). "Board plays" and
  "Full bend at" set `expression.pitch_bend_mode` and
  `expression.pitch_bend_sensitivity` on the board.
- **Simulated key** (`sim.py`): a dipole magnet over a TMAG5273 with
  misalignment, sensor offset, noise and tremor you can set, and a copy of
  the strike logic. Play gestures, or hold the key drawing and drag. The
  dashed outline is what the key really did. Sizes are guesses until a real
  capture measures them.
- **Capture file**: anything in `firmware/test/captures/`, looped.

"Save last 8 s" writes the board's or the sim's last 8 seconds there, ready
for `bend_replay`.

## What it shows

- **The key, as the sensor reads it**: pressed by the depth, leaned by the
  `path` bend; magenta while the note is held. The arrow is the field at
  the sensor, its lean drawn 4×.
- **Pressure**: depth against the strike, full-strike and full-pressure
  depths; pressure as sent (0-127) and the last velocity.
- **Bend, three ways**: each mode's compensated delta against the deadzone
  and full bend, the semitones it gives now, and its peak over the last
  note. On a straight press, any bend is crosstalk.
- **Last 5 seconds**: depth, and each mode's bend.
- **Press path**: X (or Y) against Z for the last 4 s. Stroke samples in
  periwinkle, the pad's learned slope through the note's start, and dashed,
  what `cosine` assumes (the field keeps its direction).
- **Readings**: rest field and noise (hands off), the hardest press as a
  multiple of the rest field, the learned slope, and for a board how often
  the replay agrees with what it sent.
