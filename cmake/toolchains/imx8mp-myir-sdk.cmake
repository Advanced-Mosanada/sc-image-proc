# Cross-compilation toolchain for the MYiR MYD-JX8MP (i.MX 8M Plus, 4x Cortex-A53).
#
# Uses the Yocto SDK generated for the board's image (fsl-imx-xwayland 5.10-gatesgarth).
# Unlike the SDK's own OEToolchainConfig.cmake, this file does NOT need the
# `environment-setup-*` script to be sourced, so it works from CMake presets and IDEs
# with the host's (newer) CMake.
#
# SDK location, first match wins:
#   -DSCIP_SDK_DIR=...  |  env SCIP_SDK_DIR  |  /mnt/ambat/work/sc_display/sdk
#
# IMPORTANT: the board image and this SDK must come from the same Yocto build,
# otherwise binaries link against library versions the board does not have.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT SCIP_SDK_DIR)
    if(DEFINED ENV{SCIP_SDK_DIR})
        set(SCIP_SDK_DIR "$ENV{SCIP_SDK_DIR}")
    else()
        set(SCIP_SDK_DIR "/mnt/ambat/work/sc_display/sdk")
    endif()
endif()
set(SCIP_SDK_DIR "${SCIP_SDK_DIR}" CACHE PATH "Root of the installed Yocto SDK")
# The toolchain file is re-read for try_compile() projects; pass the SDK path along.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SCIP_SDK_DIR)

set(_scip_native_sysroot "${SCIP_SDK_DIR}/sysroots/x86_64-pokysdk-linux")
set(_scip_target_sysroot "${SCIP_SDK_DIR}/sysroots/cortexa53-crypto-poky-linux")
set(_scip_bin "${_scip_native_sysroot}/usr/bin/aarch64-poky-linux")

if(NOT EXISTS "${_scip_bin}/aarch64-poky-linux-g++")
    message(FATAL_ERROR "Yocto SDK not found at '${SCIP_SDK_DIR}'. "
                        "Set SCIP_SDK_DIR to the folder containing 'sysroots/'.")
endif()

# Compilers and binutils
set(CMAKE_C_COMPILER "${_scip_bin}/aarch64-poky-linux-gcc")
set(CMAKE_CXX_COMPILER "${_scip_bin}/aarch64-poky-linux-g++")
set(CMAKE_AR "${_scip_bin}/aarch64-poky-linux-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_scip_bin}/aarch64-poky-linux-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP "${_scip_bin}/aarch64-poky-linux-strip" CACHE FILEPATH "")

# CPU tuning: same as the SDK's environment-setup (TUNE = cortexa53-crypto)
set(_scip_cpu_flags "-mcpu=cortex-a53 -march=armv8-a+crc+crypto")
set(CMAKE_C_FLAGS_INIT "${_scip_cpu_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_scip_cpu_flags}")

# Headers and libraries come only from the board's sysroot; programs from the host.
set(CMAKE_SYSROOT "${_scip_target_sysroot}")
set(CMAKE_FIND_ROOT_PATH "${_scip_target_sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config (GStreamer and friends) must also look only inside the sysroot.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_scip_target_sysroot}")
set(ENV{PKG_CONFIG_LIBDIR} "${_scip_target_sysroot}/usr/lib/pkgconfig:${_scip_target_sysroot}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

# Run cross-compiled tests on the PC through user-mode QEMU (ctest uses this).
# The real performance numbers still have to come from the board.
find_program(SCIP_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(SCIP_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${SCIP_QEMU_AARCH64};-L;${_scip_target_sysroot}")
endif()
