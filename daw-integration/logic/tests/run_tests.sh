#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGIC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TMP_C_BIN="$(mktemp "${TMPDIR:-/tmp}/test_logic_core.XXXXXX")"
TMP_OBJ="$(mktemp "${TMPDIR:-/tmp}/tiles_logic_core.XXXXXX.o")"
TMP_CPP_BIN="$(mktemp "${TMPDIR:-/tmp}/test_studio_link.XXXXXX")"
TMP_MGR_BIN="$(mktemp "${TMPDIR:-/tmp}/test_logic_manager.XXXXXX")"
TMP_LOG_BIN="$(mktemp "${TMPDIR:-/tmp}/test_midi_logger.XXXXXX")"
TMP_MON_BIN="$(mktemp "${TMPDIR:-/tmp}/test_midi_monitor.XXXXXX")"
trap 'rm -f "${TMP_C_BIN}" "${TMP_OBJ}" "${TMP_CPP_BIN}" "${TMP_MGR_BIN}" "${TMP_LOG_BIN}" "${TMP_MON_BIN}"' EXIT

CC_BIN="${CC:-cc}"
CXX_BIN="${CXX:-c++}"

"${CC_BIN}" -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
    "${LOGIC_DIR}/bridge/tiles_logic_core.c" \
    "${SCRIPT_DIR}/test_logic_core.c" \
    -o "${TMP_C_BIN}"
"${TMP_C_BIN}"

"${CC_BIN}" -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -c \
    "${LOGIC_DIR}/bridge/tiles_logic_core.c" \
    -o "${TMP_OBJ}"

"${CXX_BIN}" -std=c++17 -O2 -Wall -Wextra -Werror -Wpedantic \
    "${TMP_OBJ}" \
    "${LOGIC_DIR}/bridge/StudioLink.cpp" \
    "${SCRIPT_DIR}/test_studio_link.cpp" \
    -o "${TMP_CPP_BIN}"
"${TMP_CPP_BIN}"

"${CXX_BIN}" -std=c++17 -O2 -Wall -Wextra -Werror -Wpedantic \
    "${TMP_OBJ}" \
    "${LOGIC_DIR}/bridge/LogicManager.cpp" \
    "${SCRIPT_DIR}/test_logic_manager.cpp" \
    -o "${TMP_MGR_BIN}" \
    -framework CoreFoundation -framework AppKit -framework Cocoa -lobjc
"${TMP_MGR_BIN}"
"${CXX_BIN}" -std=c++17 -O2 -Wall -Wextra -Werror -Wpedantic \
    "${TMP_OBJ}" \
    "${LOGIC_DIR}/bridge/MIDILogger.cpp" \
    "${SCRIPT_DIR}/test_midi_logger.cpp" \
    -o "${TMP_LOG_BIN}"
"${TMP_LOG_BIN}"

"${CXX_BIN}" -std=c++17 -O2 -Wall -Wextra -Werror -Wpedantic \
    "${TMP_OBJ}" \
    "${LOGIC_DIR}/bridge/MIDIMonitor.cpp" \
    "${SCRIPT_DIR}/test_midi_monitor.cpp" \
    -o "${TMP_MON_BIN}" \
    -framework CoreFoundation -framework AppKit -framework Cocoa -lobjc
"${TMP_MON_BIN}"

if command -v node >/dev/null 2>&1; then
    node "${SCRIPT_DIR}/test_scripter_ui.js"
    node "${SCRIPT_DIR}/test_tiles_controller.js"
fi

if [ -f "${SCRIPT_DIR}/test_install_scripts.sh" ]; then
    bash "${SCRIPT_DIR}/test_install_scripts.sh"
fi
