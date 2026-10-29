#!/usr/bin/env bash
# SENTIA TILES updater (macOS; Linux works too). Ships in an update kit made
# by tools/make_update_kit.sh, next to firmware/unit1.uf2 .. unit4.uf2 and
# tiles_control.py.
#
# Updates every TILES connected to this computer. Each board gets the build
# for its own unit number, so "SENTIA TILES 3" keeps its name and a Live set
# still finds it. The unit number comes from, in order:
#   1. the board's chip ID, for the four pre-production boards (UNITS below);
#   2. what the board reports itself (INFO unit=, or its USB name);
#   3. asking you.
#
#   bash update_tiles.sh            update every connected TILES
#   bash update_tiles.sh --check    only list them: chip ID, unit, firmware
#
# Needs picotool (brew install picotool) and Python 3. The first run makes a
# small Python environment with pyusb in ~/.venvs/tiles-tools.
#
# A board that isn't found or won't reboot: unplug it, hold its BOOTSEL
# button while plugging it back in, and run this again (boards waiting in
# BOOTSEL are updated too).
set -uo pipefail

KIT="$(cd "$(dirname "$0")" && pwd)"
FW="$KIT/firmware"
VERSION="$(cat "$FW/VERSION" 2>/dev/null || echo "?")"
VENV="${TILES_VENV:-$HOME/.venvs/tiles-tools}"
PY="$VENV/bin/python"
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

# Chip ID (the USB serial number) -> unit number, for the boards built so far.
unit_for_chip() {
    case "$1" in
    35CAF92DD823BB90) echo 1 ;;
    F1A60E66E44C9D4B) echo 2 ;;
    3B048C9FD07BA2D9) echo 3 ;;
    D8D37A03B3B6CE95) echo 4 ;;
    *) echo "" ;;
    esac
}

say() { printf '%s\n' "$*"; }
die() { printf 'Error: %s\n' "$*" >&2; exit 1; }

# ---- tools ----
command -v picotool >/dev/null 2>&1 || die "picotool is missing. Install it with:  brew install picotool"
command -v python3 >/dev/null 2>&1 || die "Python 3 is missing. Install it with:  xcode-select --install"
if ! "$PY" -c "import usb.core" >/dev/null 2>&1; then
    say "Setting up a small Python environment for USB access (once) ..."
    python3 -m venv "$VENV" || die "couldn't make $VENV"
    "$VENV/bin/pip" install --quiet pyusb || die "couldn't install pyusb (internet needed once)"
fi
[ -d "$FW" ] || die "no firmware folder next to this script ($FW)"

# Every TILES-looking USB device: "kind<TAB>chip ID<TAB>USB name".
# kind = app (running TILES), bootsel (any RP2350 waiting to be flashed).
list_boards() {
    "$PY" - <<'PYEOF'
import usb.core, usb.util
IDS = [((0x1209, 0x0001), "app"), ((0x2E8A, 0x100A), "app"), ((0x2E8A, 0x000F), "bootsel")]
def text(d, i):
    try:
        return usb.util.get_string(d, i) or ""
    except Exception:
        return ""
for (vid, pid), kind in IDS:
    for d in usb.core.find(find_all=True, idVendor=vid, idProduct=pid):
        name = text(d, d.iProduct)
        # 0x2E8A:0x100A belongs to another product too: only a TILES name counts.
        if (vid, pid) == (0x2E8A, 0x100A) and "TILES" not in name.upper():
            continue
        print("%s\t%s\t%s" % (kind, text(d, d.iSerialNumber).upper(), name))
PYEOF
}

# One running board's INFO (settings interface, firmware 0.2.7 and later).
info_of() {
    TILES_SERIAL="$1" "$PY" "$KIT/tiles_control.py" info 2>/dev/null
}

wait_for() { # kind serial seconds
    local i=0
    while [ "$i" -lt $(($3 * 2)) ]; do
        list_boards | grep -q "^$1	$2	" && return 0
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}

boards="$(list_boards)"
[ -n "$boards" ] || die "no TILES found. Plug one in (or hold BOOTSEL while plugging it in) and run this again."
count_running="$(printf '%s\n' "$boards" | grep -c '^app' || true)"

