add_executable(ixc_camera_app WIN32
    main.cpp
    preview_window.cpp
    adjustments_panel.cpp
    app.rc
    app.manifest)
set_target_properties(ixc_camera_app PROPERTIES OUTPUT_NAME "IXCCamera")
ixc_delay_load_d3d(ixc_camera_app)
target_link_libraries(ixc_camera_app PRIVATE ixc_camera ixc_processing_gpu ixc_face ixc_effects ixc_vcam_manager shell32 ole32 comctl32 comdlg32 user32 gdi32)
