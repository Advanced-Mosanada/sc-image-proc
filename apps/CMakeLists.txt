add_executable(scip-cli scip-cli/main.cpp)
target_link_libraries(scip-cli PRIVATE scip_stages scip_gststages scip_io scip_buildinfo)
scip_set_warnings(scip-cli)

install(TARGETS scip-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
