# scip-cli: run one pipeline from the command line and report timings.
add_executable(scip-cli scip-cli/main.cpp)
target_link_libraries(scip-cli PRIVATE scip_stages scip_gststages scip_io scip_buildinfo)
scip_set_warnings(scip-cli)

# scipd: the target daemon, controlled over the network with AIPC.
add_executable(scipd scipd/main.cpp)
target_link_libraries(scipd PRIVATE scip_stages scip_gststages scip_aipc scip_buildinfo)
scip_set_warnings(scipd)

install(TARGETS scip-cli scipd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
