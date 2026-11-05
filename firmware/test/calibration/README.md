# Calibration reports

What the calibration studio (`tools/studio/`) measured on a board, one
report per run: `unit<N>-<time>.md` to read, and `.json` beside it with
every number (the studio's `/api/state` at that moment).

A report has each pad's rest field and noise, the learned press paths, the
guided steps (hands off, then strikes, straight presses, leans and vibrato
on each test pad) with what each found, the numbers across the test pads,
the suggested values (one set for every pad) and whether they were
applied, the live tuning in effect, and the captures recorded on the way
(in `../captures/`, replayable with `bend_replay`).

Live tuning is lost at power-off. A value that holds up across reports
becomes a default in `firmware/src/services/tune.h` or `bend_core.c`, with
the report named in `services/HISTORY.md`.
