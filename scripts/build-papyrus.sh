#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CAPRICA_VERSION="0.3.0"
CAPRICA_CACHE="${XDG_CACHE_HOME:-${HOME}/.cache}/caprica-mmvp/${CAPRICA_VERSION}"
CAPRICA_EXE="${CAPRICA_CACHE}/Caprica.exe"
CAPRICA_ARCHIVE="${CAPRICA_CACHE}/Caprica.7z"

if [[ ! -f "${CAPRICA_EXE}" ]]; then
    mkdir -p "${CAPRICA_CACHE}"
    curl -fL \
        "https://github.com/Orvid/Caprica/releases/download/v${CAPRICA_VERSION}/Caprica.v${CAPRICA_VERSION}.7z" \
        -o "${CAPRICA_ARCHIVE}"
    7z x -y "${CAPRICA_ARCHIVE}" "-o${CAPRICA_CACHE}" >/dev/null
fi

if [[ -n "${MMVP_WINE64:-}" ]]; then
    WINE64="${MMVP_WINE64}"
elif command -v wine64 >/dev/null 2>&1; then
    WINE64="$(command -v wine64)"
elif [[ -x "${HOME}/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64" ]]; then
    WINE64="${HOME}/.local/share/Steam/steamapps/common/Proton 10.0/files/bin/wine64"
elif [[ -x "${HOME}/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine" ]]; then
    WINE64="${HOME}/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine"
else
    echo "Could not locate Wine or Proton. Set MMVP_WINE64 explicitly." >&2
    exit 2
fi

to_windows_path()
{
    printf 'Z:%s' "$1" | sed 's|/|\\|g'
}

STUBS="$(to_windows_path "${PROJECT_DIR}/tools/PapyrusStubs")"
FLAGS="$(to_windows_path "${PROJECT_DIR}/tools/PapyrusStubs/MMVP_Papyrus_Flags.flg")"
SOURCES="$(to_windows_path "${PROJECT_DIR}/package/common/Scripts/Source/User")"
OUTPUT="$(to_windows_path "${PROJECT_DIR}/package/common/Scripts")"

mkdir -p "${PROJECT_DIR}/package/common/Scripts"
pushd "${PROJECT_DIR}/package/common/Scripts/Source/User" >/dev/null
for source in \
    'MMVP\HolotapeQuest.psc' \
    'Fragments\Terminals\TERM_MMVP_PlayerTerminal_00000808.psc'
do
    "${WINE64}" "${CAPRICA_EXE}" \
        --ignorecwd \
        --enable-debug-info=0 \
        --game fallout4 \
        --flags "${FLAGS}" \
        --import "${STUBS}" \
        --import "${SOURCES}" \
        --output "${OUTPUT}" \
        --optimize \
        --release \
        --final \
        "${source}"
done
popd >/dev/null

echo "Compiled MMVP Papyrus scripts under package/common/Scripts."
