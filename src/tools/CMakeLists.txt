# ixc_probe: headless camera diagnostics used by integration tests and benchmarks.
add_executable(ixc_probe ixc_probe.cpp)
target_link_libraries(ixc_probe PRIVATE ixc_camera)
