#pragma once

// Registration of the IXC Camera system camera (MFCreateVirtualCamera).
//
// The camera is registered with System lifetime and All Users access, so it survives reboots
// and needs no IXC process running. Both register and remove require administrator rights;
// the installer / uninstaller performs them once.

#include "diagnostics/error.h"

#include <string>
#include <vector>

namespace ixc::vcam {

struct Status {
    bool comRegistered = false;          // HKLM CLSID entry present
    std::wstring registeredDllPath;      // InprocServer32 default value
    bool dllFileExists = false;
    bool dllReadableByService = false;   // not under a user profile (Frame Server can't read those)
    bool cameraPresent = false;          // Windows enumerates an IXC Camera device
    std::string cameraName;              // name as apps see it
    std::wstring cameraLink;
    std::wstring wrappedCameraLink;      // physical camera it wraps (recorded by IXC at registration)
    std::string wrappedCameraName;
};

Status QueryStatus();

// True for the IXC Camera device itself (so it's never offered as a source to wrap).
bool IsIxcCameraLink(const std::wstring& symbolicLink, const std::string& friendlyName);

// Creates (or re-creates) the system camera wrapping `physicalLink`. Requires the source DLL
// to be COM-registered first.
Error Register(const std::wstring& physicalLink, const std::string& physicalName);

// Removes the system camera. Succeeds when it doesn't exist.
Error Unregister();

bool IsProcessElevated();

}  // namespace ixc::vcam
