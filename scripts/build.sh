#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/vcpkg-main-menu-video-player"
F4SE_OG_SDK="${MMVP_F4SE_OG_SDK:-${HOME}/Games/FO4 Test List/mods/Fallout 4 Script Extender (F4SE)/Root/src}"
mkdir -p "${CACHE_DIR}"

if [[ ! -f "${F4SE_OG_SDK}/f4se/f4se/PapyrusNativeFunctions.h" ]]; then
    echo "F4SE 0.6.23 SDK not found at: ${F4SE_OG_SDK}" >&2
    echo "Set MMVP_F4SE_OG_SDK to the extracted official SDK root." >&2
    exit 2
fi

BUILDER_IMAGE="localhost/main-menu-video-player-builder:latest"
if ! podman image exists "${BUILDER_IMAGE}"; then
    podman build \
        --tag "${BUILDER_IMAGE}" \
        --file "${PROJECT_DIR}/Containerfile" \
        "${PROJECT_DIR}"
fi

podman run --rm \
    --entrypoint /work/scripts/container-build.sh \
    -v "${PROJECT_DIR}:/work:Z" \
    -v "${CACHE_DIR}:/vcpkg-cache:Z" \
    -v "${F4SE_OG_SDK}:/f4se-og-sdk:ro,Z" \
    "${BUILDER_IMAGE}"
