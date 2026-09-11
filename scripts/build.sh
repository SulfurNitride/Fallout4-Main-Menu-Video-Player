#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="${XDG_CACHE_HOME:-${HOME}/.cache}/vcpkg-main-menu-video-player"
mkdir -p "${CACHE_DIR}"

CONTAINER_ENGINE="${MMVP_CONTAINER_ENGINE:-podman}"
if ! command -v "${CONTAINER_ENGINE}" >/dev/null 2>&1; then
    echo "Container engine not found: ${CONTAINER_ENGINE}" >&2
    echo "Install Podman or set MMVP_CONTAINER_ENGINE to another compatible engine." >&2
    exit 127
fi

BUILDER_IMAGE="${MMVP_BUILDER_IMAGE:-localhost/main-menu-video-player-builder:latest}"
BUILD_ARGS=()
if [[ -n "${MMVP_BASE_IMAGE:-}" ]]; then
    BUILD_ARGS+=(--build-arg "MMVP_BASE_IMAGE=${MMVP_BASE_IMAGE}")
fi
if ! "${CONTAINER_ENGINE}" image inspect "${BUILDER_IMAGE}" >/dev/null 2>&1; then
    "${CONTAINER_ENGINE}" build \
        "${BUILD_ARGS[@]}" \
        --tag "${BUILDER_IMAGE}" \
        --file "${PROJECT_DIR}/Containerfile" \
        "${PROJECT_DIR}"
fi

"${CONTAINER_ENGINE}" run --rm \
    --entrypoint /work/scripts/container-build.sh \
    -v "${PROJECT_DIR}:/work:Z" \
    -v "${CACHE_DIR}:/vcpkg-cache:Z" \
    "${BUILDER_IMAGE}"
