#pragma once

// Identifiers shared by the IXC Camera media source DLL, the registration tool and the app.
// These values are part of the installed footprint: never change them after a release, or
// existing installations can no longer be removed cleanly.

#include <guiddef.h>

namespace ixc::vcam {

// COM class of the media source's IMFActivate (registered under HKLM\Software\Classes\CLSID).
// {3011A045-BC7A-469D-86D0-2800938E32BF}
inline constexpr GUID kSourceClsid = {0x3011a045, 0xbc7a, 0x469d, {0x86, 0xd0, 0x28, 0x00, 0x93, 0x8e, 0x32, 0xbf}};
inline constexpr wchar_t kSourceClsidString[] = L"{3011A045-BC7A-469D-86D0-2800938E32BF}";

// Virtual camera attribute: symbolic link of the wrapped physical camera (string). Used only as
// a fallback when Frame Server doesn't provide the associated camera source itself.
// {CB5A6A96-8CBE-498B-8B00-9513884343FB}
inline constexpr GUID kAttrPhysicalLink = {0xcb5a6a96, 0x8cbe, 0x498b, {0x8b, 0x00, 0x95, 0x13, 0x88, 0x43, 0x43, 0xfb}};

// TraceLogging provider "IXC.Camera.Source" (zero cost unless a trace session enables it).
// {6880FEE1-8B41-4322-ACF6-E6C1BF585B50}
inline constexpr wchar_t kTraceProviderGuidString[] = L"{6880FEE1-8B41-4322-ACF6-E6C1BF585B50}";

// Friendly name passed to MFCreateVirtualCamera. Windows may append a suffix identifying it as
// a virtual camera; apps show whatever Windows reports.
inline constexpr wchar_t kFriendlyName[] = L"IXC Camera";

inline constexpr wchar_t kSourceDllName[] = L"IXCCameraSource.dll";

}  // namespace ixc::vcam
