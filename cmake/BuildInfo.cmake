# Generates scip/build_info.hpp so every binary can report exactly what it is
# (version, git commit, target CPU, build type, compiler). Handy when several
# builds float around between the PC and the board.
find_package(Git QUIET)
set(SCIP_GIT_HASH "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=10
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        OUTPUT_VARIABLE _scip_git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _scip_git_result)
    if(_scip_git_result EQUAL 0 AND _scip_git_hash)
        set(SCIP_GIT_HASH "${_scip_git_hash}")
    endif()
endif()

set(SCIP_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated")
configure_file("${CMAKE_CURRENT_LIST_DIR}/build_info.hpp.in"
               "${SCIP_GENERATED_INCLUDE_DIR}/scip/build_info.hpp" @ONLY)
