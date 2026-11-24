# Cross-compilation toolchain for the Radxa ROCK 5B+ (Rockchip RK3588:
# 4x Cortex-A76 + 4x Cortex-A55), running Radxa's Debian 12 (bookworm) image.
#
# Compiler: Ubuntu's GCC 12 cross compiler, the same major version as the board's
# own GCC 12.2 (so the board's libstdc++ has every symbol we use):
#   sudo apt install g++-12-aarch64-linux-gnu
#
# Sysroot: copied from the board by scripts/rk3588-sysroot.sh. First match wins:
#   -DSCIP_SYSROOT_DIR=...  |  env SCIP_SYSROOT_DIR  |  ~/sysroots/rock5bplus-debian12
#
# Re-create the sysroot after upgrading packages on the board, otherwise binaries
# may link against library versions the board does not have.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

if(NOT SCIP_SYSROOT_DIR)
    if(DEFINED ENV{SCIP_SYSROOT_DIR})
        set(SCIP_SYSROOT_DIR "$ENV{SCIP_SYSROOT_DIR}")
    else()
        set(SCIP_SYSROOT_DIR "$ENV{HOME}/sysroots/rock5bplus-debian12")
    endif()
endif()
set(SCIP_SYSROOT_DIR "${SCIP_SYSROOT_DIR}" CACHE PATH "Sysroot copied from the RK3588 board")
# The toolchain file is re-read for try_compile() projects; pass the sysroot along.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SCIP_SYSROOT_DIR)

if(NOT EXISTS "${SCIP_SYSROOT_DIR}/usr/include/aarch64-linux-gnu")
    message(FATAL_ERROR "RK3588 sysroot not found at '${SCIP_SYSROOT_DIR}'. "
                        "Create it with scripts/rk3588-sysroot.sh or set SCIP_SYSROOT_DIR.")
endif()

find_program(_scip_cxx NAMES aarch64-linux-gnu-g++-12 REQUIRED)
find_program(_scip_cc NAMES aarch64-linux-gnu-gcc-12 REQUIRED)
set(CMAKE_C_COMPILER "${_scip_cc}")
set(CMAKE_CXX_COMPILER "${_scip_cxx}")

# CPU tuning: big.LITTLE (code tuned for the A76, runs on both), with the
# features both cores have (dotprod, fp16, crypto).
set(_scip_cpu_flags "-mcpu=cortex-a76.cortex-a55+crypto")

# Ubuntu's cross GCC searches its own /usr/aarch64-linux-gnu (glibc 2.39,
# libstdc++ 12.4) BEFORE the sysroot, which gives binaries that need GLIBC_2.38
# while the board has 2.36. Put the board's headers, start files and libraries
# first, so we build exactly against what the board runs (glibc 2.36, GCC 12.2's
# libstdc++). Only the compiler's own builtin headers (arm_neon.h, ...) stay.
set(_scip_gcc_ver 12)
set(_scip_s "${SCIP_SYSROOT_DIR}")
execute_process(COMMAND "${_scip_cc}" -print-file-name=include
                OUTPUT_VARIABLE _scip_gcc_include OUTPUT_STRIP_TRAILING_WHITESPACE)
set(_scip_c_includes
    "-nostdinc"
    "-isystem ${_scip_gcc_include}"
    "-isystem ${_scip_s}/usr/include/aarch64-linux-gnu"
    "-isystem ${_scip_s}/usr/include")
set(_scip_cxx_includes
    "-nostdinc"
    "-isystem ${_scip_s}/usr/include/c++/${_scip_gcc_ver}"
    "-isystem ${_scip_s}/usr/include/aarch64-linux-gnu/c++/${_scip_gcc_ver}"
    "-isystem ${_scip_s}/usr/include/c++/${_scip_gcc_ver}/backward"
    "-isystem ${_scip_gcc_include}"
    "-isystem ${_scip_s}/usr/include/aarch64-linux-gnu"
    "-isystem ${_scip_s}/usr/include")
list(JOIN _scip_c_includes " " _scip_c_includes)
list(JOIN _scip_cxx_includes " " _scip_cxx_includes)
set(CMAKE_C_FLAGS_INIT "${_scip_cpu_flags} ${_scip_c_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_scip_cpu_flags} ${_scip_cxx_includes}")

# -B: crt1.o / crti.o from the board's glibc; -L: its libc, libstdc++, libgcc first.
set(_scip_link_flags
    "-B${_scip_s}/usr/lib/aarch64-linux-gnu/"
    "-L${_scip_s}/usr/lib/gcc/aarch64-linux-gnu/${_scip_gcc_ver}"
    "-L${_scip_s}/usr/lib/aarch64-linux-gnu"
    "-L${_scip_s}/usr/lib")
list(JOIN _scip_link_flags " " _scip_link_flags)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_scip_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_scip_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_scip_link_flags}")

# Headers and libraries come only from the board's sysroot; programs from the host.
set(CMAKE_SYSROOT "${SCIP_SYSROOT_DIR}")
set(CMAKE_FIND_ROOT_PATH "${SCIP_SYSROOT_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config (GStreamer and friends) must also look only inside the sysroot.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SCIP_SYSROOT_DIR}")
set(ENV{PKG_CONFIG_LIBDIR} "${SCIP_SYSROOT_DIR}/usr/lib/aarch64-linux-gnu/pkgconfig:${SCIP_SYSROOT_DIR}/usr/share/pkgconfig:${SCIP_SYSROOT_DIR}/usr/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")

# Run cross-compiled tests on the PC through user-mode QEMU (ctest uses this).
# The real performance numbers still have to come from the board.
find_program(SCIP_QEMU_AARCH64 NAMES qemu-aarch64-static qemu-aarch64)
if(SCIP_QEMU_AARCH64)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${SCIP_QEMU_AARCH64};-L;${SCIP_SYSROOT_DIR}")
endif()
