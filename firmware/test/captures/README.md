# Sensor captures

Raw Hall readings from one pad, recorded on a real TILES, for tuning pitch
bend on the computer instead of by feel. `test/run.sh` replays every
capture here.

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

It first checks the replay gives exactly what the device sent, then prints
each note's peak depth and bend. Pass another sensitivity as a second
argument, and `--out replay.csv` for the bend per sample.

Columns: `t_ms,x,y,z,depth,smoothed_depth,bend,flags,state`. Flags: 1 valid
sample, 2 touched, 4 note-on (bend started; `bend` = its centre), 8 bend
stepped, 16 bend produced (`bend` = the value sent).
