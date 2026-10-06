add_executable(ixc_camera_app WIN32
    main.cpp
    preview_window.cpp
    settings_panel.cpp
    widgets.cpp
    perf_monitor.cpp
    app.rc
    app.manifest)
set_target_properties(ixc_camera_app PROPERTIES OUTPUT_NAME "IXCCamera")
ixc_delay_load_d3d(ixc_camera_app)
target_link_libraries(ixc_camera_app PRIVATE ixc_camera ixc_processing_gpu ixc_segmentation_gpu ixc_face ixc_effects ixc_vcam_manager shell32 ole32 comctl32 comdlg32 user32 gdi32 dwmapi uxtheme)
target_include_directories(ixc_camera_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # app.rc finds ixc.ico
