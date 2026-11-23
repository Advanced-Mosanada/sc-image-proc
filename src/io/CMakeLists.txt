# scip_io: frame sources and sinks.
#   always      : image files, test patterns
#   GStreamer   : video files, cameras, display, H.264 recording / UDP streaming
add_library(scip_io STATIC
    devices.cpp
    image_file.cpp
    sinks.cpp
    sources.cpp)
add_library(scip::io ALIAS scip_io)

target_link_libraries(scip_io
    PUBLIC scip_core
    PRIVATE scip_cvinterop opencv_imgcodecs opencv_imgproc)

if(SCIP_WITH_GSTREAMER)
    target_sources(scip_io PRIVATE
        gst/gst_sink.cpp
        gst/gst_source.cpp)
    target_link_libraries(scip_io PRIVATE scip_gst)
else()
    target_sources(scip_io PRIVATE gst/no_gstreamer.cpp)
endif()

# Display connector listing (optional): libdrm.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(DRM QUIET IMPORTED_TARGET libdrm)
endif()
if(DRM_FOUND)
    target_link_libraries(scip_io PRIVATE PkgConfig::DRM)
    target_compile_definitions(scip_io PRIVATE SCIP_HAVE_DRM=1)
else()
    message(STATUS "libdrm not found: display connectors will not be listed")
endif()

scip_set_warnings(scip_io)
