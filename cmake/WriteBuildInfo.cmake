# Run at build time by the scip_build_info_refresh target (cmake -P).
# Inputs: GIT_EXECUTABLE SOURCE_DIR INPUT OUTPUT SCIP_VERSION SCIP_TARGET_CPU
#         SCIP_BUILD_TYPE SCIP_COMPILER

function(scip_git out)
    set(${out} "" PARENT_SCOPE)
    if(NOT GIT_EXECUTABLE)
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        OUTPUT_VARIABLE value
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE result)
    if(result EQUAL 0)
        set(${out} "${value}" PARENT_SCOPE)
    endif()
endfunction()

scip_git(SCIP_GIT_COMMIT rev-parse --short=10 HEAD)
scip_git(SCIP_GIT_DESCRIBE describe --always --dirty --abbrev=10)
scip_git(SCIP_GIT_BRANCH rev-parse --abbrev-ref HEAD)
scip_git(SCIP_GIT_DATE log -1 --format=%cI)
scip_git(_status status --porcelain --untracked-files=no)

if(SCIP_GIT_COMMIT STREQUAL "")
    set(SCIP_GIT_COMMIT "unknown")
    set(SCIP_GIT_DESCRIBE "unknown")
endif()
if(SCIP_GIT_BRANCH STREQUAL "HEAD")
    set(SCIP_GIT_BRANCH "")  # detached
endif()
if(_status STREQUAL "")
    set(SCIP_GIT_DIRTY false)
else()
    set(SCIP_GIT_DIRTY true)
endif()

# configure_file only touches OUTPUT when the text changes, so an unchanged
# commit does not trigger a rebuild.
configure_file("${INPUT}" "${OUTPUT}" @ONLY)
