#!/usr/bin/env bash
#
# Build the self-contained SENTIA TILES Logic Pro installer and portable test
# kit under dist/. No compiler is needed here: the kit ships sources and the
# installer builds the bridge on the machine it is run on.
#
# Outputs (default <repo>/dist, override with --out DIR):
#   SENTIA-TILES-Logic-Installer.sh   self-extracting, SHA-256 verified
#   SENTIA-TILES-Logic-Installer.sh.sha256
#   SENTIA-TILES-Logic-Test-Kit/      unpacked sources + Install_*.command
#   SENTIA-TILES-Logic-Test-Kit.zip
#   SENTIA-TILES-Logic-Test-Kit.tar.gz
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGIC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${LOGIC_DIR}/../.." && pwd)"

KIT_NAME="SENTIA-TILES-Logic-Test-Kit"
INSTALLER_NAME="SENTIA-TILES-Logic-Installer.sh"
PAYLOAD_MARKER="__TILES_LOGIC_PAYLOAD_BELOW__"

OUT_DIR="${REPO_ROOT}/dist"
MAKE_ZIP=1

usage() {
    cat <<'EOF'
Usage: build_standalone_installer.sh [options]

  --out DIR     Write the installer and test kit into DIR (default: <repo>/dist).
  --no-zip      Skip the .zip test kit (only .tar.gz is written).
  -h, --help    Show this help.
EOF
}

die() { echo "error: $*" >&2; exit 1; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)
            [ "$#" -ge 2 ] || die "--out needs a directory"
            OUT_DIR="$2"; shift ;;
        --no-zip) MAKE_ZIP=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
    shift
done

command -v tar >/dev/null 2>&1 || die "tar not found"
command -v base64 >/dev/null 2>&1 || die "base64 not found"

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        die "no SHA-256 tool (shasum, sha256sum or openssl) found"
    fi
}

copy_tree() {
    local src="$1" dst="$2"
    mkdir -p "$dst"
    ( cd "$src" && tar -cf - --exclude=build --exclude=.DS_Store . ) | ( cd "$dst" && tar -xf - )
}

rm -rf "$OUT_DIR/$KIT_NAME" "$OUT_DIR/$KIT_NAME.zip" "$OUT_DIR/$KIT_NAME.tar.gz" \
       "$OUT_DIR/$INSTALLER_NAME" "$OUT_DIR/$INSTALLER_NAME.sha256"
mkdir -p "$OUT_DIR/$KIT_NAME"

KIT="$OUT_DIR/$KIT_NAME"
copy_tree "$LOGIC_DIR/bridge" "$KIT/bridge"
copy_tree "$LOGIC_DIR/scripter" "$KIT/scripter"
copy_tree "$LOGIC_DIR/midi-device-script" "$KIT/midi-device-script"
copy_tree "$LOGIC_DIR/tests" "$KIT/tests"
copy_tree "$LOGIC_DIR/install" "$KIT/install"
cp "$LOGIC_DIR/MIDI_Map.txt" "$KIT/MIDI_Map.txt"
cp "$LOGIC_DIR/l_README.md" "$KIT/l_README.md"
cp "$LOGIC_DIR/l_HISTORY.md" "$KIT/l_HISTORY.md"

cat > "$KIT/Install_SENTIA_TILES_Logic.command" <<'COMMAND'
#!/usr/bin/env bash
# Double-click on macOS to install the SENTIA TILES Logic Pro integration.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
exec bash "${DIR}/install/install_logic.sh" "$@"
COMMAND
chmod +x "$KIT/Install_SENTIA_TILES_Logic.command"
chmod +x "$KIT/install/install_logic.sh" "$KIT/install/build_standalone_installer.sh"
chmod +x "$KIT/bridge/build.sh" "$KIT/tests/run_tests.sh"

cat > "$KIT/README-TEST-KIT.md" <<'EOF'
# SENTIA TILES Logic Pro test kit

Portable copy of `daw-integration/logic/`. No repository checkout needed.

## Install

On macOS, double-click `Install_SENTIA_TILES_Logic.command`, or run:

    bash install/install_logic.sh

Add `--uninstall` to remove it again, or `--stage DIR` to install into a
sandbox without loading the LaunchAgent.

## Verify

Run the full host test suite (no Logic Pro or hardware needed):

    bash tests/run_tests.sh

## Build the bridge only

    bash bridge/build.sh

See `l_README.md` for Logic Pro port setup, the three control pages, MPE
routing, and troubleshooting.
EOF

tar -czf "$OUT_DIR/$KIT_NAME.tar.gz" -C "$OUT_DIR" "$KIT_NAME"

if [ "$MAKE_ZIP" -eq 1 ]; then
    if command -v zip >/dev/null 2>&1; then
        ( cd "$OUT_DIR" && zip -qry "$KIT_NAME.zip" "$KIT_NAME" )
    else
        echo "warning: zip not found; writing only $KIT_NAME.tar.gz" >&2
    fi
fi

SHA="$(sha256_of "$OUT_DIR/$KIT_NAME.tar.gz")"

INSTALLER="$OUT_DIR/$INSTALLER_NAME"
cat > "$INSTALLER" <<'HEADER'
#!/usr/bin/env bash
#
# SENTIA TILES Logic Pro installer (self-extracting, SHA-256 verified).
#
# Extracts a portable copy of the Logic Pro integration and installs it, or
# verifies the test kit without installing. Works without a repository.
#
set -euo pipefail

