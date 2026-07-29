#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -lt 1 ]]; then
    echo "Usage: $0 /path/to/Fallout4/Data [output.esp]" >&2
    exit 2
fi

GAME_DATA_DIR="$(realpath "$1")"
OUTPUT_PATH="${2:-${PROJECT_DIR}/package/common/MMVP_WorldScreens.esp}"
OUTPUT_DIR="$(dirname "$(realpath -m "${OUTPUT_PATH}")")"
OUTPUT_FILE="$(basename "${OUTPUT_PATH}")"
NUGET_CACHE="${XDG_CACHE_HOME:-${HOME}/.cache}/nuget-mmvp"

mkdir -p "${OUTPUT_DIR}" "${NUGET_CACHE}"

podman run --rm \
    -v "${PROJECT_DIR}:/work:Z" \
    -v "${GAME_DATA_DIR}:/game-data:ro,Z" \
    -v "${OUTPUT_DIR}:/output:Z" \
    -v "${NUGET_CACHE}:/root/.nuget/packages:Z" \
    -w /work \
    mcr.microsoft.com/dotnet/sdk:9.0 \
    dotnet run --project tools/WorldPluginGenerator --configuration Release -- \
        --data-dir /game-data \
        --output "/output/${OUTPUT_FILE}"
