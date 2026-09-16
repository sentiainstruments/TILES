# tools/

Codegen and helper scripts. Primary planned job: take
`shared/board-map/` and `shared/protocol/` as the single authored source
and generate both the firmware's C headers (`firmware/src/board/pad_config.*`)
and the companion app's TypeScript types (`companion-app/src/shared/`), so
the two sides can't drift out of sync. Also home for calibration-jig or
batch-programming scripts once manufacturing tooling is needed.

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
