# shared/footprint/

TILES' face as every Sentia manual and guide draws it, taken from the
instrument footprint (`TILES lineup/buttons&pads.ai`), in points, in a
`0 0 792 613` box. Owner: Mats2000.

- `tiles-footprint.json` — the six keys (outline path, placed at `x`,
  `y`) and the 24-pad grid (origin, step, size, corner radius).
- `tiles-face.svg` — the same, ready to use: ids `key-minus`,
  `key-plus`, `key-triangle`, `key-diamond`, `key-square`, `key-circle`
  and `pad-1` … `pad-24`.

Pads 1–24 run row by row from the top left, six across and four down:
the same numbering as `docs/hardware/sentia_tiles_board_map_v1.json`
and the firmware.

If the footprint file changes, regenerate both files from it in one pull
request. How to draw with them: `docs/style/manuals.md`.
