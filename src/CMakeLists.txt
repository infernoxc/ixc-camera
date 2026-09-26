# ixc_core: platform-level building blocks shared by the UI app and the virtual-camera source.
add_library(ixc_core STATIC
    common/strings.cpp
    common/json.cpp
    common/fileio.cpp
    diagnostics/error.cpp
    diagnostics/log.cpp
    profiles/profile.cpp
    profiles/profile_store.cpp
    profiles/active_profile.cpp)
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
    processing/color.cpp
    processing/image_pipeline.cpp
    processing/backend_selector.cpp)
target_link_libraries(ixc_processing PUBLIC ixc_core)

# ixc_processing_gpu: Direct3D 11 compute version of the same pipeline (byte-identical output).
# Kept separate so nothing links Direct3D unless the benchmark justifies it.
find_program(IXC_FXC fxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkBinPath}/x64" REQUIRED)
set(IXC_SHADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)
set(IXC_SHADER_HEADERS)
foreach(entry CSColorY CSColorUV CSScaleY CSScaleUV CSSharpen)
    set(out ${IXC_SHADER_DIR}/${entry}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${IXC_SHADER_DIR}
        COMMAND ${IXC_FXC} /nologo /T cs_5_0 /E ${entry} /O3 /WX /Fh ${out} /Vn g_${entry} ${CMAKE_CURRENT_SOURCE_DIR}/processing/gpu/pipeline.hlsl
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/processing/gpu/pipeline.hlsl
        COMMENT "Compiling shader ${entry}"
        VERBATIM)
    list(APPEND IXC_SHADER_HEADERS ${out})
endforeach()
add_library(ixc_processing_gpu STATIC
    processing/gpu/gpu_pipeline.cpp
    processing/gpu/adaptive_processor.cpp
    ${IXC_SHADER_HEADERS})
target_include_directories(ixc_processing_gpu PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ixc_processing_gpu PUBLIC ixc_processing d3d11 dxgi)

# Binaries that link the GPU path delay-load Direct3D: it's only loaded if a GPU attempt is made.
function(ixc_delay_load_d3d target)
    target_link_options(${target} PRIVATE /DELAYLOAD:d3d11.dll /DELAYLOAD:dxgi.dll)
    target_link_libraries(${target} PRIVATE delayimp)
endfunction()

add_subdirectory(virtual_camera)
add_subdirectory(app)
add_subdirectory(tools)
