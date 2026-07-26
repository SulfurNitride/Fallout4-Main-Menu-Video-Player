# The pinned FFmpeg port tries to download an obsolete MSYS2 snapshot for
# Windows targets. Reuse the host shell for the local Linux cross-build, or the
# MSYS2 installation prepared by GitHub Actions for a native Windows build.
set(_upstream_port "${VCPKG_ROOT_DIR}/ports/ffmpeg")
file(READ "${_upstream_port}/portfile.cmake" _portfile)
string(REPLACE
    "if(VCPKG_TARGET_IS_WINDOWS)\n    vcpkg_acquire_msys(MSYS_ROOT)\n    set(SHELL \"\${MSYS_ROOT}/usr/bin/bash.exe\")\nelse()\n    set(SHELL /bin/sh)\nendif()"
    "if(VCPKG_HOST_IS_LINUX)\n    set(SHELL /bin/bash)\nelseif(DEFINED ENV{MMVP_MSYS2_ROOT})\n    file(TO_CMAKE_PATH \"\$ENV{MMVP_MSYS2_ROOT}\" MMVP_MSYS2_ROOT)\n    set(SHELL \"\${MMVP_MSYS2_ROOT}/usr/bin/bash.exe\")\nelseif(VCPKG_TARGET_IS_WINDOWS)\n    vcpkg_acquire_msys(MSYS_ROOT)\n    set(SHELL \"\${MSYS_ROOT}/usr/bin/bash.exe\")\nelse()\n    set(SHELL /bin/sh)\nendif()"
    _portfile
    "${_portfile}"
)
string(REPLACE
    "\${CMAKE_CURRENT_LIST_DIR}"
    "${_upstream_port}"
    _portfile
    "${_portfile}"
)

set(_patched_portfile "${CURRENT_BUILDTREES_DIR}/mmvp-portfile.cmake")
file(WRITE "${_patched_portfile}" "${_portfile}")
set(CURRENT_PORT_DIR "${_upstream_port}")
include("${_patched_portfile}")
