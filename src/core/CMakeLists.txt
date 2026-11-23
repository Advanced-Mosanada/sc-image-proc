# scip_core: frames, stage interface, pipeline, statistics.
# Depends on nothing but the C++ standard library, on purpose: it is the part
# every backend (CPU, NEON, OpenCL, G2D, NPU) builds on.
add_library(scip_core STATIC
    frame.cpp
    params.cpp
    pipeline.cpp
    stats.cpp)
add_library(scip::core ALIAS scip_core)

target_include_directories(scip_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(scip_core PUBLIC cxx_std_17)
scip_set_warnings(scip_core)
