# IXCCameraSource.dll: the media source Windows Frame Server loads for "IXC Camera".
# Deliberately small: no UI code, no file logging (TraceLogging only), static CRT.
add_library(ixc_vcam_source SHARED
    source/dllmain.cpp
    source/activate.cpp
    source/media_source.cpp
    source/media_stream.cpp
    source/frame_processor.cpp
    source/exports.def
    source/source.rc)
set_target_properties(ixc_vcam_source PROPERTIES OUTPUT_NAME "IXCCameraSource")
target_include_directories(ixc_vcam_source PRIVATE ${CMAKE_SOURCE_DIR}/src)
ixc_delay_load_d3d(ixc_vcam_source)
target_link_libraries(ixc_vcam_source PRIVATE ixc_settings ixc_processing_gpu mf mfplat mfuuid mfsensorgroup ole32 advapi32
    runtimeobject)  # combase.dll imports used by the WRL class factory (system DLL)

# ixc_vcam_manager: register / remove / query the system camera (used by the tool and the app).
add_library(ixc_vcam_manager STATIC registration.cpp)
target_link_libraries(ixc_vcam_manager PUBLIC ixc_camera mfsensorgroup cfgmgr32 advapi32 shell32)
