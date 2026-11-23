# scip_stages: the built-in processing functions.
#
# Each .cpp registers its function with SCIP_REGISTER_STAGE. This is an OBJECT
# library on purpose: its object files are linked into every executable that
# lists it, so the registrations are kept. (From a static library the linker
# would drop them, because nothing references them by name.)
#
# To add a function: create a .cpp here (see README.md) and list it below.
add_library(scip_stages OBJECT
    clahe.cpp
    edges.cpp
    gaussian_blur.cpp
    invert.cpp
    resize.cpp
    threshold.cpp
    to_gray.cpp)
add_library(scip::stages ALIAS scip_stages)

target_link_libraries(scip_stages
    PUBLIC scip_core
    PRIVATE scip_cvinterop opencv_imgproc)
scip_set_warnings(scip_stages)
