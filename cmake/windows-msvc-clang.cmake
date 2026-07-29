set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_MT llvm-mt)
set(CMAKE_RC_COMPILER llvm-rc)

set(MSVC_ROOT "/usr/share/msvc")

set(MSVC_INCLUDE_FLAGS
    "/imsvc${MSVC_ROOT}/crt/include"
    "/imsvc${MSVC_ROOT}/sdk/include/ucrt"
    "/imsvc${MSVC_ROOT}/sdk/include/um"
    "/imsvc${MSVC_ROOT}/sdk/include/shared"
    "/imsvc${MSVC_ROOT}/sdk/include/winrt"
)
string(JOIN " " MSVC_INCLUDE_FLAGS_STRING ${MSVC_INCLUDE_FLAGS})

set(CMAKE_C_FLAGS_INIT
    "--target=x86_64-pc-windows-msvc ${MSVC_INCLUDE_FLAGS_STRING}")
set(CMAKE_CXX_FLAGS_INIT
    "--target=x86_64-pc-windows-msvc ${MSVC_INCLUDE_FLAGS_STRING}")

set(MSVC_LINK_FLAGS
    "/lldignoreenv"
    "/libpath:${MSVC_ROOT}/sdk/lib/um/x86_64"
    "/libpath:${MSVC_ROOT}/sdk/lib/ucrt/x86_64"
    "/libpath:${MSVC_ROOT}/crt/lib/x86_64"
)
string(JOIN " " MSVC_LINK_FLAGS_STRING ${MSVC_LINK_FLAGS})

set(CMAKE_EXE_LINKER_FLAGS_INIT "${MSVC_LINK_FLAGS_STRING}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${MSVC_LINK_FLAGS_STRING}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${MSVC_LINK_FLAGS_STRING}")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
