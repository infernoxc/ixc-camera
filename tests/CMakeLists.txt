add_executable(ixc_unit_tests
    test_main.cpp
    test_json.cpp
    test_profile.cpp
    test_diagnostics.cpp
    test_camera.cpp
    test_processing.cpp)
target_link_libraries(ixc_unit_tests PRIVATE ixc_camera ixc_processing)

add_test(NAME unit COMMAND ixc_unit_tests)
set_tests_properties(unit PROPERTIES TIMEOUT 120 LABELS "unit")

# Hardware integration tests. They exit 77 (reported as skipped) when no physical camera exists,
# e.g. on CI runners. Run only these with: ctest --preset <p> -L hardware
add_test(NAME camera_enumerate COMMAND ixc_probe --list)
add_test(NAME camera_capture_nv12 COMMAND ixc_probe --capture 8 --output nv12)
add_test(NAME camera_capture_rgb32 COMMAND ixc_probe --capture 5 --output rgb32)
add_test(NAME camera_open_close_cycles COMMAND ixc_probe --cycles 25 --output nv12)
set_tests_properties(camera_enumerate camera_capture_nv12 camera_capture_rgb32 camera_open_close_cycles PROPERTIES
    LABELS "hardware"
    SKIP_RETURN_CODE 77
    TIMEOUT 180
    RUN_SERIAL TRUE)
