# scip_stages: the built-in processing stages and the text factory.
add_library(scip_stages STATIC
    factory.cpp
    gaussian_blur.cpp
    resize.cpp
    to_gray.cpp)
add_library(scip::stages ALIAS scip_stages)

target_link_libraries(scip_stages
    PUBLIC scip_core
    PRIVATE scip_cvinterop opencv_imgproc)
scip_set_warnings(scip_stages)
