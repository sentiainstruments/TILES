#!/usr/bin/env bash
#
# Verifies the install layer: shell syntax, the LaunchAgent plist, and the
# self-extracting standalone installer (build, extract, checksum).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOGIC_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# The standalone installer's --verify-only runs this whole suite; skip the
# installer build there so verification cannot recurse into itself.
if [ "${TILES_LOGIC_KIT_VERIFY:-0}" = "1" ]; then
    echo "Installer build test skipped during standalone kit verification."
    exit 0
fi

bash -n "${LOGIC_DIR}/install/install_logic.sh"
bash -n "${LOGIC_DIR}/install/build_standalone_installer.sh"

# The bridge ships as a native arm64 binary; the build must request that.
grep -q "TARGET_ARCH=\"arm64\"" "${LOGIC_DIR}/bridge/build.sh" || { echo "build.sh does not pin arm64" >&2; exit 1; }
grep -q "CMAKE_OSX_ARCHITECTURES \"arm64\"" "${LOGIC_DIR}/bridge/CMakeLists.txt" || { echo "CMakeLists.txt does not default to arm64" >&2; exit 1; }

if command -v plutil >/dev/null 2>&1; then
    plutil -lint "${LOGIC_DIR}/install/com.sentia.tiles.logic-bridge.plist" >/dev/null
fi

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/tiles-logic-installer.XXXXXX")"
trap 'rm -rf "${TMP_ROOT}"' EXIT

bash "${LOGIC_DIR}/install/build_standalone_installer.sh" --out "${TMP_ROOT}/dist" >/dev/null

INSTALLER="${TMP_ROOT}/dist/SENTIA-TILES-Logic-Installer.sh"
KIT_TGZ="${TMP_ROOT}/dist/SENTIA-TILES-Logic-Test-Kit.tar.gz"

[ -x "${INSTALLER}" ] || { echo "standalone installer not executable" >&2; exit 1; }
[ -s "${KIT_TGZ}" ] || { echo "test kit tarball missing" >&2; exit 1; }
if command -v zip >/dev/null 2>&1; then
    [ -s "${TMP_ROOT}/dist/SENTIA-TILES-Logic-Test-Kit.zip" ] || { echo "test kit zip missing" >&2; exit 1; }
fi

bash "${INSTALLER}" --extract-only "${TMP_ROOT}/extracted" >/dev/null
for rel in install/install_logic.sh install/com.sentia.tiles.logic-bridge.plist \
           bridge/CMakeLists.txt scripter/TILES_Controller.js \
           tests/run_tests.sh Install_SENTIA_TILES_Logic.command; do
    [ -f "${TMP_ROOT}/extracted/${rel}" ] || { echo "extracted kit missing ${rel}" >&2; exit 1; }
done

# A corrupted payload must be refused.
cp "${INSTALLER}" "${TMP_ROOT}/tampered.sh"
printf 'AAAA' >> "${TMP_ROOT}/tampered.sh"
if bash "${TMP_ROOT}/tampered.sh" --extract-only "${TMP_ROOT}/bad" >/dev/null 2>&1; then
    echo "tampered installer was accepted" >&2
    exit 1
fi

echo "All SENTIA TILES installer tests passed."
