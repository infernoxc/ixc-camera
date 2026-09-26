// IXC Camera UI entry point (Phase 2 skeleton).
//
// At this stage the window only proves the build, logging and profile plumbing work end to
// end. Camera preview, settings panels and the first-run wizard come in later phases.

#include "common/strings.h"
#include "diagnostics/error.h"
#include "diagnostics/log.h"
#include "ixc/version.h"
#include "profiles/profile_store.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"IXCCameraMainWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\IXCCamera.UI.SingleInstance";

std::wstring g_status;

std::filesystem::path LocalAppDataDir() {
    PWSTR raw = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
        dir = std::filesystem::path(raw) / L"IXC Camera";
    }
    CoTaskMemFree(raw);
    return dir;
}

std::wstring LoadOrCreateDefaultProfile(const std::filesystem::path& root) {
    ixc::ProfileStore store(root / L"profiles");
    std::string stem = "default";
    ixc::ProfileLoadResult r = store.Load(stem);
    if (r.ok) {
        for (const auto& w : r.warnings) ixc::log::Warn("profiles", "default: " + w);
        return L"Profile loaded: " + ixc::Utf8ToWide(r.profile.name);
    }

    // Only create a fresh default when the file is genuinely absent; never overwrite a file we
    // failed to parse, so the user can recover it.
    if (std::filesystem::exists(store.directory() / L"default.json")) {
        ixc::log::Error("profiles", "default profile unreadable: " + r.error);
        return L"Default profile could not be read: " + ixc::Utf8ToWide(r.error);
    }
    ixc::Profile p;
    const HRESULT hr = store.Save(p, stem);
    if (FAILED(hr)) {
        ixc::Error e{hr, "SaveDefaultProfile", "IXC Camera could not save the default profile."};
        ixc::log::Error("profiles", e.Describe());
        return ixc::Utf8ToWide(e.Describe());
    }
    ixc::log::Info("profiles", "created default profile");
    return L"Created default profile";
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            InflateRect(&rc, -24, -24);
            const std::wstring text = L"IXC Camera " + ixc::Utf8ToWide(IXC_VERSION_STRING) +
                                      L"\n\nDevelopment skeleton build. Camera features are not implemented yet.\n\n" +
                                      g_status;
            SetBkMode(dc, TRANSPARENT);
            HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(dc, text.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);  // no DLL planting from the CWD

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(mutex);
        return 0;
    }

    const std::filesystem::path root = LocalAppDataDir();
    if (!root.empty()) {
        ixc::log::Config cfg;
        cfg.directory = root / L"logs";
        ixc::log::Init(cfg);
        ixc::log::Info("app", std::string("IXC Camera ") + IXC_VERSION_STRING + " starting");
        g_status = LoadOrCreateDefaultProfile(root);
    } else {
        g_status = L"Could not locate the local application data folder.";
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"IXC Camera", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                720, 420, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        ixc::Error e{HRESULT_FROM_WIN32(GetLastError()), "CreateMainWindow", "IXC Camera could not open its window."};
        ixc::log::Error("app", e.Describe());
        MessageBoxW(nullptr, ixc::Utf8ToWide(e.Describe()).c_str(), L"IXC Camera", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, showCmd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ixc::log::Info("app", "exiting");
    ixc::log::Shutdown();
    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
