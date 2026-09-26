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

# ixc_camera: physical camera enumeration, format negotiation and capture.
# format_select / frame_stats / reconnect_policy are pure logic and unit-tested without hardware.
add_library(ixc_camera STATIC
    camera/format_select.cpp
    camera/frame_stats.cpp
    camera/reconnect_policy.cpp
    camera/device_enum.cpp
    camera/capture_session.cpp)
target_link_libraries(ixc_camera PUBLIC ixc_core mfplat mf mfreadwrite mfuuid cfgmgr32 ole32)

# ixc_processing: image processing (CPU paths now; GPU pipeline in Phase 6).
add_library(ixc_processing STATIC
    processing/color.cpp)
target_link_libraries(ixc_processing PUBLIC ixc_core)

add_subdirectory(app)
add_subdirectory(tools)
