# scip_stages: the built-in processing functions.
#
# Each .cpp registers its function with SCIP_REGISTER_STAGE. This is an OBJECT
# library on purpose: its object files are linked into every executable that
# lists it, so the registrations are kept. (From a static library the linker
# would drop them, because nothing references them by name.)
#
# To add a function: create a .cpp here (see README.md) and list it below.
add_library(scip_stages OBJECT
    # contrast
    autolevels.cpp
    clahe.cpp
    equalize.cpp
    levels.cpp
    retinex.cpp
    # spatial noise reduction
    bilateral.cpp
    gaussian_blur.cpp
    guided.cpp
    median.cpp
    nlmeans.cpp
    # temporal noise reduction
    temporal_mean.cpp
    temporal_median.cpp
    tnr.cpp
    # sharpening
    detail.cpp
    sharpen.cpp
    unsharp.cpp
    # other
    edges.cpp
    invert.cpp
    resize.cpp
    threshold.cpp
    to_gray.cpp)
add_library(scip::stages ALIAS scip_stages)

target_link_libraries(scip_stages
    PUBLIC scip_core
    PRIVATE scip_cvinterop opencv_imgproc opencv_photo)
scip_set_warnings(scip_stages)
