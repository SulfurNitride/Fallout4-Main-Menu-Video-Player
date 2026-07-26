set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

# vcpkg sanitizes the environment used by ports. These point to tools supplied
# by the Windows CI runner and must be visible to the FFmpeg overlay and the
# pinned pkg-config fixup helper.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED
    MMVP_MSYS2_ROOT
    PKG_CONFIG
)
