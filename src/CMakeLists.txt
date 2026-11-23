# Libraries, from the bottom up:
#
#   scip_core       Frame, Stage, Pipeline, stats       (no dependencies)
#   scip_cvinterop  Frame <-> cv::Mat views              (header-only, OpenCV core)
#   scip_stages     processing stages                   (core + OpenCV imgproc)
#   scip_gst        GStreamer helpers (internal)         (GStreamer; only if enabled)
#   scip_gststages  GStreamer filters as functions      (scip_gst; empty without it)
#   scip_io         sources and sinks                   (core + OpenCV + scip_gst)
#   scip_engine     the running pipeline, reconfigurable (core + io + threads)
#   scip_aipc       network control interface            (engine + nlohmann/json)
#
# Hardware backends (NEON, OpenCL, G2D, NPU) will be added as further libraries
# next to these, each behind its own CMake option.

add_subdirectory(core)

add_library(scip_cvinterop INTERFACE)
add_library(scip::cvinterop ALIAS scip_cvinterop)
target_link_libraries(scip_cvinterop INTERFACE scip_core opencv_core)

add_subdirectory(stages)
add_subdirectory(gst)
add_subdirectory(io)
add_subdirectory(engine)
add_subdirectory(aipc)
