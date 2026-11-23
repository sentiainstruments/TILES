#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_BIN="${1:-${SCRIPT_DIR}/tiles-logic-bridge}"

# The Logic Pro bridge is a native Apple-silicon (arm64) binary. Building it
# explicitly for arm64 keeps the result correct even under a Rosetta shell or
# on an Intel host, and the architecture is verified before we hand it back.
TARGET_ARCH="arm64"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "tiles-logic-bridge requires macOS (CoreMIDI / CoreFoundation)." >&2
    echo "To run the cross-platform protocol/state test suite here, run: ../tests/run_tests.sh" >&2
    exit 1
fi

verify_arch() {
    local bin="$1"
    local archs
    archs="$(lipo -archs "${bin}" 2>/dev/null || file "${bin}" 2>/dev/null || true)"
    case "${archs}" in
        *"${TARGET_ARCH}"*) ;;
        *)
            echo "error: ${bin} was not built for ${TARGET_ARCH} (got: ${archs:-unknown})" >&2
            rm -f "${bin}"
            exit 1
            ;;
    esac
}

if command -v cmake >/dev/null 2>&1; then
    BUILD_DIR="${SCRIPT_DIR}/build"
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="${TARGET_ARCH}"
    cmake --build "${BUILD_DIR}" --target tiles-logic-bridge --parallel
    cp "${BUILD_DIR}/tiles-logic-bridge" "${OUT_BIN}"
    chmod +x "${OUT_BIN}"
    verify_arch "${OUT_BIN}"
    echo "Built ${OUT_BIN} (${TARGET_ARCH}, via CMake)"
    exit 0
fi

TMP_OBJ="$(mktemp "${TMPDIR:-/tmp}/tiles_logic_core.XXXXXX.o")"
trap 'rm -f "${TMP_OBJ}"' EXIT

clang -arch "${TARGET_ARCH}" -std=c11 -O2 -Wall -Wextra -Werror -c \
    "${SCRIPT_DIR}/tiles_logic_core.c" \
    -o "${TMP_OBJ}"

clang++ -arch "${TARGET_ARCH}" -std=c++17 -O2 -Wall -Wextra -Werror \
    -framework CoreMIDI \
    -framework CoreFoundation \
    -framework AppKit \
    -lobjc \
    "${TMP_OBJ}" \
    "${SCRIPT_DIR}/StudioLink.cpp" \
    "${SCRIPT_DIR}/LogicManager.cpp" \
    "${SCRIPT_DIR}/MIDILogger.cpp" \
    "${SCRIPT_DIR}/MIDIMonitor.cpp" \
    "${SCRIPT_DIR}/tiles_logic_bridge.cpp" \
    -o "${OUT_BIN}"

chmod +x "${OUT_BIN}"
verify_arch "${OUT_BIN}"
echo "Built ${OUT_BIN} (${TARGET_ARCH})"
