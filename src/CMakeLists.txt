# Libraries, from the bottom up:
#
#   scip_core       Frame, Stage, Pipeline, stats       (no dependencies)
#   scip_cvinterop  Frame <-> cv::Mat views              (header-only, OpenCV core)
#   scip_stages     processing stages                   (core + OpenCV imgproc)
#   scip_io         sources / image files               (core + OpenCV imgcodecs)
#
# Hardware backends (NEON, OpenCL, G2D, NPU) will be added as further libraries
# next to these, each behind its own CMake option.

add_subdirectory(core)

add_library(scip_cvinterop INTERFACE)
add_library(scip::cvinterop ALIAS scip_cvinterop)
target_link_libraries(scip_cvinterop INTERFACE scip_core opencv_core)

add_subdirectory(stages)
add_subdirectory(io)
