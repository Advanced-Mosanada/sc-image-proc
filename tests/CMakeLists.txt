# GoogleTest is built from source for every preset (host and board), so the
# board does not need a libgtest package and both sides use the same version.
include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(scip_tests
    test_aipc.cpp
    test_engine.cpp
    test_frame.cpp
    test_gst.cpp
    test_gst_stages.cpp
    test_io.cpp
    test_params.cpp
    test_pipeline.cpp
    test_registry.cpp
    test_stages.cpp
    test_stats.cpp)
target_link_libraries(scip_tests PRIVATE
    scip_stages scip_gststages scip_aipc scip_engine scip_io scip_cvinterop opencv_imgproc
    nlohmann_json::nlohmann_json GTest::gtest_main)
scip_set_warnings(scip_tests)

# PRE_TEST: list the tests when ctest runs, not at build time, so cross builds
# do not need to execute target binaries while compiling.
include(GoogleTest)
gtest_discover_tests(scip_tests DISCOVERY_MODE PRE_TEST)

# End-to-end smoke test of the command-line tool.
add_test(NAME cli.pattern_gray_blur
         COMMAND scip-cli -i pattern:320x240 -p "gray ! blur ksize=5 ! resize width=160 height=120" -n 5 -w 1)
add_test(NAME cli.rejects_unknown_stage COMMAND scip-cli -p does-not-exist)
set_tests_properties(cli.rejects_unknown_stage PROPERTIES WILL_FAIL TRUE)

# Installed next to the app so the same tests can be run on the board.
install(TARGETS scip_tests RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/scip)
