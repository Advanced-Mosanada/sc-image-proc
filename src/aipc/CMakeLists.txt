# scip_aipc: the AIPC control interface (docs/aipc-protocol.md).
#
# JSON comes from nlohmann/json, header-only. A system copy is used when there
# is one (e.g. a newer board image); otherwise a pinned release is downloaded,
# like GoogleTest, so PC and board builds use the same version.
find_package(nlohmann_json 3.10 QUIET)
if(nlohmann_json_FOUND)
    # Visible to the tests too, like the FetchContent target would be.
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES IMPORTED_GLOBAL TRUE)
else()
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(scip_aipc STATIC
    convert.cpp
    server.cpp
    service.cpp
    system_info.cpp)
add_library(scip::aipc ALIAS scip_aipc)

target_link_libraries(scip_aipc
    PUBLIC scip_engine
    PRIVATE scip_buildinfo nlohmann_json::nlohmann_json opencv_core)
scip_set_warnings(scip_aipc)
