# Sensor captures

Raw Hall readings from one pad, recorded on a real TILES, for tuning pitch
bend on the computer instead of by feel. `test/run.sh` replays every
capture here. The studio (`tools/studio/`) plays them back, and its "Save
last 8 s" writes here too (`studio-*.csv`; live rows round
`smoothed_depth`, so use this tool for exact LEGACY replays).

## Recording

With TILES plugged in and pitch bend on (square click):

```bash
~/.venvs/tiles-tools/bin/python tools/tiles_control.py capture 8 6 test/captures/unit2-pad8-straight-press.csv
```

That records pad 8 for 6 seconds (the device keeps about 8). Name files
`unit<N>-pad<P>-<gesture>.csv`. One gesture per file:

- `straight-press`: press straight down, light, then medium, then as hard
  as it goes, holding each a second. No tilt. Any bend here is the problem
  to fix.
- `tilt-left`, `tilt-right`: press, then lean and hold.
- `vibrato`: press, then wiggle side to side.
- `strike`: quick hard hits.

## Replaying

```bash
cc -std=c11 -Isrc/services test/bend_replay.c src/services/bend_core.c -lm -o /tmp/bend_replay
/tmp/bend_replay test/captures/unit2-pad8-straight-press.csv
```

It replays every capture in all three bend modes (`legacy`, `cosine`,
`path`) side by side. It first checks its replay of the device's own mode
gives exactly what the device sent, then prints each note's peak bend,
time bent and 95th-percentile |delta| per mode, and what the press path
learner made of the strokes. Pass another sensitivity as a second argument,
and `--trace trace.csv` for every sample's result per mode.

A first line `# bend mode=... sensitivity=... bx=... by=... strokes=...` is
the device's setup when recording started (Alpha 6.7), and `# tune
key=value ...` the live tuning that differed from the defaults (Alpha 6.8);
the replay starts from both. Columns:
`t_ms,x,y,z,depth,smoothed_depth,bend,flags,state,velocity,aftertouch,strike_ms,peak_depth`
(Alpha 6.6 files stop at `state`, 6.7 at `aftertouch`). Flags: 1 valid
sample, 2 touched, 4 bend started (`bend` = its centre), 8 bend stepped, 16
bend produced (`bend` = the value sent), 32 a stroke sample for the press
path, 64 a stroke began, 128 the note fired (`velocity`, `strike_ms` touch
to the strike depth, `peak_depth`), 256 a held note's pass
(`smoothed_depth`, `aftertouch`), 512 the note ended (`velocity` = release
velocity).

The studio's guided calibration saves one capture per step here,
`unit<N>-pad<P>-<step>-<time>.csv` (strikes, straight, lean, vibrato).
