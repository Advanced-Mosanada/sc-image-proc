# scip_io: frame sources and sinks.
#   always      : image files, test patterns
#   GStreamer   : video files, cameras, display, H.264 recording / UDP streaming
add_library(scip_io STATIC
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

scip_set_warnings(scip_io)
