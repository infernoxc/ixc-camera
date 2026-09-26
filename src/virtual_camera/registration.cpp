#include "virtual_camera/registration.h"

#include "camera/device_enum.h"
#include "common/strings.h"
#include "diagnostics/log.h"
#include "virtual_camera/vcam_ids.h"

#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <shlobj.h>
#include <KnownFolders.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

using Microsoft::WRL::ComPtr;

namespace ixc::vcam {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring ReadRegString(HKEY root, const std::wstring& path, const wchar_t* name) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

// Windows doesn't expose which physical camera a virtual camera wraps (the documented device
// property is empty on build 26200), so IXC records it in its own key. Removing the camera
// deletes the key.
constexpr wchar_t kIxcKey[] = L"SOFTWARE\\IXC Camera";
constexpr wchar_t kWrappedLinkValue[] = L"WrappedCameraLink";
constexpr wchar_t kWrappedNameValue[] = L"WrappedCameraName";

void WriteWrappedCamera(const std::wstring& link, const std::wstring& name) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIxcKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, kWrappedLinkValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(link.c_str()),
                   static_cast<DWORD>((link.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(key, kWrappedNameValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                   static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

void DeleteWrappedCamera() { RegDeleteTreeW(HKEY_LOCAL_MACHINE, kIxcKey); }

HRESULT CreateVcam(ComPtr<IMFVirtualCamera>& vcam) {
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                 MFVirtualCameraAccess_AllUsers, kFriendlyName, kSourceClsidString, nullptr, 0, &vcam);
}

}  // namespace

bool IsIxcCameraLink(const std::wstring& link, const std::string& name) {
    // Virtual cameras created by MFCreateVirtualCamera are software devices (SWD#VCAMDEVAPI).
    const bool vcam = Lower(link).find(L"#vcamdevapi#") != std::wstring::npos;
    return vcam && name.rfind("IXC Camera", 0) == 0;
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) && e.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

Status QueryStatus() {
    Status s;
    const std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsidString + L"\\InprocServer32";
    s.registeredDllPath = ReadRegString(HKEY_LOCAL_MACHINE, key, nullptr);
    s.comRegistered = !s.registeredDllPath.empty();
    if (s.comRegistered) {
        std::error_code ec;
        s.dllFileExists = std::filesystem::is_regular_file(s.registeredDllPath, ec);
        // Frame Server runs as LocalService and can't read files inside user profiles.
        PWSTR profiles = nullptr;
        std::wstring profileRoot;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProfiles, 0, nullptr, &profiles))) profileRoot = Lower(profiles);
        CoTaskMemFree(profiles);
        s.dllReadableByService = s.dllFileExists && (profileRoot.empty() || Lower(s.registeredDllPath).rfind(profileRoot, 0) != 0);
    }

    std::vector<camera::CameraInfo> cams;
    if (SUCCEEDED(camera::EnumerateCameras(cams))) {
        for (const auto& c : cams) {
            if (IsIxcCameraLink(c.symbolicLink, c.name)) {
                s.cameraPresent = true;
                s.cameraName = c.name;
                s.cameraLink = c.symbolicLink;
                s.wrappedCameraLink = ReadRegString(HKEY_LOCAL_MACHINE, kIxcKey, kWrappedLinkValue);
                s.wrappedCameraName = WideToUtf8(ReadRegString(HKEY_LOCAL_MACHINE, kIxcKey, kWrappedNameValue));
                break;
            }
        }
    }
    return s;
}

Error Unregister() {
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = CreateVcam(vcam);
    if (FAILED(hr)) return {hr, "OpenVirtualCamera", "IXC Camera could not open the system camera registration."};
    hr = vcam->Remove();
    vcam->Shutdown();
    if (FAILED(hr)) {
        // Remove() on a camera that isn't registered fails (observed: MF_E_INVALIDREQUEST on build
        // 26200). Treat it as success only if Windows really enumerates no IXC Camera device.
        if (!QueryStatus().cameraPresent) {
            log::Info("vcam", "no system camera to remove (Remove returned " + HResultHex(hr) + ")");
            DeleteWrappedCamera();
            return {};
        }
        return {hr, "RemoveVirtualCamera", "IXC Camera could not remove the system camera."};
    }
    DeleteWrappedCamera();
    log::Info("vcam", "system camera removed");
    return {};
}

Error Register(const std::wstring& physicalLink, const std::string& physicalName) {
    if (physicalLink.empty()) return {E_INVALIDARG, "RegisterVirtualCamera", "No physical camera was selected for IXC Camera."};

    BOOL supported = FALSE;
    HRESULT hr = MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported);
    if (FAILED(hr) || !supported) {
        return {FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "CheckVirtualCameraSupport",
                "This version of Windows doesn't support software cameras (Windows 11 build 22000 or later is required)."};
    }
    const Status st = QueryStatus();
    if (!st.comRegistered || !st.dllFileExists) {
        return {REGDB_E_CLASSNOTREG, "CheckSourceRegistration", "The IXC Camera media source is not installed."};
    }

    // Re-create from scratch so the wrapped camera always matches the request.
    if (Error e = Unregister(); FAILED(e.hr)) return e;

    ComPtr<IMFVirtualCamera> vcam;
    hr = CreateVcam(vcam);
    if (FAILED(hr)) return {hr, "CreateVirtualCamera", "IXC Camera could not register the system camera."};
    hr = vcam->AddDeviceSourceInfo(physicalLink.c_str());
    if (SUCCEEDED(hr)) hr = vcam->SetString(kAttrPhysicalLink, physicalLink.c_str());
    if (FAILED(hr)) {
        vcam->Shutdown();
        return {hr, "AddDeviceSourceInfo", "IXC Camera could not link the system camera to the webcam."};
    }
    hr = vcam->Start(nullptr);
    if (FAILED(hr)) {
        vcam->Remove();  // don't leave a half-registered camera behind
        vcam->Shutdown();
        return {hr, "StartVirtualCamera", "IXC Camera could not register the system camera."};
    }
    vcam->Shutdown();  // System lifetime: the camera stays registered after this object is gone
    WriteWrappedCamera(physicalLink, Utf8ToWide(physicalName));
    log::Info("vcam", "system camera registered for " + physicalName + " (" + WideToUtf8(physicalLink) + ")");
    return {};
}

}  // namespace ixc::vcam