EXPECTED_SHA256="__EXPECTED_SHA256__"
PAYLOAD_MARKER="__TILES_LOGIC_PAYLOAD_BELOW__"

usage() {
    cat <<'USAGE'
Usage: SENTIA-TILES-Logic-Installer.sh [options]

  (no option)          Install the bridge, LaunchAgent and Logic assets.
  --verify-only        Extract and run the full test suite; do not install.
  --extract-only DIR   Extract the test kit into DIR and stop.
  --stage DIR          Install into DIR (sandbox); the LaunchAgent is not loaded.
  --unit N             Bind the bridge to SENTIA TILES unit N (1-4).
  --headless           Run the installed bridge with no menu-bar item.
  --keep               Keep the temporary extraction directory.
  -h, --help           Show this help.
USAGE
}

die() { echo "error: $*" >&2; exit 1; }

sha256_of() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        die "no SHA-256 tool found"
    fi
}

MODE="install"
TARGET=""
UNIT=""
HEADLESS=""
KEEP=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --verify-only) MODE="verify" ;;
        --extract-only)
            [ "$#" -ge 2 ] || die "--extract-only needs a directory"
            MODE="extract"; TARGET="$2"; shift ;;
        --stage)
            [ "$#" -ge 2 ] || die "--stage needs a directory"
            MODE="stage"; TARGET="$2"; shift ;;
        --unit)
            [ "$#" -ge 2 ] || die "--unit needs a value"
            UNIT="$2"; shift
            case "$UNIT" in 1|2|3|4) ;; *) die "--unit must be 1, 2, 3 or 4" ;; esac ;;
        --headless) HEADLESS="--headless" ;;
        --keep) KEEP=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
    shift
done

PAYLOAD_B64="$(mktemp "${TMPDIR:-/tmp}/tiles-logic-payload.XXXXXX")"
PAYLOAD_TGZ="$(mktemp "${TMPDIR:-/tmp}/tiles-logic-kit.XXXXXX.tgz")"
WORK_DIR=""
trap 'rm -f "$PAYLOAD_B64" "$PAYLOAD_TGZ"; if [ -n "$WORK_DIR" ] && [ "$KEEP" -eq 0 ]; then rm -rf "$WORK_DIR"; fi' EXIT

awk -v marker="$PAYLOAD_MARKER" '$0 == marker { found=1; next } found { print }' "$0" > "$PAYLOAD_B64"
[ -s "$PAYLOAD_B64" ] || die "installer payload missing (was the file truncated?)"

if ! base64 --decode < "$PAYLOAD_B64" > "$PAYLOAD_TGZ" 2>/dev/null; then
    base64 -D -i "$PAYLOAD_B64" -o "$PAYLOAD_TGZ" 2>/dev/null || \
        base64 -D < "$PAYLOAD_B64" > "$PAYLOAD_TGZ"
fi

ACTUAL_SHA256="$(sha256_of "$PAYLOAD_TGZ")"
[ "$ACTUAL_SHA256" = "$EXPECTED_SHA256" ] || \
    die "checksum mismatch (expected $EXPECTED_SHA256, got $ACTUAL_SHA256)"

if [ "$MODE" = "extract" ]; then
    mkdir -p "$TARGET"
    tar -xzf "$PAYLOAD_TGZ" -C "$TARGET" --strip-components=1
    echo "SENTIA TILES Logic test kit extracted to $TARGET"
    exit 0
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tiles-logic-work.XXXXXX")"
tar -xzf "$PAYLOAD_TGZ" -C "$WORK_DIR"
KIT="$WORK_DIR/SENTIA-TILES-Logic-Test-Kit"
[ -d "$KIT" ] || die "payload did not contain the test kit"

if [ "$MODE" = "verify" ]; then
    TILES_LOGIC_KIT_VERIFY=1 bash "$KIT/tests/run_tests.sh"
    echo "SENTIA TILES Logic test kit verified."
    exit 0
fi

ARGS=()
[ -n "$UNIT" ] && ARGS+=(--unit "$UNIT")
[ -n "$HEADLESS" ] && ARGS+=(--headless)
if [ "$MODE" = "stage" ]; then
    ARGS+=(--stage "$TARGET")
fi

bash "$KIT/install/install_logic.sh" "${ARGS[@]}"
HEADER

sed "s|__EXPECTED_SHA256__|${SHA}|" "$INSTALLER" > "${INSTALLER}.tmp"
mv "${INSTALLER}.tmp" "$INSTALLER"
printf '%s\n\n' "$PAYLOAD_MARKER" >> "$INSTALLER"
base64 < "$OUT_DIR/$KIT_NAME.tar.gz" >> "$INSTALLER"
chmod +x "$INSTALLER"

echo "$SHA  $INSTALLER_NAME" > "$OUT_DIR/$INSTALLER_NAME.sha256"

echo "Built:"
echo "  $OUT_DIR/$INSTALLER_NAME"
echo "  $OUT_DIR/$INSTALLER_NAME.sha256"
echo "  $OUT_DIR/$KIT_NAME/"
echo "  $OUT_DIR/$KIT_NAME.tar.gz"
if [ -e "$OUT_DIR/$KIT_NAME.zip" ]; then
    echo "  $OUT_DIR/$KIT_NAME.zip"
fi
