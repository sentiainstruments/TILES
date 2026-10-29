#!/usr/bin/env bash
# Builds an update kit to send to someone who updates their own TILES:
# the firmware built once per unit number (1-4), tools/update_tiles.sh and
# tools/tiles_control.py, zipped as dist/TILES-update-<version>.zip.
#
#   tools/make_update_kit.sh
#
# Each unit builds in its own firmware/build-unitN (cmake -DTILES_UNIT=N),
# configured like firmware/build: its CMakeCache gives the SDK and
# toolchain paths unless PICO_SDK_PATH / PICO_TOOLCHAIN_PATH are set.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$ROOT/firmware/build/CMakeCache.txt"
cache_value() { [ -f "$CACHE" ] && sed -n "s/^$1:[A-Z]*=//p" "$CACHE" | head -1; }
SDK="${PICO_SDK_PATH:-$(cache_value PICO_SDK_PATH)}"
TOOLCHAIN="${PICO_TOOLCHAIN_PATH:-$(cache_value PICO_TOOLCHAIN_PATH)}"
[ -n "$SDK" ] || { echo "set PICO_SDK_PATH (or configure firmware/build first, firmware/BUILD.md)" >&2; exit 1; }

# The version, from product_identity.h: alpha.5.3 (alpha.5 for point 0).
ID="$ROOT/firmware/src/midi/product_identity.h"
stage="$(sed -n 's/^#define TILES_FW_STAGE_NAME "\([a-z]*\)".*/\1/p' "$ID")"
build="$(sed -n 's/^#define TILES_FW_BUILD \([0-9]*\)u.*/\1/p' "$ID")"
point="$(sed -n 's/^#define TILES_FW_POINT \([0-9]*\)u.*/\1/p' "$ID")"
VERSION="$stage.$build"
[ "$point" != 0 ] && VERSION="$VERSION.$point"

KIT="$ROOT/dist/TILES-update-$VERSION"
rm -rf "$KIT" "$KIT.zip"
mkdir -p "$KIT/firmware"

for unit in 1 2 3 4; do
    dir="$ROOT/firmware/build-unit$unit"
    echo "== unit $unit"
    cmake -S "$ROOT/firmware" -B "$dir" -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release -DTILES_UNIT="$unit" \
        -DPICO_SDK_PATH="$SDK" ${TOOLCHAIN:+-DPICO_TOOLCHAIN_PATH="$TOOLCHAIN"} >/dev/null
    cmake --build "$dir" -j8 >/dev/null
    cp "$dir/src/sentia_tiles_firmware.uf2" "$KIT/firmware/unit$unit.uf2"
done
echo "$VERSION" >"$KIT/firmware/VERSION"
cp "$ROOT/tools/update_tiles.sh" "$ROOT/tools/tiles_control.py" "$KIT/"
chmod +x "$KIT/update_tiles.sh"
cat >"$KIT/README.txt" <<EOF
SENTIA TILES firmware $VERSION

Updates every TILES plugged into this Mac, each with the build for its own
unit number (it keeps its name, "SENTIA TILES 3", so Live still finds it).

Once: install Homebrew (https://brew.sh), then in Terminal:
    brew install picotool

Then plug in your TILES, open Terminal in this folder and run:
    bash update_tiles.sh            update them
    bash update_tiles.sh --check    only show each board's unit and firmware

The first run sets up a small Python helper (needs internet once). Leave the
pads alone while each board restarts. A board that isn't found: unplug it,
hold its BOOTSEL button while plugging it back in, run the script again.
EOF
(cd "$ROOT/dist" && zip -qr -X "TILES-update-$VERSION.zip" "TILES-update-$VERSION")
echo "Kit: dist/TILES-update-$VERSION.zip"
