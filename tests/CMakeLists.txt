add_executable(ixc_unit_tests
    test_main.cpp
    test_json.cpp
    test_profile.cpp
    test_diagnostics.cpp
    test_camera.cpp
    test_processing.cpp
    test_pipeline.cpp
    test_active_profile.cpp
    test_gpu_pipeline.cpp
    test_backend_selector.cpp
    test_exposure_governor.cpp
    test_face.cpp
    test_effects.cpp
    test_segmentation.cpp
    test_auto_framer.cpp
    test_background.cpp
    test_denoise.cpp
    test_flicker.cpp
    test_gpu_segmentation.cpp
    test_update.cpp)
target_link_libraries(ixc_unit_tests PRIVATE ixc_camera ixc_processing ixc_processing_gpu ixc_face ixc_effects ixc_segmentation ixc_segmentation_gpu)

add_test(NAME unit COMMAND ixc_unit_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 120 LABELS "unit")

# Hardware integration tests. They exit 77 (reported as skipped) when no physical camera exists,
# e.g. on CI runners. Run only these with: ctest --preset <p> -L hardware
add_test(NAME camera_enumerate COMMAND ixc_probe --list)
add_test(NAME camera_capture_nv12 COMMAND ixc_probe --capture 8 --output nv12)
add_test(NAME camera_capture_rgb32 COMMAND ixc_probe --capture 5 --output rgb32)
add_test(NAME camera_open_close_cycles COMMAND ixc_probe --cycles 25 --output nv12)
# IXC Camera media source loaded directly in the test process (no install needed): validates
# activation, NV12-only filtering and frame flow before the DLL ever runs inside Frame Server.
add_test(NAME vcam_source_in_process COMMAND ixc_probe --source-test $<TARGET_FILE:ixc_vcam_source> --seconds 5)
# Image pipeline inside the real source, including a live settings change mid-stream.
add_test(NAME vcam_source_effects COMMAND ixc_probe --source-effect-test $<TARGET_FILE:ixc_vcam_source>)
# The installed IXC Camera system camera (skipped when it isn't installed).
add_test(NAME vcam_capture_mf COMMAND ixc_probe --capture 6 --camera "IXC Camera" --output nv12)
add_test(NAME vcam_capture_directshow COMMAND ixc_probe --dshow-capture 4 --camera "IXC Camera")
add_test(NAME vcam_open_close_cycles COMMAND ixc_probe --cycles 15 --camera "IXC Camera" --output nv12)
set_tests_properties(camera_enumerate camera_capture_nv12 camera_capture_rgb32 camera_open_close_cycles
                     vcam_source_in_process vcam_source_effects vcam_capture_mf vcam_capture_directshow vcam_open_close_cycles PROPERTIES
    LABELS "hardware"
    SKIP_RETURN_CODE 77
    TIMEOUT 180
    RUN_SERIAL TRUE)
# Face tracking engine on live frames from the first physical camera (detection success depends
# on someone being in view, so only the engine's operation is checked). Skipped without a camera
# or when built with IXC_WITH_FACE_TRACKING=OFF.
add_test(NAME face_engine_live COMMAND ixc_probe --bench-face 4)
set_tests_properties(face_engine_live PROPERTIES TIMEOUT 120 LABELS "hardware" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
# A Chromium browser (Edge/Chrome/Brave, headless, throwaway profile) opens IXC Camera via getUserMedia.
add_test(NAME browser_getusermedia COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/browser_camera.ps1)
set_tests_properties(browser_getusermedia PROPERTIES TIMEOUT 120 LABELS "hardware" SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
