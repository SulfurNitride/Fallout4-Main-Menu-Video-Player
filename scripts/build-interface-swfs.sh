#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${MMVP_SWF_OUTPUT_DIR:-${PROJECT_DIR}/build/interface}"

if [[ -n "${FLEX_HOME:-}" && -x "${FLEX_HOME}/bin/mxmlc" ]]; then
    MXMLC="${FLEX_HOME}/bin/mxmlc"
elif command -v mxmlc >/dev/null 2>&1; then
    MXMLC="$(command -v mxmlc)"
    FLEX_HOME="$(cd "$(dirname "${MXMLC}")/.." && pwd)"
else
    echo "Apache Flex mxmlc was not found. Set FLEX_HOME or add mxmlc to PATH." >&2
    exit 2
fi

if [[ -n "${JAVA_HOME:-}" && -x "${JAVA_HOME}/bin/java" ]]; then
    export PATH="${JAVA_HOME}/bin:${PATH}"
elif ! command -v java >/dev/null 2>&1; then
    echo "Java was not found. Set JAVA_HOME or add java to PATH." >&2
    exit 2
fi

PLAYERGLOBAL="${FLEX_HOME}/frameworks/libs/player/10.2/playerglobal.swc"
FRAMEWORK_LIBS="${FLEX_HOME}/frameworks/libs"
if [[ ! -f "${PLAYERGLOBAL}" ]]; then
    echo "Flash Player 10.2 playerglobal.swc was not found at ${PLAYERGLOBAL}." >&2
    exit 2
fi

mkdir -p "${OUTPUT_DIR}"

COMMON_ARGS=(
    "-compiler.external-library-path=${PLAYERGLOBAL}"
    "-compiler.library-path=${FRAMEWORK_LIBS}"
    "-compiler.source-path+=${PROJECT_DIR}/interface/shared"
    "-compiler.debug=false"
    "-compiler.optimize=true"
    "-target-player=10.2"
    "-swf-version=10"
    "-static-link-runtime-shared-libraries=true"
    "-default-size=826,700"
    "-default-frame-rate=60"
)

"${MXMLC}" \
    "${COMMON_ARGS[@]}" \
    "-output=${OUTPUT_DIR}/MMVPBrowser.swf" \
    "${PROJECT_DIR}/interface/browser/MMVPBrowser.as"

"${MXMLC}" \
    "${COMMON_ARGS[@]}" \
    "-output=${OUTPUT_DIR}/MMVPPlayer.swf" \
    "${PROJECT_DIR}/interface/player/MMVPPlayer.as"

python3 "${PROJECT_DIR}/scripts/validate-interface-swfs.py" \
    "${OUTPUT_DIR}/MMVPBrowser.swf" \
    "${OUTPUT_DIR}/MMVPPlayer.swf"

echo "Built validated interface SWFs under ${OUTPUT_DIR}"
