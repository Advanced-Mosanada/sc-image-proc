# scip_set_warnings(<target>)
#
# Applies the project's warning set to one of OUR targets. Third-party code
# (GoogleTest, OpenCV headers) is not affected: imported include directories
# are treated as system headers.
function(scip_set_warnings target)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Woverloaded-virtual
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmissing-declarations)

    if(SCIP_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
