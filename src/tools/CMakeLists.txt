# ixc_probe: headless camera diagnostics used by integration tests and benchmarks.
add_executable(ixc_probe ixc_probe.cpp)
target_link_libraries(ixc_probe PRIVATE ixc_camera ixc_processing ixc_processing_gpu ixc_face ixc_effects strmiids oleaut32)

# ixc_vcam: register / remove / query the IXC Camera system camera.
add_executable(ixc_vcam ixc_vcam.cpp)
target_link_libraries(ixc_vcam PRIVATE ixc_vcam_manager)
