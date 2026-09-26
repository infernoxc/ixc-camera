// IXC-Camera-Setup-x64.exe: installer and uninstaller (Phase 12). Native, no installer framework.
//
//   IXC-Camera-Setup-x64.exe [/quiet]                       install or upgrade
//   IXC-Camera-Setup-x64.exe /uninstall [/quiet] [/removeuserdata]
//
// The program files are embedded as resources (payload.rc). Every step is logged to
// %TEMP%\IXC-Camera-Setup.log; exit codes are documented in docs/installer.md. A failed step is
// never reported as success: interactive runs offer Retry, and on cancel (or /quiet) the partial
// installation is rolled back.

#include "ixc/version.h"

#include <windows.h>
#include <aclapi.h>
#include <commctrl.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <functional>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

namespace {

enum ExitCode : int {
    kOk = 0,
    kCancelled = 1,
    kUnsupportedOs = 2,
    kUnsupportedArch = 3,
    kFilesFailed = 4,
    kComFailed = 5,
    kCameraFailed = 6,
    kUninstallIncomplete = 7,
    kBadArguments = 8,
    kShortcutFailed = 9,
    kRebootRequired = 3010,  // success; some files are removed at the next restart
};

constexpr wchar_t kProduct[] = L"IXC Camera";
constexpr wchar_t kSetupName[] = L"IXC-Camera-Setup-x64.exe";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\IXC Camera";
constexpr wchar_t kInprocKey[] = L"SOFTWARE\\Classes\\CLSID\\{3011A045-BC7A-469D-86D0-2800938E32BF}\\InprocServer32";
constexpr wchar_t kSettingsKey[] = L"SOFTWARE\\IXC Camera";

struct PayloadFile {
    int resource;
    const wchar_t* name;
};
constexpr PayloadFile kFiles[] = {
    {101, L"IXCCameraSource.dll"}, {102, L"ixc_vcam.exe"}, {103, L"IXCCamera.exe"}, {104, L"LICENSE.txt"}, {105, L"THIRD_PARTY_LICENSES.md"},
};

bool g_quiet = false;
bool g_rebootNeeded = false;
FILE* g_log = nullptr;

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g_log, L"%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list a;
    va_start(a, fmt);
    vfwprintf(g_log, fmt, a);
    va_end(a);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
std::wstring InstallDir() { return KnownFolder(FOLDERID_ProgramFiles) + L"\\IXC Camera"; }
std::wstring SettingsDir() { return KnownFolder(FOLDERID_ProgramData) + L"\\IXC Camera"; }
std::wstring ShortcutPath() { return KnownFolder(FOLDERID_CommonPrograms) + L"\\IXC Camera.lnk"; }
std::wstring DesktopShortcutPath() { return KnownFolder(FOLDERID_PublicDesktop) + L"\\IXC Camera.lnk"; }
std::wstring LogPath() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"IXC-Camera-Setup.log";
}
std::wstring SelfPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}
std::wstring SysMessage(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s;
}

// ---- UI --------------------------------------------------------------------------------------------

int Dialog(const wchar_t* main, const std::wstring& content, TASKDIALOG_COMMON_BUTTON_FLAGS buttons, PCWSTR icon,
           const wchar_t* verification = nullptr, BOOL* verified = nullptr) {
    if (g_quiet) return IDCANCEL;
    TASKDIALOGCONFIG c{sizeof(c)};
    c.pszWindowTitle = L"IXC Camera Setup";
    c.pszMainInstruction = main;
    c.pszContent = content.c_str();
    c.dwCommonButtons = buttons;
    c.pszMainIcon = icon;
    c.pszVerificationText = verification;
    if (verified && *verified) c.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    int button = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&c, &button, nullptr, verified))) return IDCANCEL;
    return button;
}

// ---- steps -----------------------------------------------------------------------------------------

struct StepError {
    std::wstring stage;
    std::wstring message;
    int exitCode;
};

bool ReadAllowed(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void DeleteNowOrAtReboot(const std::wstring& path) {
    if (!ReadAllowed(path)) return;
    if (DeleteFileW(path.c_str())) {
        Log(L"deleted %s", path.c_str());
        return;
    }
    // Still loaded (the camera service may hold IXCCameraSource.dll): rename, delete at restart.
    const bool alreadyAside = path.find(L".old-") != std::wstring::npos;
    const std::wstring aside = alreadyAside ? path : path + L".old-" + std::to_wstring(GetTickCount64());
    const std::wstring& target = !alreadyAside && MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING) ? aside : path;
    MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    g_rebootNeeded = true;
    Log(L"in use, removed at next restart: %s", target.c_str());
}

