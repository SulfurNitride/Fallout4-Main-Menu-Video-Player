cmake_minimum_required(VERSION 3.24)

project(
    MainMenuVideoPlayer
    VERSION 0.1.0
    LANGUAGES CXX RC
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake's llvm-rc wrapper does not inherit clang-cl's /imsvc search paths.
# Expose the SDK headers normally as well so version resources can preprocess.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND MSVC)
    include_directories(SYSTEM
        "/usr/share/msvc/sdk/include/um"
        "/usr/share/msvc/sdk/include/shared"
        "/usr/share/msvc/sdk/include/winrt"
    )
endif()

find_package(minhook CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_path(FFMPEG_INCLUDE_DIR "libavcodec/avcodec.h" REQUIRED)
find_library(AVFORMAT_LIBRARY NAMES avformat REQUIRED)
find_library(AVCODEC_LIBRARY NAMES avcodec REQUIRED)
find_library(AVUTIL_LIBRARY NAMES avutil REQUIRED)
find_library(SWRESAMPLE_LIBRARY NAMES swresample REQUIRED)
find_library(SWSCALE_LIBRARY NAMES swscale REQUIRED)
find_library(AOM_LIBRARY NAMES aom REQUIRED)

set(MMVP_CORE_SOURCES
    src/AudioOutput.cpp
    src/AudioOutput.h
    src/BinkHook.cpp
    src/BinkHook.h
    src/Config.cpp
    src/Config.h
    src/EngineSettings.cpp
    src/EngineSettings.h
    src/F4SEMinimal.h
    src/Log.cpp
    src/Log.h
    src/VideoPlayer.cpp
    src/VideoPlayer.h
)

# Only the Linux-hosted clang-cl cross-build needs this fallback. Native MSVC
# lowers MinHook's __movsb intrinsic itself.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(APPEND MMVP_CORE_SOURCES src/CompatIntrinsics.cpp)
    set_source_files_properties(
        src/CompatIntrinsics.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    )
endif()

add_library(mmvp_core STATIC ${MMVP_CORE_SOURCES})

target_compile_features(mmvp_core PUBLIC cxx_std_23)
target_include_directories(mmvp_core
    PUBLIC "${CMAKE_SOURCE_DIR}/src"
    PRIVATE "${FFMPEG_INCLUDE_DIR}"
)
target_precompile_headers(mmvp_core PRIVATE src/PCH.h)
target_link_libraries(mmvp_core
    PUBLIC
        spdlog::spdlog
    PRIVATE
        minhook::minhook
        ${AVFORMAT_LIBRARY}
        ${AVCODEC_LIBRARY}
        ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY}
        ${SWSCALE_LIBRARY}
        ${AOM_LIBRARY}
        bcrypt
        mfplat
        mfuuid
        ole32
        secur32
        strmiids
        user32
        ws2_32
        xaudio2
)

if(MSVC)
    target_compile_options(mmvp_core PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
        /Zc:preprocessor
        /wd4200
        /wd4201
        /wd4324
    )
endif()

function(add_mmvp_variant target runtime_define output_suffix)
    add_library(${target} SHARED
        src/Plugin.cpp
        cmake/version.rc
    )

    target_compile_features(${target} PRIVATE cxx_std_23)
    target_precompile_headers(${target} PRIVATE src/PCH.h)
    target_compile_definitions(${target} PRIVATE
        ${runtime_define}=1
        MMVP_VERSION="${PROJECT_VERSION}"
        MMVP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        MMVP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        MMVP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    )
    target_link_libraries(${target} PRIVATE mmvp_core)
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "MainMenuVideoPlayer-${output_suffix}"
        PREFIX ""
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /EHsc
            /utf-8
            /Zc:preprocessor
            /wd4200
            /wd4201
            /wd4324
        )
    endif()
endfunction()

add_mmvp_variant(MainMenuVideoPlayerOG MMVP_RUNTIME_OG OG)
add_mmvp_variant(MainMenuVideoPlayerNG MMVP_RUNTIME_NG NG)
add_mmvp_variant(MainMenuVideoPlayerAE MMVP_RUNTIME_AE AE)

add_executable(mmvp_decode_smoke EXCLUDE_FROM_ALL
    tools/DecodeSmoke.cpp
)
target_include_directories(mmvp_decode_smoke PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(mmvp_decode_smoke PRIVATE
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${AOM_LIBRARY}
    bcrypt
    mfplat
    mfuuid
    ole32
    secur32
    strmiids
    user32
    ws2_32
)

set(PACKAGE_ROOT "${CMAKE_BINARY_DIR}/package")
set(PACKAGE_INSTALLER "${PACKAGE_ROOT}/installer")
set(PACKAGE_COMMON "${CMAKE_SOURCE_DIR}/package/common")
set(PACKAGE_ARCHIVE
    "${CMAKE_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}-FOMOD.zip")
file(MAKE_DIRECTORY "${PACKAGE_ROOT}")

configure_file(
    "${CMAKE_SOURCE_DIR}/package/fomod/info.xml.in"
    "${CMAKE_BINARY_DIR}/generated-fomod/info.xml"
    @ONLY
)

add_custom_target(package_mod
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${PACKAGE_ROOT}"
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${PACKAGE_ARCHIVE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${PACKAGE_INSTALLER}/common"
            "${PACKAGE_INSTALLER}/fomod"
            "${PACKAGE_INSTALLER}/runtime/OG/F4SE/Plugins"
            "${PACKAGE_INSTALLER}/runtime/NG/F4SE/Plugins"
            "${PACKAGE_INSTALLER}/runtime/AE/F4SE/Plugins"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PACKAGE_COMMON}"
            "${PACKAGE_INSTALLER}/common"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/README.md"
            "${PACKAGE_INSTALLER}/common/README.md"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "${PACKAGE_INSTALLER}/common/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
            "${PACKAGE_INSTALLER}/common/THIRD_PARTY_NOTICES.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/licenses"
            "${PACKAGE_INSTALLER}/common/licenses"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/generated-fomod/info.xml"
            "${PACKAGE_INSTALLER}/fomod/info.xml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/package/fomod/ModuleConfig.xml"
            "${PACKAGE_INSTALLER}/fomod/ModuleConfig.xml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:MainMenuVideoPlayerOG>"
            "${PACKAGE_INSTALLER}/runtime/OG/F4SE/Plugins/MainMenuVideoPlayer.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:MainMenuVideoPlayerNG>"
            "${PACKAGE_INSTALLER}/runtime/NG/F4SE/Plugins/MainMenuVideoPlayer.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:MainMenuVideoPlayerAE>"
            "${PACKAGE_INSTALLER}/runtime/AE/F4SE/Plugins/MainMenuVideoPlayer.dll"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${PACKAGE_INSTALLER}"
            "${CMAKE_COMMAND}" -E tar cf "${PACKAGE_ARCHIVE}" --format=zip --
            fomod common runtime
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    BYPRODUCTS "${PACKAGE_ARCHIVE}"
    DEPENDS
        MainMenuVideoPlayerOG
        MainMenuVideoPlayerNG
        MainMenuVideoPlayerAE
    COMMENT "Packaging the OG, NG, and AE DLLs in one FOMOD"
    VERBATIM
)
