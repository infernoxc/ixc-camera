# ixc_core: platform-level building blocks shared by the UI app and the virtual-camera source.
add_library(ixc_core STATIC
    common/strings.cpp
    common/json.cpp
    common/fileio.cpp
    diagnostics/error.cpp
    diagnostics/log.cpp
    profiles/profile.cpp
    profiles/profile_store.cpp
    profiles/active_profile.cpp
    profiles/app_settings.cpp)
target_include_directories(ixc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ixc_core PUBLIC ixc_settings)

# ixc_camera: physical camera enumeration, format negotiation and capture.
# format_select / frame_stats / reconnect_policy are pure logic and unit-tested without hardware.
add_library(ixc_camera STATIC
    camera/format_select.cpp
    camera/frame_stats.cpp
    camera/reconnect_policy.cpp
    camera/exposure_governor.cpp
    camera/smooth_motion.cpp
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

# ixc_face: face detection (libfacedetection), tracking and adaptive cadence.
# The tracker/cadence logic always builds (unit-tested); the detector is compiled in only with
# IXC_WITH_FACE_TRACKING.
add_library(ixc_face STATIC
    face/face_types.cpp
    face/downscale.cpp
    face/tracker.cpp
    face/cadence.cpp
    face/detector.cpp
    face/face_engine.cpp
    face/auto_framer.cpp)
target_link_libraries(ixc_face PUBLIC ixc_processing)
if(IXC_WITH_FACE_TRACKING)
    # The vendored library is compiled twice (portable + AVX2) into separate namespaces. It is
    # third-party code: built without IXC's warning flags, but with the same CRT, optimization
    # and Control Flow Guard settings. No /arch:AVX2 (see third_party/libfacedetection/README.IXC.md).
    set(IXC_LFD_DIR ${CMAKE_SOURCE_DIR}/third_party/libfacedetection)
    # Integrity: the vendored files must be exactly the pinned upstream ones (README.IXC.md).
    foreach(pair
            "src/facedetectcnn.h=896ad9cae317d1ebce947662ea44d7d1c3f12cd77606da977c0adea9c5625605"
            "src/facedetectcnn.cpp=e86cbb3f4080762197d0a43b98f3534e08fe890a7eed5c5904e395b8bed4a55d"
            "src/facedetectcnn-model.cpp=0c5689b7070c070429a2c45eb3ae7f02018e492db94b0a3eb660681e9ca0aee6"
            "src/facedetectcnn-data.cpp=cf019963f413528540d6de741e72eea4f2f0b572ca85b22dd4baa8fc71b42bb6")
        string(REPLACE "=" ";" kv "${pair}")
        list(GET kv 0 rel)
        list(GET kv 1 expected)
        file(SHA256 ${IXC_LFD_DIR}/${rel} actual)
        if(NOT actual STREQUAL expected)
            message(FATAL_ERROR "third_party/libfacedetection/${rel} differs from the pinned upstream file "
                                "(see third_party/libfacedetection/README.IXC.md).")
        endif()
    endforeach()
    add_library(ixc_lfd OBJECT face/lfd_scalar.cpp face/lfd_avx2.cpp)
    target_include_directories(ixc_lfd PRIVATE ${IXC_LFD_DIR}/src ${IXC_LFD_DIR}/ixc)
    target_compile_definitions(ixc_lfd PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ixc_lfd PRIVATE /W0 /utf-8 /EHsc /permissive- /Zi
        "$<$<NOT:$<CONFIG:Debug>>:/O2;/Gy;/Gw>"
        "$<IF:$<BOOL:${IXC_ENABLE_ASAN}>,/fsanitize=address,/guard:cf>")
    target_sources(ixc_face PRIVATE $<TARGET_OBJECTS:ixc_lfd>)
    target_compile_definitions(ixc_face PUBLIC IXC_FACE_TRACKING=1)
else()
    target_compile_definitions(ixc_face PUBLIC IXC_FACE_TRACKING=0)
endif()

# ixc_segmentation: person segmentation for the background effects. MediaPipe's selfie segmenter
# (Apache-2.0, third_party/mediapipe_selfie_segmenter) is compiled in as data and run by IXC's own
# kernels: no model file, no runtime library, nothing loaded from disk.
file(SHA256 ${CMAKE_SOURCE_DIR}/third_party/mediapipe_selfie_segmenter/selfie_segmenter_landscape.inc IXC_SEG_MODEL_SHA)
if(NOT IXC_SEG_MODEL_SHA STREQUAL "beec5241486c2ad93a96ddd79a6aec6ccadce64d85657c2d48254a579d55b4b4")
    message(FATAL_ERROR "third_party/mediapipe_selfie_segmenter/selfie_segmenter_landscape.inc differs from the pinned file "
                        "(see third_party/mediapipe_selfie_segmenter/README.IXC.md).")
endif()
add_library(ixc_segmentation STATIC
    segmentation/selfie_net.cpp
    segmentation/mask_refine.cpp
    segmentation/segmentation_engine.cpp)
target_link_libraries(ixc_segmentation PUBLIC ixc_processing)

# ixc_effects: built-in IXC effects applied to the processed frame (Phase 8).
add_library(ixc_effects STATIC
    effects/effects.cpp
    effects/background_image.cpp
    effects/backgrounds.cpp
    effects/background_renderer.cpp)
target_link_libraries(ixc_effects PUBLIC ixc_face ixc_segmentation)

add_subdirectory(virtual_camera)
add_subdirectory(app)
add_subdirectory(tools)
add_subdirectory(installer)
