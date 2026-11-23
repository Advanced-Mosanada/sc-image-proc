# scip_io: frame sources (files, test patterns; camera next) and image writing.
add_library(scip_io STATIC
    image_file.cpp
    sources.cpp)
add_library(scip::io ALIAS scip_io)

target_link_libraries(scip_io
    PUBLIC scip_core
    PRIVATE scip_cvinterop opencv_imgcodecs opencv_imgproc)
scip_set_warnings(scip_io)
