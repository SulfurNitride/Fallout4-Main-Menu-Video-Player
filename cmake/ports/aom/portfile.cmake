# AOM assumes every compiler for which CMake sets MSVC accepts SSSE3/SSE4
# intrinsics without an explicit target flag. That is true for cl.exe but not
# clang-cl. Keep AOM's normal -m<isa> per-target flags when using Clang.
set(_upstream_port "${VCPKG_ROOT_DIR}/ports/aom")
file(READ "${_upstream_port}/portfile.cmake" _portfile)

set(_clang_cl_fix [=[
vcpkg_replace_string(
    "${SOURCE_PATH}/build/cmake/aom_optimization.cmake"
    "if(MSVC)\n    get_msvc_intrinsic_flag(\"\${flag}\" \"flag\")\n  endif()"
    "if(MSVC AND NOT CMAKE_C_COMPILER_ID MATCHES \"Clang\")\n    get_msvc_intrinsic_flag(\"\${flag}\" \"flag\")\n  endif()"
)

]=])
string(REPLACE
    "vcpkg_cmake_configure("
    "${_clang_cl_fix}vcpkg_cmake_configure("
    _portfile
    "${_portfile}"
)

set(_patched_portfile "${CURRENT_BUILDTREES_DIR}/mmvp-portfile.cmake")
file(WRITE "${_patched_portfile}" "${_portfile}")
set(CURRENT_PORT_DIR "${_upstream_port}")
include("${_patched_portfile}")
