#!/usr/bin/env bash
set -euo pipefail

cmake \
    -S /work \
    -B /work/build/windows \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=/work/cmake/windows-msvc-clang.cmake \
    -DVCPKG_APPLOCAL_DEPS=OFF \
    -DVCPKG_OVERLAY_PORTS=/work/cmake/ports \
    -DVCPKG_OVERLAY_TRIPLETS=/work/cmake/triplets \
    -DVCPKG_TARGET_TRIPLET=x64-windows-clang-linux \
    -DVCPKG_HOST_TRIPLET=x64-linux \
    -DMMVP_F4SE_OG_SDK=/f4se-og-sdk

cmake --build /work/build/windows --target package_mod --parallel

mkdir -p /work/dist
package_archive="$(
    find /work/build/windows \
        -maxdepth 1 \
        -type f \
        -name 'MainMenuVideoPlayer-*-FOMOD.zip' \
        -printf '%T@ %p\n' |
        sort -nr |
        head -n 1 |
        cut -d' ' -f2-
)"

if [[ -z "${package_archive}" ]]; then
    echo "The FOMOD archive was not produced." >&2
    exit 1
fi

cmake -E copy_if_different "${package_archive}" /work/dist/
