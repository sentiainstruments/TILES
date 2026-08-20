# tools/

Codegen and helper scripts. Primary planned job: take
`shared/board-map/` and `shared/protocol/` as the single authored source
and generate both the firmware's C headers (`firmware/src/board/pad_config.*`)
and the companion app's TypeScript types (`companion-app/src/shared/`), so
the two sides can't drift out of sync. Also home for calibration-jig or
batch-programming scripts once manufacturing tooling is needed.

Nothing here yet.
