#!/usr/bin/env bash
# Flashes SENTIA TILES firmware onto a board that is RUNNING the app -- no
# BOOTSEL button. Host-side (Mac/Linux), needs `picotool` (brew install picotool).
#
#   tools/flash.sh                          # the default build output, the only board connected
#   tools/flash.sh path/to/firmware.uf2
#   TILES_SERIAL=F1A60E66E44C9D4B tools/flash.sh   # pick one board when several are connected
#
# Why the --vid/--pid: `picotool load -f` only recognizes a running board by
# itself when it uses one of Raspberry Pi's own stock USB product IDs. TILES
# doesn't (firmware/src/midi/product_identity.h), and with no ID given picotool
# reports "No accessible RP-series devices in BOOTSEL mode were found" -- the
# long-standing "-f never finds board 2" problem. Given the ID, it finds the
# board through its standard reset interface, reboots it into BOOTSEL, follows
# it by USB serial number (the chip ID), flashes, verifies, and reboots it
# back into the app.
#
# Tries the current ID first, then the one firmware used before 2026-09-29,
# so an older board can still be updated. A board already sitting in BOOTSEL
# (button, or a crash into the ROM bootloader) is flashed as is.
set -euo pipefail

UF2="${1:-$(dirname "$0")/../firmware/build/src/sentia_tiles_firmware.uf2}"
[ -f "$UF2" ] || { echo "no such file: $UF2 (build first -- see firmware/AGENTS.md)" >&2; exit 1; }

SER_ARGS=()
[ -n "${TILES_SERIAL:-}" ] && SER_ARGS=(--ser "$TILES_SERIAL")

# Must match firmware/src/midi/product_identity.h (current first, then legacy).
IDS=("0x1209 0x0001" "0x2e8a 0x100a")

if picotool info ${SER_ARGS[@]+"${SER_ARGS[@]}"} >/dev/null 2>&1; then
    echo "Board already in BOOTSEL -- flashing it."
    exec picotool load ${SER_ARGS[@]+"${SER_ARGS[@]}"} -x -v --ignore-partitions "$UF2"
fi

for id in "${IDS[@]}"; do
    read -r vid pid <<<"$id"
    echo "Trying a running board with USB ID $vid:$pid ..."
    if picotool load -f --vid "$vid" --pid "$pid" ${SER_ARGS[@]+"${SER_ARGS[@]}"} -x -v --ignore-partitions "$UF2"; then
        exit 0
    fi
done

echo "No running TILES board found. Is it plugged in (and not frozen)? As a last resort, hold BOOTSEL while plugging in and run this again." >&2
exit 1
