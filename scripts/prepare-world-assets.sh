#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -ne 2 ]]; then
    echo "Usage: $0 /path/to/Fallout4/Data /path/to/bsa-ba2-tool" >&2
    exit 2
fi

GAME_DATA_DIR="$(realpath "$1")"
BA2_TOOL="$(realpath "$2")"
if [[ ! -x "${BA2_TOOL}" ]]; then
    echo "Rust-BSA-BA2-Handler CLI is not executable: ${BA2_TOOL}" >&2
    exit 2
fi

ASSET_WORK_DIR="$(mktemp -d /tmp/mmvp-world-assets.XXXXXX)"
trap 'rm -rf "${ASSET_WORK_DIR}"' EXIT

"${BA2_TOOL}" unpack \
    "${GAME_DATA_DIR}/Fallout4 - Meshes.ba2" \
    "${ASSET_WORK_DIR}/meshes"
"${BA2_TOOL}" unpack \
    "${GAME_DATA_DIR}/Fallout4 - Materials.ba2" \
    "${ASSET_WORK_DIR}/materials"

python3 "${PROJECT_DIR}/tools/prepare_world_assets.py" \
    --mesh-root "${ASSET_WORK_DIR}/meshes" \
    --material-root "${ASSET_WORK_DIR}/materials" \
    --output-root "${PROJECT_DIR}/package/experimental"
