#!/usr/bin/env bash
#
# Install the SENTIA TILES Logic Pro integration on macOS.
#
# Builds the native CoreMIDI bridge, installs it to ~/.local/bin, registers a
# per-user LaunchAgent (com.sentia.tiles.logic-bridge) so it starts at login,
# and copies the Logic Pro Scripter presets and MIDI Device Script into
# ~/Music/Audio Music Apps/.
#
# Run from anywhere:
#   bash daw-integration/logic/install/install_logic.sh
#   bash daw-integration/logic/install/install_logic.sh --uninstall
#   bash daw-integration/logic/install/install_logic.sh --stage /tmp/tiles-sandbox
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGIC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

LABEL="com.sentia.tiles.logic-bridge"
SCRIPTER_FILES=(TILES_Controller.js TILES_DISPLAY.js LogicScripterUI.js)

STAGE=""
HEADLESS=0
UNINSTALL=0
SKIP_LAUNCH=0
UNIT=""

usage() {
    cat <<'EOF'
Usage: install_logic.sh [options]

  (no option)        Build and install the bridge, LaunchAgent and Logic assets.
  --uninstall        Unload the LaunchAgent and remove the installed files.
  --stage DIR        Install under DIR (sandbox) instead of the home folder;
                     files are staged but the LaunchAgent is not loaded.
  --headless         Start the bridge with no macOS menu-bar item.
  --no-load          Install the LaunchAgent but do not load it now.
  --unit N           Bind the bridge to SENTIA TILES unit N (1-4); default is
                     to connect to whichever unit is present.
  -h, --help         Show this help.

Requires macOS and the Xcode command line tools (clang, cmake optional).
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --uninstall) UNINSTALL=1 ;;
        --stage)
            [ "$#" -ge 2 ] || die "--stage needs a directory"
            STAGE="$2"; shift ;;
        --headless) HEADLESS=1 ;;
        --no-load) SKIP_LAUNCH=1 ;;
        --unit)
            [ "$#" -ge 2 ] || die "--unit needs a value"
            UNIT="$2"; shift
            case "$UNIT" in 1|2|3|4) ;; *) die "--unit must be 1, 2, 3 or 4" ;; esac ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage; exit 2 ;;
    esac
    shift
done

[ "$(uname -s)" = "Darwin" ] || die "the Logic Pro bridge requires macOS"

BASE="${STAGE:-$HOME}"
BIN_DIR="${BASE}/.local/bin"
BIN="${BIN_DIR}/tiles-logic-bridge"
PLIST_DIR="${BASE}/Library/LaunchAgents"
PLIST="${PLIST_DIR}/${LABEL}.plist"
LOG_DIR="${BASE}/Library/Logs/SENTIA"
APPS_DIR="${BASE}/Music/Audio Music Apps"
MIDI_SCRIPT_DIR="${APPS_DIR}/MIDI Device Scripts/Sentia/TILES"

launch_domain="gui/$(id -u)"

# Remove the LaunchAgent by every method that has ever worked across macOS
# releases, ignoring "not loaded" failures.
unload_agent() {
    [ -f "$PLIST" ] || return 0
    launchctl bootout "${launch_domain}" "$PLIST" >/dev/null 2>&1 || \
        launchctl unload "$PLIST" >/dev/null 2>&1 || true
}

if [ "$UNINSTALL" -eq 1 ]; then
    unload_agent
    rm -f "$PLIST" "$BIN"
    for f in "${SCRIPTER_FILES[@]}"; do
        rm -f "${APPS_DIR}/${f}"
    done
    rm -f "${MIDI_SCRIPT_DIR}/TILES.lua"
    rm -f "${LOG_DIR}/tiles-logic-bridge.out.log" "${LOG_DIR}/tiles-logic-bridge.err.log"
    echo "SENTIA TILES Logic Pro integration removed."
    exit 0
fi

command -v clang >/dev/null 2>&1 || die "clang not found; install the Xcode command line tools"

mkdir -p "$BIN_DIR" "$PLIST_DIR" "$LOG_DIR" "$APPS_DIR" "$MIDI_SCRIPT_DIR"

echo "Building tiles-logic-bridge (arm64) ..."
bash "${LOGIC_DIR}/bridge/build.sh" "$BIN"
[ -x "$BIN" ] || die "build did not produce $BIN"

ARCHS="$(lipo -archs "$BIN" 2>/dev/null || file "$BIN" 2>/dev/null || true)"
case "$ARCHS" in
    *arm64*) ;;
    *) die "installed bridge is not arm64 (got: ${ARCHS:-unknown})" ;;
esac

for f in "${SCRIPTER_FILES[@]}"; do
    install -m 0644 "${LOGIC_DIR}/scripter/${f}" "${APPS_DIR}/${f}"
done
install -m 0644 "${LOGIC_DIR}/midi-device-script/TILES.lua" "${MIDI_SCRIPT_DIR}/TILES.lua"

# Render the LaunchAgent. Paths are substituted with '|' as the sed delimiter;
# the headless argument is inserted into the ProgramArguments array only when
# requested, so the default install keeps the menu-bar LogicManager.
sed \
    -e "s|__BIN__|${BIN}|g" \
    -e "s|__LOGDIR__|${LOG_DIR}|g" \
    "${SCRIPT_DIR}/${LABEL}.plist" > "$PLIST"

if [ "$HEADLESS" -eq 1 ]; then
    sed -i '' "s|<!--__HEADLESS__-->|<string>--headless</string>|" "$PLIST"
else
    sed -i '' "/<!--__HEADLESS__-->/d" "$PLIST"
fi

if command -v plutil >/dev/null 2>&1; then
    plutil -lint "$PLIST" >/dev/null || die "generated LaunchAgent plist is invalid"
fi

if [ -n "$STAGE" ]; then
    echo "Staged the SENTIA TILES Logic Pro integration under ${BASE}."
    echo "  bridge:       ${BIN}"
    echo "  LaunchAgent:  ${PLIST}"
    echo "  Logic assets: ${APPS_DIR}"
    echo "LaunchAgent not loaded in a staged install."
    exit 0
fi

if [ "$SKIP_LAUNCH" -eq 0 ]; then
    unload_agent
    if ! launchctl bootstrap "${launch_domain}" "$PLIST" >/dev/null 2>&1; then
        launchctl load -w "$PLIST" >/dev/null 2>&1 || \
            die "could not load the LaunchAgent; see $PLIST"
    fi
    launchctl enable "${launch_domain}/${LABEL}" >/dev/null 2>&1 || true
    echo "LaunchAgent loaded (${LABEL})."
else
    echo "LaunchAgent installed but not loaded (--no-load)."
fi

cat <<EOF
SENTIA TILES Logic Pro integration installed.
  bridge:       ${BIN}
  LaunchAgent:  ${PLIST}
  Logic assets: ${APPS_DIR}
  logs:         ${LOG_DIR}/tiles-logic-bridge.err.log

Next, in Logic Pro: Settings > MIDI > Inputs enable "TILES Logic MPE" and
"SENTIA TILES N Parts", then Control Surfaces > Setup add Mackie Control on
"TILES Logic MCU". See daw-integration/logic/l_README.md for the full setup.
EOF