bool ExtractFile(const PayloadFile& f, const std::wstring& dir, StepError& err) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(f.resource), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    const DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!data || !size) {
        err = {L"ExtractFiles", std::wstring(L"The setup file is damaged (missing ") + f.name + L"). Download it again.", kFilesFailed};
        return false;
    }
    const std::wstring path = dir + L"\\" + f.name, tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, data, size, &written, nullptr) && written == size;
    const DWORD e = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        err = {L"ExtractFiles", L"Could not write " + path + L": " + SysMessage(e), kFilesFailed};
        return false;
    }
    if (ReadAllowed(path) && !DeleteFileW(path.c_str())) DeleteNowOrAtReboot(path);  // old version still loaded
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = {L"ExtractFiles", L"Could not place " + path + L": " + SysMessage(GetLastError()), kFilesFailed};
        return false;
    }
    Log(L"installed %s (%lu bytes)", path.c_str(), size);
    return true;
}

bool CallDllEntry(const std::wstring& dll, const char* entry, HRESULT& hr) {
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }
    using Fn = HRESULT(STDAPICALLTYPE*)();
    auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, entry)));
    hr = fn ? fn() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    FreeLibrary(m);
    return SUCCEEDED(hr);
}

std::wstring RegisteredDll() {
    wchar_t v[MAX_PATH] = L"";
    DWORD size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kInprocKey, nullptr, RRF_RT_REG_SZ, nullptr, v, &size) != ERROR_SUCCESS) return L"";
    return v;
}

bool Run(const std::wstring& exe, const std::wstring& args, DWORD& exitCode, DWORD timeoutMs = 120000) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        exitCode = GetLastError();
        return false;
    }
    const bool done = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1460);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return done;
}

bool CreateSettingsFolder(StepError& err) {
    const std::wstring dir = SettingsDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    // SYSTEM, Administrators: full; Users: modify (the app publishes settings); LOCAL SERVICE
    // (the camera service): read. Protected: nothing inherited.
    PSECURITY_DESCRIPTOR sd = nullptr;
    const wchar_t* sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)(A;OICI;0x1200a9;;;LS)";
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    DWORD e = ERROR_SUCCESS;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr) ||
        !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)) {
        e = GetLastError();
    } else {
        e = SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
    }
    if (sd) LocalFree(sd);
    if (e != ERROR_SUCCESS) {
        err = {L"SettingsFolder", L"Could not set up " + dir + L": " + SysMessage(e), kFilesFailed};
        return false;
    }
    Log(L"settings folder ready: %s", dir.c_str());
    return true;
}

// Writes (or overwrites: an upgrade never duplicates it) one shortcut to the IXC Camera app.
HRESULT WriteShortcut(const std::wstring& path) {
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> file;
    const std::wstring exe = InstallDir() + L"\\IXCCamera.exe";
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) hr = link->SetPath(exe.c_str());
    if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(InstallDir().c_str());
    if (SUCCEEDED(hr)) hr = link->SetIconLocation(exe.c_str(), 0);
    if (SUCCEEDED(hr)) hr = link->SetDescription(L"Open IXC Camera: preview, picture, effects and profiles");
    if (SUCCEEDED(hr)) hr = link.As(&file);
    if (SUCCEEDED(hr)) hr = file->Save(path.c_str(), TRUE);
    return hr;
}

