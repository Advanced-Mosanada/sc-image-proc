# scip_io: frame sources and sinks, opened from URIs (io::openSource / io::openSink).
# Video, camera, display and network I/O need GStreamer; until that backend is
# built in, gst/no_gstreamer.cpp provides stand-ins that report it is missing.
add_library(scip_io STATIC
    image_file.cpp
    sinks.cpp
    sources.cpp
    gst/no_gstreamer.cpp)
add_library(scip::io ALIAS scip_io)

target_link_libraries(scip_io
    PUBLIC scip_core
    PRIVATE scip_cvinterop opencv_imgcodecs opencv_imgproc)
scip_set_warnings(scip_io)
