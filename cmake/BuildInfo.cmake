# scip_buildinfo: what a binary is (version, git commit and branch, target
# CPU, build type, compiler), for `--version` and the control interface's
# system.info. Several builds float around between the PC and the board, so
# this must be exact.
#
# The git part is refreshed on EVERY build, not only when CMake configures:
# WriteBuildInfo.cmake runs each time and rewrites build_info.cpp only when
# something changed, so a commit costs one small recompile and a relink.
find_package(Git QUIET)

set(_scip_build_info_cpp "${PROJECT_BINARY_DIR}/generated/buildinfo/build_info.cpp")
add_custom_target(scip_build_info_refresh
    COMMAND "${CMAKE_COMMAND}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DINPUT=${CMAKE_CURRENT_LIST_DIR}/build_info.cpp.in"
        "-DOUTPUT=${_scip_build_info_cpp}"
        "-DSCIP_VERSION=${PROJECT_VERSION}"
        "-DSCIP_TARGET_CPU=${CMAKE_SYSTEM_PROCESSOR}"
        "-DSCIP_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DSCIP_COMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        -P "${CMAKE_CURRENT_LIST_DIR}/WriteBuildInfo.cmake"
    BYPRODUCTS "${_scip_build_info_cpp}"
    COMMENT "Checking git commit for build info"
    VERBATIM)

add_library(scip_buildinfo STATIC "${_scip_build_info_cpp}")
add_library(scip::buildinfo ALIAS scip_buildinfo)
add_dependencies(scip_buildinfo scip_build_info_refresh)
target_include_directories(scip_buildinfo PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(scip_buildinfo PUBLIC cxx_std_17)