bool CreateShortcut(StepError& err) {
    for (const std::wstring& path : {ShortcutPath(), DesktopShortcutPath()}) {
        const HRESULT hr = WriteShortcut(path);
        if (FAILED(hr)) {
            err = {L"Shortcuts", L"Could not create the shortcut " + path + L": " + SysMessage(static_cast<DWORD>(hr)), kShortcutFailed};
            return false;
        }
        Log(L"shortcut: %s", path.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // refresh cached icons
    return true;
}

bool WriteUninstallEntry(StepError& err) {
    HKEY k = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
    if (s == ERROR_SUCCESS) {
        const std::wstring dir = InstallDir(), setup = dir + L"\\" + kSetupName;
        auto sz = [&](const wchar_t* name, const std::wstring& v) {
            RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
        };
        auto dw = [&](const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)); };
        sz(L"DisplayName", kProduct);
        sz(L"DisplayVersion", L"" IXC_VERSION_STRING);
        sz(L"Publisher", L"IXC Camera contributors");
        sz(L"InstallLocation", dir);
        sz(L"DisplayIcon", dir + L"\\IXCCamera.exe,0");
        sz(L"UninstallString", L"\"" + setup + L"\" /uninstall");
        sz(L"QuietUninstallString", L"\"" + setup + L"\" /uninstall /quiet");
        dw(L"NoModify", 1);
        dw(L"NoRepair", 1);
        DWORD kb = 0;
        for (const auto& f : kFiles) {
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (GetFileAttributesExW((dir + L"\\" + f.name).c_str(), GetFileExInfoStandard, &a)) kb += a.nFileSizeLow / 1024;
        }
        dw(L"EstimatedSize", kb);
        RegCloseKey(k);
    }
    if (s != ERROR_SUCCESS) {
        err = {L"AppsAndFeatures", L"Could not register with Apps & features: " + SysMessage(static_cast<DWORD>(s)), kFilesFailed};
        return false;
    }
    Log(L"Apps & features entry written");
    return true;
}

// Removes IXC Camera. keepSettings: upgrade (the published settings copy stays).
// Returns false when something couldn't be removed (logged); never touches other cameras.
bool UninstallCore(bool keepSettings, bool removeUserData) {
    bool complete = true;
    const std::wstring dir = InstallDir();
    const std::wstring vcam = dir + L"\\ixc_vcam.exe", dll = dir + L"\\IXCCameraSource.dll";
    DWORD code = 0;
    if (ReadAllowed(vcam)) {
        const bool ran = Run(vcam, L"unregister", code);
        Log(L"RemoveSystemCamera: %s, exit %lu", ran ? L"ran" : L"failed to run", code);
        if (!ran || code != 0) complete = false;
    }
    if (ReadAllowed(dll)) {
        HRESULT hr = S_OK;
        const bool ok = CallDllEntry(dll, "DllUnregisterServer", hr);
        Log(L"UnregisterComServer: 0x%08lX", static_cast<unsigned long>(hr));
        if (!ok) complete = false;
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKey);  // IXC-owned (wrapped camera record)
    DeleteFileW(ShortcutPath().c_str());
    DeleteFileW(DesktopShortcutPath().c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);

    const std::wstring self = SelfPath();
    for (const auto& f : kFiles) DeleteNowOrAtReboot(dir + L"\\" + f.name);
    for (const wchar_t* extra : {L"install-manifest.json", kSetupName}) {
        const std::wstring p = dir + L"\\" + extra;
        if (_wcsicmp(p.c_str(), self.c_str()) != 0) DeleteNowOrAtReboot(p);
    }
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*.old-*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteNowOrAtReboot(dir + L"\\" + fd.cFileName);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (ReadAllowed(dir) && !RemoveDirectoryW(dir.c_str())) {
        MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);  // empty after the restart
        g_rebootNeeded = true;
    }
    if (!keepSettings) {
        const std::wstring s = SettingsDir();
        DeleteFileW((s + L"\\active-profile.json").c_str());
        RemoveDirectoryW(s.c_str());
    }
    if (removeUserData) {
        // Profiles and logs of the user running the uninstaller.
        const std::wstring data = KnownFolder(FOLDERID_LocalAppData) + L"\\IXC Camera";
        std::wstring from = data + L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        const int r = ReadAllowed(data) ? SHFileOperationW(&op) : 0;
        Log(L"RemoveUserData %s: %d", data.c_str(), r);
    }
    Log(L"uninstall %s", complete ? L"complete" : L"incomplete (see above)");
    return complete;
}

// ---- flows -----------------------------------------------------------------------------------------

int CheckPlatform() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))) : nullptr;
    if (!fn || fn(&v) != 0 || v.dwMajorVersion < 10 || v.dwBuildNumber < 22000) {
        Log(L"unsupported Windows build %lu", v.dwBuildNumber);
        Dialog(L"Windows 11 is required", L"IXC Camera uses the Windows 11 software camera feature (Windows 11 build 22000 or newer).",
               TDCBF_CLOSE_BUTTON, TD_ERROR_ICON);
        return kUnsupportedOs;
    }
    USHORT process = 0, native = 0;
    if (!IsWow64Process2(GetCurrentProcess(), &process, &native) || native != IMAGE_FILE_MACHINE_AMD64) {
        Log(L"unsupported machine 0x%04X", native);
        Dialog(L"An x64 PC is required", L"This IXC Camera build runs on 64-bit Intel/AMD (x64) Windows only.", TDCBF_CLOSE_BUTTON, TD_ERROR_ICON);
        return kUnsupportedArch;
    }
    Log(L"Windows build %lu, x64", v.dwBuildNumber);
    return kOk;
}

