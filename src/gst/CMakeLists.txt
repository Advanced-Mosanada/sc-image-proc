# scip_gst: thin C++ helpers over the GStreamer C API (RAII, errors, formats).
# Internal: used by scip_io and the GStreamer-backed stages.
add_library(scip_gst STATIC
    gst_util.cpp)
add_library(scip::gst ALIAS scip_gst)

target_include_directories(scip_gst PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(scip_gst PUBLIC scip_core PkgConfig::GST)
scip_set_warnings(scip_gst)