say ""
say "SENTIA TILES update kit, firmware $VERSION"
say ""
summary=""
while IFS="	" read -r kind serial name; do
    [ -n "$serial" ] || continue
    unit="$(unit_for_chip "$serial")"
    before="?"
    if [ "$kind" = app ]; then
        info="$(info_of "$serial")"
        before="$(printf '%s\n' "$info" | sed -n 's/^firmware=//p' | head -1)"
        [ -n "$before" ] || before="old"
        if [ -z "$unit" ]; then
            unit="$(printf '%s\n' "$info" | sed -n 's/^unit=\([0-9]*\)\/.*/\1/p' | head -1)"
        fi
        if [ -z "$unit" ]; then
            unit="$(printf '%s\n' "$name" | sed -n 's/^SENTIA TILES \([0-9][0-9]*\).*/\1/p')"
        fi
    fi
    where="running"
    [ "$kind" = bootsel ] && where="in BOOTSEL"
    if [ -z "$unit" ]; then
        if [ "$kind" = bootsel ]; then
            say "Board $serial is waiting in BOOTSEL and isn't one of the known TILES."
        else
            say "Board $serial ($name) didn't say its unit number."
        fi
        printf 'Its unit number (1-4), or Enter to skip it: '
        read -r unit </dev/tty
    fi
    say "Unit ${unit:-?}  chip $serial  $where  firmware $before"
    if [ "$CHECK_ONLY" = 1 ] || [ -z "$unit" ]; then
        summary="$summary$(printf 'unit %-3s %s  %s -> (not updated)' "${unit:-?}" "$serial" "$before")\n"
        continue
    fi
    uf2="$FW/unit$unit.uf2"
    if [ ! -f "$uf2" ]; then
        say "  no build for unit $unit in this kit; skipped"
        summary="$summary$(printf 'unit %-3s %s  %s -> skipped (no unit%s.uf2)' "$unit" "$serial" "$before" "$unit")\n"
        continue
    fi

    # Into BOOTSEL: through the settings interface (picks this board out of
    # several); for old firmware without it, picotool's reset, which only
    # works with one board running.
    if [ "$kind" = app ]; then
        if TILES_SERIAL="$serial" "$PY" "$KIT/tiles_control.py" reboot bootsel >/dev/null 2>&1; then
            :
        elif [ "$count_running" = 1 ]; then
            picotool reboot -u -f --vid 0x1209 --pid 0x0001 >/dev/null 2>&1 ||
                picotool reboot -u -f --vid 0x2e8a --pid 0x100a >/dev/null 2>&1
        fi
        if ! wait_for bootsel "$serial" 20; then
            say "  couldn't reboot it into BOOTSEL. Unplug it, hold BOOTSEL while plugging it in, run this again."
            summary="$summary$(printf 'unit %-3s %s  %s -> FAILED (no BOOTSEL)' "$unit" "$serial" "$before")\n"
            continue
        fi
    fi
    say "  flashing unit$unit.uf2 ..."
    if ! picotool load --ser "$serial" -x -v --ignore-partitions "$uf2" >/dev/null 2>&1; then
        say "  flashing failed; it's still in BOOTSEL. Run this again."
        summary="$summary$(printf 'unit %-3s %s  %s -> FAILED (flash)' "$unit" "$serial" "$before")\n"
        continue
    fi
    # Back in the app: confirm what it reports.
    after="?"
    got_unit="?"
    if wait_for app "$serial" 30; then
        sleep 2
        info="$(info_of "$serial")"
        after="$(printf '%s\n' "$info" | sed -n 's/^firmware=//p' | head -1)"
        got_unit="$(printf '%s\n' "$info" | sed -n 's/^unit=\([0-9]*\)\/.*/\1/p' | head -1)"
    fi
    if [ "$got_unit" = "$unit" ]; then
        say "  done: unit $got_unit, firmware $after"
        summary="$summary$(printf 'unit %-3s %s  %s -> %s  OK' "$unit" "$serial" "$before" "$after")\n"
    else
        say "  flashed, but it reports unit ${got_unit:-?} (firmware ${after:-?}); check it"
        summary="$summary$(printf 'unit %-3s %s  %s -> %s  CHECK' "$unit" "$serial" "$before" "${after:-?}")\n"
    fi
done <<EOF
$boards
EOF

say ""
say "Summary"
printf '%b' "$summary"
if [ "$CHECK_ONLY" = 0 ]; then
    say ""
    say "If Ableton Live shows an old name or no (Parts) port: quit Live, unplug TILES,"
    say "open Audio MIDI Setup > Window > Show MIDI Studio, delete the greyed-out"
    say "SENTIA TILES icons, plug TILES back in."
fi
