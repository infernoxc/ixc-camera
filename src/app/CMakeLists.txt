add_executable(ixc_camera WIN32
    main.cpp
    app.rc
    app.manifest)
set_target_properties(ixc_camera PROPERTIES OUTPUT_NAME "IXCCamera")
target_link_libraries(ixc_camera PRIVATE ixc_core shell32 ole32)
