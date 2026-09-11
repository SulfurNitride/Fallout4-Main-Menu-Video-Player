# The pinned FFmpeg port tries to download an obsolete MSYS2 snapshot for
# Windows targets. Reuse the host shell for the local Linux cross-build, or the
# MSYS2 installation prepared by GitHub Actions for a native Windows build.
#
# The upstream port also uses cygpath while fixing up pkg-config files. That
# executable is available only in an MSYS2 installation; a Linux-hosted
# cross-build must use CMake's path conversion instead. Keep the compatibility
# shim here so that the overlay remains small and follows the pinned upstream
# port as it changes.
function(_mmvp_convert_path output mode input)
    if(VCPKG_HOST_IS_LINUX)
        # The build and install paths are already native to the Linux host.
        # CMake's normalized form is the equivalent of cygpath -u/-w for this
        # cross-build; no Windows path should be manufactured on the host.
        file(TO_CMAKE_PATH "${input}" converted)
    else()
        if(NOT DEFINED MSYS_ROOT OR "${MSYS_ROOT}" STREQUAL "")
            message(FATAL_ERROR
                "FFmpeg's pkg-config fixup requires an MSYS2 root on a Windows host. "
                "Set MMVP_MSYS2_ROOT to an MSYS2 installation.")
        endif()

        set(cygpath "${MSYS_ROOT}/usr/bin/cygpath.exe")
        if(NOT EXISTS "${cygpath}")
            message(FATAL_ERROR "MSYS2 cygpath.exe was not found at: ${cygpath}")
        endif()

        execute_process(
            COMMAND "${cygpath}" "${mode}" "${input}"
            RESULT_VARIABLE result
            OUTPUT_VARIABLE converted
            ERROR_VARIABLE error_output
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR
                "cygpath ${mode} failed for '${input}' (exit ${result}): ${error_output}")
        endif()
        file(TO_CMAKE_PATH "${converted}" converted)
    endif()

    set("${output}" "${converted}" PARENT_SCOPE)
endfunction()

set(_upstream_port "${VCPKG_ROOT_DIR}/ports/ffmpeg")
file(READ "${_upstream_port}/portfile.cmake" _portfile)
string(REPLACE
    "if(VCPKG_TARGET_IS_WINDOWS)\n    vcpkg_acquire_msys(MSYS_ROOT)\n    set(SHELL \"\${MSYS_ROOT}/usr/bin/bash.exe\")\nelse()\n    set(SHELL /bin/sh)\nendif()"
    "if(VCPKG_HOST_IS_LINUX)\n    set(SHELL /bin/bash)\nelseif(DEFINED ENV{MMVP_MSYS2_ROOT} AND NOT \"\$ENV{MMVP_MSYS2_ROOT}\" STREQUAL \"\")\n    file(TO_CMAKE_PATH \"\$ENV{MMVP_MSYS2_ROOT}\" MSYS_ROOT)\n    set(SHELL \"\${MSYS_ROOT}/usr/bin/bash.exe\")\nelseif(VCPKG_TARGET_IS_WINDOWS)\n    vcpkg_acquire_msys(MSYS_ROOT)\n    set(SHELL \"\${MSYS_ROOT}/usr/bin/bash.exe\")\nelse()\n    set(SHELL /bin/sh)\nendif()"
    _portfile
    "${_portfile}"
)
string(REPLACE
    [=[execute_process(
                COMMAND "${MSYS_ROOT}/usr/bin/cygpath.exe" -u "${CURRENT_INSTALLED_DIR}"
                OUTPUT_VARIABLE CYG_INSTALLED_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )]=]
    [=[_mmvp_convert_path(CYG_INSTALLED_DIR -u "${CURRENT_INSTALLED_DIR}")]=]
    _portfile
    "${_portfile}"
)
string(REPLACE
    [=[execute_process(
                    COMMAND "${MSYS_ROOT}/usr/bin/cygpath.exe" -w "${PATH_VALUE}"
                    OUTPUT_VARIABLE FIXED_PATH
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                )]=]
    [=[_mmvp_convert_path(FIXED_PATH -w "${PATH_VALUE}")]=]
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
