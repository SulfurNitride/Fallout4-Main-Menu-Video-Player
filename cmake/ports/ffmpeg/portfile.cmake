set(_upstream_port "${VCPKG_ROOT_DIR}/ports/ffmpeg")
file(READ "${_upstream_port}/portfile.cmake" _portfile)

string(REPLACE
    "set(OPTIONS_CROSS \"--enable-cross-compile\")"
    [=[if("zimg" IN_LIST FEATURES)
    string(APPEND OPTIONS " --enable-libzimg")
else()
    string(APPEND OPTIONS " --disable-libzimg")
endif()

set(OPTIONS_CROSS "--enable-cross-compile")]=]
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
