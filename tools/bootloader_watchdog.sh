#!/usr/bin/env bash
#
# Watches for TILES boards that have spontaneously landed in the
# RP2350's own ROM USB bootloader (as opposed to a normal watchdog
# reboot back into the application) and kicks them straight back into
# the app the instant they're seen -- no reflash needed, since flash
# contents are untouched by this failure mode.
#
# Why this exists: real-hardware testing found TILES boards
# periodically dropping off USB and reconnecting as the bootloader
# itself ("RP2350 Boot"), roughly as often as landing back in the
# app -- confirmed via macOS's own kernel USB log
# (AppleUSBHostPort::enumerateDeviceComplete_block_invoke showing
# "RP2350 Boot" instead of "SENTIA TILES"). No firmware fix is
# possible for this specific case: the chip's mask ROM is running,
# not this project's code, so nothing flashed can react to it. This
# script is the practical mitigation -- automatic, near-instant
# recovery from the outside, in place of a manual replug/reflash.
#
# --vid/--pid scope every picotool call to Raspberry Pi's own
# bootloader VID/PID (0x2e8a/0x000f) specifically, rather than
# picotool's own default "check every connected device" discovery --
# deliberately, since that default probes every USB device on the
# system (confirmed via the same kernel log: picotool attempting to
# open an unrelated Novation Launchkey) and this runs continuously
# rather than as a one-off check.
#
# ONLY run this during actual playing/practice/performance, not while
# a board is deliberately being reflashed -- it will fight a manual
# `picotool load` by kicking a device back into the app before the
# flash tool gets to touch it. Stop this (Ctrl+C) before handing a
# board back over for firmware work, start it again once you're back
# to playing.

set -euo pipefail

TILES_VID="0x2e8a"
TILES_PID="0x000f" # RP2350 ROM bootloader's own PID -- NOT the application's PID (0x100a)
POLL_INTERVAL_S="0.5"

log() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$1"
}

on_exit() {
    log "stopping -- boards left as-is from here on"
    exit 0
}
trap on_exit INT TERM

log "watching for TILES boards stuck in the ROM bootloader (Ctrl+C to stop)"

while true; do
    info_output="$(picotool info --vid "$TILES_VID" --pid "$TILES_PID" 2>/dev/null || true)"

    while IFS= read -r line; do
        if [[ "$line" =~ ^RP2350\ device\ at\ bus\ ([0-9]+),\ address\ ([0-9]+): ]]; then
            bus="${BASH_REMATCH[1]}"
            address="${BASH_REMATCH[2]}"
            log "found a board stuck in the bootloader (bus $bus, address $address) -- rebooting it into the app"
            picotool reboot -a --bus "$bus" --address "$address" >/dev/null 2>&1 || \
                log "  reboot attempt failed (bus $bus, address $address) -- will retry next pass"
        fi
    done <<< "$info_output"

    sleep "$POLL_INTERVAL_S"
done
