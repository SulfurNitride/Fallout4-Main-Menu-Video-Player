cmake_minimum_required(VERSION 3.24)
project(zimg VERSION 3.0.6 LANGUAGES CXX)

file(GLOB_RECURSE ZIMG_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/*.cpp"
)
list(FILTER ZIMG_SOURCES EXCLUDE REGEX "/arm/")
list(FILTER ZIMG_SOURCES EXCLUDE REGEX "avx512")

add_library(zimg STATIC ${ZIMG_SOURCES})
add_library(zimg::zimg ALIAS zimg)
target_compile_features(zimg PUBLIC cxx_std_14)
target_compile_definitions(zimg PRIVATE ZIMG_X86)
target_include_directories(zimg
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/api>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg"
)

if(MSVC)
    target_compile_options(zimg PRIVATE /EHsc /W3 /wd4244 /wd4267)

    file(GLOB_RECURSE ZIMG_AVX_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/*_avx.cpp"
    )
    file(GLOB_RECURSE ZIMG_AVX2_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/*_avx2.cpp"
    )
    set(ZIMG_F16C_SOURCE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/depth/x86/f16c_ivb.cpp"
    )
    set_source_files_properties(${ZIMG_AVX_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/arch:AVX"
    )
    set_source_files_properties(${ZIMG_AVX2_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/arch:AVX2"
    )
    set_source_files_properties("${ZIMG_F16C_SOURCE}"
        PROPERTIES COMPILE_OPTIONS "/arch:AVX"
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_property(SOURCE "${ZIMG_F16C_SOURCE}" APPEND PROPERTY
            COMPILE_OPTIONS "/clang:-mf16c"
        )
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS zimg
    EXPORT zimgTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/api/zimg.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zimg/api/zimg++.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(EXPORT zimgTargets
    FILE zimgConfig.cmake
    NAMESPACE zimg::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zimg"
)
configure_file(zimg.pc.in zimg.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zimg.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
