# Shared compile/link settings applied through the ixc_settings interface target.
# Every IXC target links ixc_settings; nothing sets flags globally.

add_library(ixc_settings INTERFACE)

target_compile_definitions(ixc_settings INTERFACE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A00000C  # Windows 11 (22000+) headers
    $<$<CONFIG:Debug>:IXC_DEBUG=1>)

target_include_directories(ixc_settings INTERFACE
    ${CMAKE_BINARY_DIR}/generated)

set(IXC_CXX_FLAGS
    /W4            # high warning level
    /permissive-   # standards conformance
    /utf-8         # sources and execution charset are UTF-8
    /Zc:__cplusplus
    /Zc:preprocessor
    /EHsc
    /sdl           # additional security checks
    /Zi            # PDBs for every config (symbols are not shipped in the installer)
    /w14265        # class has virtual functions but non-virtual destructor
    /w14062        # enumerator not handled in switch
    /w14242 /w14254 /w14263 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
    /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
set(IXC_LINK_FLAGS /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT /DEBUG)

if(IXC_ENABLE_ASAN)
    list(APPEND IXC_CXX_FLAGS /fsanitize=address)
    list(APPEND IXC_LINK_FLAGS /INCREMENTAL:NO)
else()
    # Control Flow Guard (not combined with ASan instrumentation).
    list(APPEND IXC_CXX_FLAGS /guard:cf)
    list(APPEND IXC_LINK_FLAGS /guard:cf)
endif()

if(IXC_WARNINGS_AS_ERRORS)
    list(APPEND IXC_CXX_FLAGS /WX)
    list(APPEND IXC_LINK_FLAGS /WX)
endif()

set(IXC_CXX_RELEASE_FLAGS /O2 /Gy /Gw)
set(IXC_LINK_RELEASE_FLAGS /OPT:REF /OPT:ICF /INCREMENTAL:NO)

# Flags apply to C++ only; the resource compiler (rc.exe) must not see them.
target_compile_options(ixc_settings INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX>:${IXC_CXX_FLAGS}>"
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<NOT:$<CONFIG:Debug>>>:${IXC_CXX_RELEASE_FLAGS}>")

target_link_options(ixc_settings INTERFACE
    ${IXC_LINK_FLAGS}
    "$<$<NOT:$<CONFIG:Debug>>:${IXC_LINK_RELEASE_FLAGS}>")
