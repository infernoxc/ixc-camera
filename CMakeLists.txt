cmake_minimum_required(VERSION 3.28)

project(IXCCamera
    VERSION 0.12.0
    DESCRIPTION "IXC Camera - lightweight local-first Windows camera"
    LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "IXC Camera targets Windows 11 x64 only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "IXC Camera requires a 64-bit (x64) build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: no VC++ redistributable, no DLL copying, works inside the
# Frame Server host process that will load the virtual-camera source.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(IXC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(IXC_ENABLE_ASAN "Build with AddressSanitizer (test builds only)" OFF)
option(IXC_BUILD_TESTS "Build unit tests" ON)
# Face tracking uses the vendored libfacedetection (BSD-3-Clause, third_party/libfacedetection).
# OFF builds IXC without it: face tracking then reports "not included in this build".
option(IXC_WITH_FACE_TRACKING "Build face tracking (libfacedetection)" ON)

include(cmake/CompilerSettings.cmake)

configure_file(src/common/version.h.in ${CMAKE_BINARY_DIR}/generated/ixc/version.h @ONLY)

add_subdirectory(src)

if(IXC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
