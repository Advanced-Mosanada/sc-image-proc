# scip_engine: input -> functions -> outputs on a worker thread, reconfigurable
# while it runs. The control interface (AIPC) drives it.
add_library(scip_engine STATIC engine.cpp)
add_library(scip::engine ALIAS scip_engine)

find_package(Threads REQUIRED)
target_link_libraries(scip_engine PUBLIC scip_core scip_io Threads::Threads)
scip_set_warnings(scip_engine)
