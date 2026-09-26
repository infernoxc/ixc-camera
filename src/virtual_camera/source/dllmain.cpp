// IXCCameraSource.dll entry points.
//
// DllRegisterServer / DllUnregisterServer write or remove only this DLL's own CLSID key under
// HKLM\Software\Classes\CLSID (administrator required). Frame Server runs as a service, so the
// registration must be machine-wide and the DLL must live somewhere the service can read
// (the installer uses %ProgramFiles%\IXC Camera).

#include "virtual_camera/source/activate.h"
#include "virtual_camera/source/trace.h"
#include "virtual_camera/vcam_ids.h"

#include <windows.h>
#include <wrl/module.h>

#include <string>

// {6880FEE1-8B41-4322-ACF6-E6C1BF585B50}
TRACELOGGING_DEFINE_PROVIDER(g_ixcTraceProvider, "IXC.Camera.Source",
                             (0x6880fee1, 0x8b41, 0x4322, 0xac, 0xf6, 0xe6, 0xc1, 0xbf, 0x58, 0x5b, 0x50));

namespace {
HMODULE g_module = nullptr;
}

using ixc::vcam::Activate;
CoCreatableClass(Activate);

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_module = module;
            DisableThreadLibraryCalls(module);
            TraceLoggingRegister(g_ixcTraceProvider);
            break;
        case DLL_PROCESS_DETACH:
            TraceLoggingUnregister(g_ixcTraceProvider);
            break;
        default:
            break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    return Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule().GetClassObject(clsid, riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return Microsoft::WRL::Module<Microsoft::WRL::InProc>::GetModule().Terminate() ? S_OK : S_FALSE;
}

namespace {

std::wstring ClsidKeyPath() { return std::wstring(L"Software\\Classes\\CLSID\\") + ixc::vcam::kSourceClsidString; }

HRESULT SetStringValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    const LSTATUS s = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    return HRESULT_FROM_WIN32(s);
}

}  // namespace

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);

    HKEY clsid = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &clsid, nullptr);
    if (s != ERROR_SUCCESS) return HRESULT_FROM_WIN32(s);
    HRESULT hr = SetStringValue(clsid, nullptr, L"IXC Camera Media Source");

    HKEY server = nullptr;
    if (SUCCEEDED(hr)) {
        s = RegCreateKeyExW(clsid, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &server, nullptr);
        hr = HRESULT_FROM_WIN32(s);
    }
    if (SUCCEEDED(hr)) hr = SetStringValue(server, nullptr, path);
    if (SUCCEEDED(hr)) hr = SetStringValue(server, L"ThreadingModel", L"Both");
    if (server) RegCloseKey(server);
    RegCloseKey(clsid);
    if (FAILED(hr)) RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str());  // no half registration
    return hr;
}

STDAPI DllUnregisterServer() {
    const LSTATUS s = RegDeleteTreeW(HKEY_LOCAL_MACHINE, ClsidKeyPath().c_str());
    return (s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(s);
}
