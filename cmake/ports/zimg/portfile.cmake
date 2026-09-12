vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO sekrit-twc/zimg
    REF f819b14e8f39d1282400b0d9543e8ef73c1b2bbd
    SHA512 ff0260d78753174b3a2c3e05a6b3883167ecd978b27be0fbde08eec6e8b3444cb570e8d0aa05c008520b58ad1a95ed85d01450441df10507e539bc1aae9df34e
    HEAD_REF master
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
     DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/zimg.pc.in"
     DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/zimg)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
