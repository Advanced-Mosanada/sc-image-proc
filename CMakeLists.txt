cmake_minimum_required(VERSION 3.25)

project(scip
    VERSION 0.1.0
    DESCRIPTION "Image processing pipeline for PC and i.MX 8M Plus"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language and global settings
# ---------------------------------------------------------------------------
# C++17: the board SDK ships GCC 10.2, whose C++20 support is incomplete.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SCIP_BUILD_TESTS "Build the unit tests" ON)
option(SCIP_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(SCIP_ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan (host only)" OFF)
option(SCIP_WITH_GSTREAMER "Video files, camera, display and streaming via GStreamer" ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(BuildInfo)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# The board image has OpenCV 4.4; the PC may have a newer one. Asking for 4.4
# here and building the imx8mp preset regularly keeps us on the common API.
find_package(OpenCV 4.4 REQUIRED COMPONENTS core imgproc imgcodecs)
message(STATUS "OpenCV ${OpenCV_VERSION} from ${OpenCV_DIR}")

# GStreamer 1.18 is what the board image has. On Ubuntu:
#   sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
if(SCIP_WITH_GSTREAMER)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0>=1.18 gstreamer-app-1.0 gstreamer-video-1.0)
    message(STATUS "GStreamer ${GST_gstreamer-1.0_VERSION}")
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)
add_subdirectory(apps)

if(SCIP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "scip ${PROJECT_VERSION}: ${CMAKE_SYSTEM_PROCESSOR}, ${CMAKE_BUILD_TYPE}, "
               "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
