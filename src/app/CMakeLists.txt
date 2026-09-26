add_executable(ixc_camera_app WIN32
    main.cpp
    preview_window.cpp
    app.rc
    app.manifest)
set_target_properties(ixc_camera_app PROPERTIES OUTPUT_NAME "IXCCamera")
target_link_libraries(ixc_camera_app PRIVATE ixc_camera ixc_processing ixc_vcam_manager shell32 ole32 comctl32 user32 gdi32)