int Install() {
    if (const int rc = CheckPlatform(); rc != kOk) return rc;
    if (Dialog(L"Install IXC Camera " IXC_VERSION_STRING L"?",
               L"IXC Camera adds a camera called \"IXC Camera\" that any app can select, with picture adjustments and "
               L"effects. Your webcam stays usable as before.\n\nNo drivers, no background service of its own, no "
               L"internet connection.",
               TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, TD_INFORMATION_ICON) != IDOK &&
        !g_quiet) {
        Log(L"cancelled by the user");
        return kCancelled;
    }
    const std::wstring dir = InstallDir();
    if (ReadAllowed(dir) || ReadAllowed(dir + L"\\IXCCameraSource.dll")) {
        Log(L"existing installation: upgrading (profiles and settings are kept)");
        UninstallCore(true, false);
    }

    const std::vector<std::pair<const wchar_t*, std::function<bool(StepError&)>>> steps = {
        {L"Copying files", [&](StepError& e) {
             CreateDirectoryW(dir.c_str(), nullptr);
             for (const auto& f : kFiles) {
                 if (!ExtractFile(f, dir, e)) return false;
             }
             const std::wstring self = SelfPath(), copy = dir + L"\\" + kSetupName;  // the uninstaller
             if (_wcsicmp(self.c_str(), copy.c_str()) != 0 && !CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
                 e = {L"ExtractFiles", L"Could not copy the uninstaller: " + SysMessage(GetLastError()), kFilesFailed};
                 return false;
             }
             return true;
         }},
        {L"Preparing the settings folder", CreateSettingsFolder},
        {L"Registering the camera component", [&](StepError& e) {
             const std::wstring dll = dir + L"\\IXCCameraSource.dll";
             HRESULT hr = S_OK;
             if (!CallDllEntry(dll, "DllRegisterServer", hr) || _wcsicmp(RegisteredDll().c_str(), dll.c_str()) != 0) {
                 wchar_t hex[16];
                 swprintf_s(hex, L"0x%08lX", static_cast<unsigned long>(hr));
                 e = {L"RegisterComServer", std::wstring(L"Registering IXCCameraSource.dll failed (") + hex + L").", kComFailed};
                 return false;
             }
             Log(L"COM server registered: %s", dll.c_str());
             return true;
         }},
        {L"Adding the IXC Camera system camera", [&](StepError& e) {
             DWORD code = 0;
             const bool ran = Run(dir + L"\\ixc_vcam.exe", L"register", code);
             Log(L"RegisterSystemCamera: exit %lu", code);
             if (!ran || code != 0) {
                 e = {L"RegisterSystemCamera",
                      L"Windows didn't accept the IXC Camera software camera (code " + std::to_wstring(code) +
                          L"). Make sure a webcam is connected and camera access is allowed in Settings > Privacy & security > Camera.",
                      kCameraFailed};
                 return false;
             }
             const bool verified = Run(dir + L"\\ixc_vcam.exe", L"status", code) && code == 0;
             Log(L"VerifySystemCamera: exit %lu", code);
             if (!verified) {
                 e = {L"VerifySystemCamera", L"IXC Camera was registered but Windows doesn't list it (code " + std::to_wstring(code) + L").", kCameraFailed};
                 return false;
             }
             return true;
         }},
        {L"Adding the Start menu and desktop shortcuts", CreateShortcut},
        {L"Registering with Apps & features", WriteUninstallEntry},
    };

    for (const auto& [name, step] : steps) {
        for (;;) {
            Log(L"step: %s", name);
            StepError err;
            if (step(err)) break;
            Log(L"FAILED at %s: %s (exit code %d)", err.stage.c_str(), err.message.c_str(), err.exitCode);
            const int b = Dialog(L"IXC Camera setup couldn't finish", err.message + L"\n\nStage: " + err.stage + L"\nLog: " + LogPath(),
                                 TDCBF_RETRY_BUTTON | TDCBF_CANCEL_BUTTON, TD_ERROR_ICON);
            if (b == IDRETRY) continue;
            Log(L"rolling back");
            UninstallCore(true, false);
            return err.exitCode;
        }
    }
    Log(L"install complete%s", g_rebootNeeded ? L" (an old file is removed at the next restart)" : L"");
    BOOL launch = TRUE;
    Dialog(L"IXC Camera is installed",
           L"Select \"IXC Camera\" as the camera in any app. Open IXC Camera from the Start menu to adjust the picture and effects.",
           TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON, L"Open IXC Camera now", &launch);
    if (launch && !g_quiet) {
        // Through Explorer, so the app runs as the signed-in user, not elevated.
        DWORD code = 0;
        Run(KnownFolder(FOLDERID_Windows) + L"\\explorer.exe", L"\"" + dir + L"\\IXCCamera.exe\"", code, 0);
    }
    return g_rebootNeeded ? kRebootRequired : kOk;
}

