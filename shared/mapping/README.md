# shared/mapping/

The file a custom TILES mapping travels in: the companion app exports
it, `manuals/` turns it into a manual. Owners: Mats2000 and Samrepasky4
(it mirrors the firmware's settings, which the app edits).

## Status: not frozen

Until it is, the reference shape is
`companion-app/fixtures/tiles-settings.json` — the same data
`tools/tiles_control.py schema`, `list` and `scales` read from a unit:

- `firmware` — the firmware version the values came from.
- `settings` — every setting: `id`, `key` (`pedal.mode`, `color.root`,
  ...), `type`, allowed `values` or range, `default`, current `value`,
  `persist`.
- `scales` — the custom scale slots: `slot`, `name`, `intervals`.

## Before manuals depend on it

Sam and Sebastian, approved by Mats2000, in one pull request here:

1. Name the format and give it a `format` and `version` field.
2. Add what a mapping holds beyond settings (per-pad notes and colours,
   the mapping's name, user labels), as the app stores it.
3. Add a JSON Schema and at least two example files to this folder.
4. Have the app's export and the manual generator both test against the
   examples in CI.

## Rules once frozen

Adding a field is fine. Renaming or removing one, or changing what it
means, bumps `version`, and the app and `manuals/` are updated in the
same pull request.
