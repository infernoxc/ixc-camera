# ixc_core: platform-level building blocks shared by the UI app and the virtual-camera source.
add_library(ixc_core STATIC
    common/strings.cpp
    common/json.cpp
    common/fileio.cpp
    diagnostics/error.cpp
    diagnostics/log.cpp
    profiles/profile.cpp
    profiles/profile_store.cpp)
target_include_directories(ixc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ixc_core PUBLIC ixc_settings)

add_subdirectory(app)