int Uninstall(bool removeUserData, bool fromTemp) {
    const std::wstring dir = InstallDir(), self = SelfPath();
    // The uninstaller lives in the folder it removes: run a temporary copy instead.
    if (!fromTemp && _wcsnicmp(self.c_str(), dir.c_str(), dir.size()) == 0) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring copy = std::wstring(tmp) + L"IXC-Camera-Uninstall-" + std::to_wstring(GetTickCount64()) + L".exe";
        if (CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
            std::wstring args = L"/uninstall /fromtemp";
            if (g_quiet) args += L" /quiet";
            if (removeUserData) args += L" /removeuserdata";
            DWORD code = 0;
            Run(copy, args, code, INFINITE);
            MoveFileExW(copy.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            return static_cast<int>(code);
        }
    }
    if (!g_quiet) {
        if (Dialog(L"Uninstall IXC Camera?", L"The IXC Camera system camera and the IXC app are removed. Your webcam and other apps aren't affected.",
                   TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_WARNING_ICON) != IDYES) {
            Log(L"uninstall cancelled");
            return kCancelled;
        }
        if (!removeUserData) {
            removeUserData = Dialog(L"Also delete your IXC profiles and settings?", L"Choose No to keep them for a later reinstall.",
                                    TDCBF_YES_BUTTON | TDCBF_NO_BUTTON, TD_INFORMATION_ICON) == IDYES;
        }
    }
    const bool complete = UninstallCore(false, removeUserData);
    if (fromTemp) MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    if (!complete) {
        Dialog(L"IXC Camera was only partly removed", L"Some parts couldn't be removed. Details are in " + LogPath() + L".", TDCBF_CLOSE_BUTTON,
               TD_WARNING_ICON);
        return kUninstallIncomplete;
    }
    Dialog(L"IXC Camera was removed", g_rebootNeeded ? L"A few files are removed at the next restart." : L"", TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON);
    return g_rebootNeeded ? kRebootRequired : kOk;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool uninstall = false, removeUserData = false, fromTemp = false, bad = false;
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!_wcsicmp(a, L"/uninstall")) uninstall = true;
        else if (!_wcsicmp(a, L"/quiet")) g_quiet = true;
        else if (!_wcsicmp(a, L"/removeuserdata")) removeUserData = true;
        else if (!_wcsicmp(a, L"/fromtemp")) fromTemp = true;
        else bad = true;
    }
    LocalFree(argv);
    g_log = _wfsopen(LogPath().c_str(), L"a, ccs=UTF-8", _SH_DENYNO);  // shared: the uninstaller's temp copy logs too
    Log(L"==== IXC Camera Setup %s: %s%s", L"" IXC_VERSION_STRING, uninstall ? L"uninstall" : L"install", g_quiet ? L" (quiet)" : L"");
    if (bad) {
        Log(L"bad arguments");
        Dialog(L"Unknown option", L"Usage: IXC-Camera-Setup-x64.exe [/quiet] | /uninstall [/quiet] [/removeuserdata]", TDCBF_CLOSE_BUTTON,
               TD_ERROR_ICON);
        return kBadArguments;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int rc = uninstall ? Uninstall(removeUserData, fromTemp) : Install();
    CoUninitialize();
    Log(L"exit code %d", rc);
    if (g_log) fclose(g_log);
    return rc;
}
