# IXC-Camera-Setup-x64.exe: installer/uninstaller with the release binaries embedded (Phase 12).
# The payload is staged by a custom command (so the setup re-embeds whenever a binary changes),
# then compiled into the setup as RCDATA resources.
set(IXC_PAYLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/payload)
set(IXC_PAYLOAD_FILES
    ${IXC_PAYLOAD_DIR}/IXCCameraSource.dll ${IXC_PAYLOAD_DIR}/ixc_vcam.exe ${IXC_PAYLOAD_DIR}/IXCCamera.exe
    ${IXC_PAYLOAD_DIR}/LICENSE.txt ${IXC_PAYLOAD_DIR}/THIRD_PARTY_LICENSES.md)
add_custom_command(
    OUTPUT ${IXC_PAYLOAD_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${IXC_PAYLOAD_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ixc_vcam_source> ${IXC_PAYLOAD_DIR}/IXCCameraSource.dll
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ixc_vcam> ${IXC_PAYLOAD_DIR}/ixc_vcam.exe
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ixc_camera_app> ${IXC_PAYLOAD_DIR}/IXCCamera.exe
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LICENSE ${IXC_PAYLOAD_DIR}/LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md ${IXC_PAYLOAD_DIR}/THIRD_PARTY_LICENSES.md
    DEPENDS ixc_vcam_source ixc_vcam ixc_camera_app $<TARGET_FILE:ixc_vcam_source> $<TARGET_FILE:ixc_vcam>
            $<TARGET_FILE:ixc_camera_app> ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md
    COMMENT "Staging the installer payload"
    VERBATIM)

set(IXC_PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/payload.rc)
file(WRITE ${IXC_PAYLOAD_RC}
"101 RCDATA \"payload/IXCCameraSource.dll\"
102 RCDATA \"payload/ixc_vcam.exe\"
103 RCDATA \"payload/IXCCamera.exe\"
104 RCDATA \"payload/LICENSE.txt\"
105 RCDATA \"payload/THIRD_PARTY_LICENSES.md\"
1 ICON \"${CMAKE_SOURCE_DIR}/src/app/ixc.ico\"
")
set_source_files_properties(${IXC_PAYLOAD_RC} PROPERTIES OBJECT_DEPENDS "${IXC_PAYLOAD_FILES};${CMAKE_SOURCE_DIR}/src/app/ixc.ico")

add_executable(ixc_setup WIN32 setup.cpp setup.manifest ${IXC_PAYLOAD_RC} ${IXC_PAYLOAD_FILES})
set_target_properties(ixc_setup PROPERTIES OUTPUT_NAME "IXC-Camera-Setup-x64")
target_include_directories(ixc_setup PRIVATE ${CMAKE_CURRENT_BINARY_DIR})  # rc resolves payload/ from here
target_link_libraries(ixc_setup PRIVATE ixc_settings comctl32 shell32 ole32 advapi32)
target_link_options(ixc_setup PRIVATE /MANIFESTUAC:NO)  # setup.manifest carries requireAdministrator
