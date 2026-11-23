add_executable(scip-cli scip-cli/main.cpp)
target_link_libraries(scip-cli PRIVATE scip_stages scip_io)
target_include_directories(scip-cli PRIVATE "${SCIP_GENERATED_INCLUDE_DIR}")
scip_set_warnings(scip-cli)

install(TARGETS scip-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
