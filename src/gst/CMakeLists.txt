# scip_gst:        thin C++ helpers over the GStreamer C API (RAII, errors, formats).
#                  Internal; used by scip_io and scip_gststages. GStreamer builds only.
# scip_gststages:  GStreamer video filters as pipeline functions
#                  (scip::gst::registerStages). Empty stand-in without GStreamer.
if(SCIP_WITH_GSTREAMER)
    add_library(scip_gst STATIC
        gst_util.cpp)
    add_library(scip::gst ALIAS scip_gst)
    target_include_directories(scip_gst PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(scip_gst PUBLIC scip_core PkgConfig::GST)
    scip_set_warnings(scip_gst)

    add_library(scip_gststages STATIC gst_stages.cpp)
    target_link_libraries(scip_gststages PUBLIC scip_core PRIVATE scip_gst)
else()
    add_library(scip_gststages STATIC gst_stages_none.cpp)
    target_link_libraries(scip_gststages PUBLIC scip_core)
endif()
add_library(scip::gststages ALIAS scip_gststages)
scip_set_warnings(scip_gststages)
