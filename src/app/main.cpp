// IXC Camera UI: camera selection, native format selection, live preview, and the IXC Camera
// system camera status (which webcam it uses; changing it asks for administrator approval).
//
// Resource rules this window follows:
//   * The physical camera is opened only while the preview runs, and it's released when the
//     window is minimized or closed.
//   * Frames are painted only when they arrive. There is no render loop.
//   * The 1 s status timer runs only while previewing.

#include "app/perf_monitor.h"
#include "app/settings_panel.h"
#include "app/updater.h"
#include "app/widgets.h"
#include "app/preview_window.h"
#include "camera/capture_session.h"
#include "camera/device_enum.h"
#include "camera/format_select.h"
#include "camera/power_line.h"
#include "common/fileio.h"
#include "common/strings.h"
#include "diagnostics/error.h"
#include "diagnostics/log.h"
#include "effects/backgrounds.h"
#include "effects/effects.h"
#include "face/face_settings.h"
#include "ixc/version.h"
#include "processing/image_pipeline.h"
#include "profiles/active_profile.h"
#include "profiles/app_settings.h"
#include "profiles/profile_store.h"
#include "profiles/settings_sync.h"
#include "virtual_camera/registration.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <dbt.h>
#include <ks.h>
#include <ksmedia.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <commdlg.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace ixc;
using namespace ixc::camera;

namespace {

constexpr wchar_t kWindowClass[] = L"IXCCameraMainWindow";

// What the IXC Camera source receives: Auto anti-flicker resolved for this user's region (the
// camera service runs under a service account and can't know it; there Auto = leave as is).
Profile ForPublishing(const Profile& p) {
    Profile out = p;
    out.antiFlicker = ResolveAntiFlicker(p.antiFlicker, UserRegion());
    return out;
}
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\IXCCamera.UI.SingleInstance";

enum ControlId : int {
    kIdCamera = 101,
    kIdFormat = 103,
    kIdStartStop = 104,
    kIdPreview = 105,
    kIdStatus = 106,
    kIdHint = 107,
    kIdPrivacy = 108,
    kIdPerf = 109,
    kIdRefresh = 110,
    kIdUpdatePill = 111,   // header "Update available" (opens the Updates section)
    kIdNav0 = 120,         // navigation rail: one per app::PanelSection, 120..125
    kIdQuick0 = 130,       // bottom bar: background None/Blur/Image/Colour 130..133, effects 134
    // Settings column controls: app::PanelId (settings_panel.h), 200 and up.
};
using app::kIdProfileCombo;
using app::kIdProfileSave;
using app::kIdProfileDelete;
using app::kIdProfileImport;
using app::kIdProfileExport;
using app::kIdHotkeys;
using app::kIdVcamUse;
using app::PanelSection;

// Navigation rail entries, in app::PanelSection order.
constexpr const wchar_t* kNavLabels[app::kPanelSections] = {L"Camera", L"Effects", L"Background", L"Profiles", L"Settings", L"Updates"};
// Bottom quick bar: background shortcuts (modes the renderer supports) and the effects switch.
constexpr BackgroundMode kQuickModes[] = {BackgroundMode::Original, BackgroundMode::Blur, BackgroundMode::Replace, BackgroundMode::Color};
constexpr const wchar_t* kQuickLabels[] = {L"None", L"Blur", L"Image", L"Colour"};
constexpr int kQuickCount = 4;
constexpr double kUpdateCheckInterval = 24.0 * 3600.0;  // automatic check at most once a day

constexpr UINT kStateMessage = WM_APP + 11;
constexpr UINT kVcamDoneMessage = WM_APP + 12;  // wParam = ixc_vcam.exe exit code
constexpr UINT kAutoStartMessage = WM_APP + 13;  // start the preview once the window is shown
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kDeviceRefreshTimer = 2;
constexpr UINT_PTR kPublishTimer = 3;       // sends settings to IXC Camera (coalesced, see profiles/settings_sync.h)
constexpr UINT_PTR kPerfTimer = 4;          // live CPU/RAM readout (always on, 1 s)
constexpr UINT_PTR kSaveTimer = 5;          // writes the user's profile after the last change

std::filesystem::path LocalAppDataDir() {
    PWSTR raw = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
        dir = std::filesystem::path(raw) / L"IXC Camera";
    }
    CoTaskMemFree(raw);
    return dir;
}

std::wstring W(const std::string& s) { return Utf8ToWide(s); }

class MainWindow : public ICaptureListener {
public:
    explicit MainWindow(HINSTANCE instance, std::filesystem::path dataDir)
        : instance_(instance), dataDir_(std::move(dataDir)), store_(dataDir_ / L"profiles") {}

    bool Create(int showCmd);

    // ICaptureListener — called on capture threads; only posts messages.
    void OnFrameAvailable() override { preview_.NotifyFrameAvailable(); }
    void OnStateChanged(CaptureState s, const Error&) override {
        PostMessageW(hwnd_, kStateMessage, static_cast<WPARAM>(s), 0);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void CreateControls();
    void ApplyFont();
    void Layout();
    void Paint(HDC dc, const RECT& rc);
    void SetHint(const std::wstring& text);
    void SetVcamText(const std::wstring& text) {
        SetWindowTextW(vcamStatus_, text.c_str());
        panel_.Relayout();
    }
    void RefreshFeatureStates();
    LRESULT ControlColor(HWND control, HDC dc);
    int Scale(int v) const { return MulDiv(v, dpi_, 96); }
    int VcamPillWidth() const;

    void LoadProfile();
    void SaveProfile();
    void RefreshCameras();
    void OnCameraSelected();
    void StartPreview();
    void StopPreview(const wchar_t* placeholder);
    void UpdateStatus();
    void OnCaptureState(CaptureState s);
    void ShowPrivacyHelp(bool show);
    void RefreshVcamStatus();
    void UpdateVcamControls();
    void UseSelectedForIxcCamera();
    void OnVcamDone(DWORD exitCode);
    void OnPictureChanged();
    void UpdatePipeline();
    void SaveAndPublish();
    void PublishNow();
    SettingsSync sync_;
    // Navigation, quick bar, updates (0.14).
    void SelectSection(PanelSection s);
    void RefreshQuickBar();
    void OnQuick(int index);
    void OnUpdateChecked(const app::UpdateCheckResult& r);
    void OnUpdateDownloaded(const app::UpdateDownloadResult& r);
    void InstallUpdate();
    void SkipUpdate();
    HWND nav_[app::kPanelSections] = {};
    HWND quick_[kQuickCount + 1] = {};
    HWND refresh_ = nullptr, updatePill_ = nullptr;
    RECT rail_{}, quickBar_{};
    app::Updater updater_;
    ReleaseInfo update_;  // newest release found (update_.ok), offered to the user
    // Profiles and hotkeys (Phase 9).
    void RefreshProfileList();
    void SwitchProfile(const std::string& stem);
    void CycleProfile(int step);
    void SaveProfileAs();
    void DeleteProfile();
    void ImportProfile();
    void ExportProfile();
    void SaveAppSettings();
    void RegisterHotkeys();
    void UnregisterHotkeys();
    void OnHotkey(int id);
    std::string UniqueStem(std::string_view name) const;
    static void CALLBACK OnVcamProcessExit(void* ctx, BOOLEAN timedOut);

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND camera_ = nullptr, format_ = nullptr, startStop_ = nullptr;
    HWND status_ = nullptr, hint_ = nullptr, privacy_ = nullptr, perf_ = nullptr;
    app::PerfMonitor perfMonitor_;
    void UpdatePerf();
    // Owned by the settings panel (see settings_panel.h); handled here.
    HWND vcamStatus_ = nullptr, vcamUse_ = nullptr, profileCombo_ = nullptr, profileDelete_ = nullptr, hotkeys_ = nullptr;
    app::theme::Fonts fonts_;
    HICON iconSmall_ = nullptr, iconLarge_ = nullptr, iconHeader_ = nullptr;
    RECT header_{}, previewFrame_{}, statusDot_{};
    enum class VcamState { Missing, Inactive, Active } vcamState_ = VcamState::Missing;
    std::wstring vcamPill_;
    AppSettings app_;
    std::string stem_ = "default";  // active profile file
    bool hotkeysRegistered_ = false;
    std::string cycledLens_;  // the effect the lens hotkeys added last ("" = none); only it is replaced
    effects::BackgroundSource bgSource_;  // the preview's background picture (one cached)
    vcam::Status vcam_;
    HANDLE vcamProcess_ = nullptr;  // elevated ixc_vcam.exe while a change is in progress
    HANDLE vcamWait_ = nullptr;

    UINT dpi_ = 96;
    HDEVNOTIFY devNotify_ = nullptr;

    std::filesystem::path dataDir_;
    ProfileStore store_;
    Profile profile_;
    double smoothEv_ = 0;  // Smooth motion gain in the current preview pipeline
    bool profileLoaded_ = false;

    std::vector<CameraInfo> cameras_;
    std::vector<CaptureFormat> formats_;  // normalized; format combo item i+1 ↔ formats_[i] (item 0 = Auto)
    ComPtr<CaptureSession> session_;
    app::PreviewWindow preview_;
    app::SettingsPanel panel_;
    bool publishWarned_ = false;
    bool previewing_ = false;
    bool resumeOnRestore_ = false;
};

bool MainWindow::Create(int showCmd) {
    const int large = GetSystemMetrics(SM_CXICON), smallSize = GetSystemMetrics(SM_CXSMICON);
    iconLarge_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, large, large, LR_DEFAULTCOLOR));
    iconSmall_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, smallSize, smallSize, LR_DEFAULTCOLOR));
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted in WM_PAINT (no flicker)
    wc.hIcon = iconLarge_;
    wc.hIconSm = iconSmall_;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    app::PreviewWindow::RegisterClass(instance_);
    app::RegisterWidgetClass(instance_);

    // A comfortable default size, never larger than the work area.
    RECT work{0, 0, 1280, 800};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT sysDpi = GetDpiForSystem();
    const int ww = std::min(MulDiv(1280, static_cast<int>(sysDpi), 96), static_cast<int>(work.right - work.left) * 94 / 100);
    const int wh = std::min(MulDiv(820, static_cast<int>(sysDpi), 96), static_cast<int>(work.bottom - work.top) * 94 / 100);
    hwnd_ = CreateWindowExW(0, kWindowClass, L"IXC Camera", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, ww, wh,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;
    app::theme::ApplyDarkWindow(hwnd_);

    if (FAILED(CaptureSession::Create(this, session_))) return false;

    // Refresh the camera list when webcams are plugged in or removed (event-driven).
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = KSCATEGORY_VIDEO_CAMERA;
    devNotify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    LoadProfile();
    panel_.SetFaceOverlay(app_.showFaceMarkers);
    panel_.Refresh();
    RefreshProfileList();
    SendMessageW(hotkeys_, BM_SETCHECK, app_.hotkeysEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    RegisterHotkeys();
    RefreshCameras();
    RefreshVcamStatus();
    // Make sure IXC Camera applies this user's current settings (write only when they differ).
    if (vcam_.comRegistered && profileLoaded_) {
        const ProfileLoadResult published = LoadActiveProfile();
        Profile current = ForPublishing(profile_);
        Validate(current);
        if (!published.ok || !(published.profile == current)) SaveAndPublish();
    }
    RefreshFeatureStates();
    ShowWindow(hwnd_, showCmd);
    // The control panel opens straight to the live preview.
    if (!cameras_.empty()) PostMessageW(hwnd_, kAutoStartMessage, 0, 0);
    // Background update check, at most once a day (offline = silently nothing).
    const double now = static_cast<double>(std::time(nullptr));
    if (now - app_.lastUpdateCheck >= kUpdateCheckInterval || app_.lastUpdateCheck > now) updater_.Check(hwnd_, false);
    RefreshQuickBar();
    SetFocus(startStop_);  // not the profile box (its text would show selected)
    return true;
}

void MainWindow::CreateControls() {
    using namespace app;
    fonts_.Create(dpi_);
    iconHeader_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, Scale(28), Scale(28), LR_DEFAULTCOLOR));
    auto combo = [&](int id) {
        HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 0, 0, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        theme::ApplyDarkControl(h, true);
        return h;
    };
    auto label = [&](int id, DWORD style) {
        return CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | style | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };
    camera_ = combo(kIdCamera);
    format_ = combo(kIdFormat);
    startStop_ = CreateButton(hwnd_, kIdStartStop, L"Start preview", ButtonStyle::Primary, &fonts_, theme::kBg);
    refresh_ = CreateButton(hwnd_, kIdRefresh, L"Refresh", ButtonStyle::Secondary, &fonts_, theme::kBg);
    updatePill_ = CreateButton(hwnd_, kIdUpdatePill, L"Update available", ButtonStyle::Primary, &fonts_, theme::kBg);
    ShowWindow(updatePill_, SW_HIDE);
    for (int i = 0; i < kPanelSections; ++i)
        nav_[i] = CreateButton(hwnd_, kIdNav0 + i, kNavLabels[i], i == 0 ? ButtonStyle::NavSelected : ButtonStyle::Nav, &fonts_, theme::kBg);
    for (int i = 0; i <= kQuickCount; ++i)
        quick_[i] = CreateButton(hwnd_, kIdQuick0 + i, i < kQuickCount ? kQuickLabels[i] : L"Effects", ButtonStyle::Secondary, &fonts_, theme::kBg);
    preview_.Create(hwnd_, instance_, kIdPreview);
    status_ = label(kIdStatus, WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS);
    hint_ = label(kIdHint, SS_LEFT);  // shown only while it has something to say
    perf_ = label(kIdPerf, WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE);
    privacy_ = CreateButton(hwnd_, kIdPrivacy, L"Open camera privacy settings", ButtonStyle::Secondary, &fonts_, theme::kBg);
    ShowWindow(privacy_, SW_HIDE);
    preview_.Clear(L"Choose a camera and select Start preview.");
    panel_.Create(hwnd_, instance_, &profile_, &fonts_, [this] { OnPictureChanged(); });
    profileCombo_ = panel_.profileCombo();
    profileDelete_ = panel_.profileDelete();
    hotkeys_ = panel_.hotkeys();
    vcamStatus_ = panel_.vcamStatus();
    vcamUse_ = panel_.vcamUse();
}

void MainWindow::ApplyFont() {
    fonts_.Create(dpi_);
    for (HWND h : {camera_, format_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
    for (HWND h : {status_, hint_, vcamStatus_, perf_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.caption), TRUE);
    SendMessageW(profileCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.body), TRUE);
    if (iconHeader_) DestroyIcon(iconHeader_);
    iconHeader_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(1), IMAGE_ICON, Scale(28), Scale(28), LR_DEFAULTCOLOR));
}

// Header (brand + IXC Camera status) across the top; the preview and its toolbar/status on the
// left; the scrolling settings column on the right. Everything is recomputed from the client
// size, so no size clips or overlaps: the preview shrinks first, the settings column scrolls.
void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    const int pad = Scale(16), gap = Scale(10), rowH = Scale(34), headerH = Scale(56);
    header_ = {0, 0, w, headerH};

    const int panelW = std::clamp(w * 30 / 100, Scale(320), Scale(420));
    panel_.SetBounds({w - panelW, headerH, w, h});

    // Navigation rail on the left (its version line is painted under the entries).
    const int railW = std::clamp(w * 12 / 100, Scale(128), Scale(176));
    rail_ = {0, headerH, railW, h};
    HDWP dwp = BeginDeferWindowPos(24);
    auto place = [&](HWND hw, int x, int yy, int ww, int hh) {
        if (dwp) dwp = DeferWindowPos(dwp, hw, nullptr, x, yy, std::max(0, ww), std::max(0, hh), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    for (int i = 0; i < app::kPanelSections; ++i) place(nav_[i], Scale(10), headerH + Scale(12) + i * Scale(42), railW - Scale(20), Scale(38));

    // Header: the update notice sits left of the IXC Camera status pill (painted).
    if (GetWindowLongPtrW(updatePill_, GWL_STYLE) & WS_VISIBLE) place(updatePill_, w - Scale(18) - VcamPillWidth() - Scale(10) - Scale(150), (headerH - Scale(30)) / 2, Scale(150), Scale(30));

    const int left = railW + pad, right = w - panelW - Scale(4);
    int y = headerH + Scale(8);
    // Toolbar: camera, format, refresh, start/stop.
    const int buttonW = Scale(150), refreshW = Scale(84);
    const int comboSpace = std::max(Scale(200), right - left - buttonW - refreshW - 3 * gap);
    const int cameraW = comboSpace * 55 / 100;
    RECT cr{};
    GetWindowRect(camera_, &cr);  // a combo box sizes its own edit height; centre it on the row
    const int comboTop = y + std::max(0, (rowH - static_cast<int>(cr.bottom - cr.top)) / 2);
    place(camera_, left, comboTop, cameraW, Scale(400));
    place(format_, left + cameraW + gap, comboTop, comboSpace - cameraW - gap, Scale(400));
    place(refresh_, right - buttonW - gap - refreshW, y, refreshW, rowH);
    place(startStop_, right - buttonW, y, buttonW, rowH);
    y += rowH + gap;

    // Footer: status line, then (only when needed) the hint and the privacy button.
    const int quickH = Scale(34);
    int footer = Scale(28) + quickH + Scale(10);
    wchar_t hintText[512] = L"";
    GetWindowTextW(hint_, hintText, 512);
    int hintH = 0;
    if (hintText[0]) {
        HDC dc = GetDC(hwnd_);
        HGDIOBJ old = SelectObject(dc, fonts_.caption);
        RECT calc{0, 0, right - left - Scale(24), 0};
        DrawTextW(dc, hintText, -1, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        hintH = calc.bottom + Scale(14);
        footer += hintH + Scale(6);
    }
    const bool privacyVisible = IsWindowVisible(privacy_) != FALSE;
    if (privacyVisible) footer += Scale(40);

    const int previewBottom = std::max(y + Scale(120), h - pad - footer);
    previewFrame_ = {left, y, right, previewBottom};
    place(preview_.hwnd(), left + 1, y + 1, right - left - 2, previewBottom - y - 2);
    // Quick bar under the preview: background shortcuts, then the effects switch.
    int fy = previewBottom + Scale(8);
    quickBar_ = {left, fy, right, fy + quickH};
    const int quickLabelW = Scale(92), quickW = std::clamp((right - left - quickLabelW - Scale(110) - 5 * Scale(6)) / kQuickCount, Scale(56), Scale(110));
    for (int i = 0; i < kQuickCount; ++i) place(quick_[i], left + quickLabelW + i * (quickW + Scale(6)), fy, quickW, quickH);
    place(quick_[kQuickCount], right - Scale(110), fy, Scale(110), quickH);
    fy += quickH + Scale(4);
    statusDot_ = {left + Scale(4), fy + Scale(10), left + Scale(12), fy + Scale(18)};
    const int perfW = Scale(230);  // live CPU/RAM, right-aligned on the status row
    place(status_, left + Scale(20), fy, std::max(Scale(80), right - left - Scale(20) - perfW), Scale(28));
    place(perf_, right - perfW, fy, perfW, Scale(28));
    fy += Scale(28);
    if (hintH) {
        place(hint_, left + Scale(12), fy + Scale(7), right - left - Scale(24), hintH - Scale(14));
        fy += hintH + Scale(6);
    }
    ShowWindow(hint_, hintH ? SW_SHOWNA : SW_HIDE);
    if (privacyVisible) place(privacy_, left, fy, Scale(260), Scale(32));
    if (dwp) EndDeferWindowPos(dwp);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int MainWindow::VcamPillWidth() const {
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, fonts_.caption);
    SIZE sz{};
    GetTextExtentPoint32W(dc, vcamPill_.c_str(), static_cast<int>(vcamPill_.size()), &sz);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    return sz.cx + Scale(34);
}

void MainWindow::Paint(HDC dc, const RECT& rc) {
    using namespace app::theme;
    FillRect(dc, &rc, Brush(kBg));
    // Header.
    RECT hdr = header_;
    RECT line{hdr.left, hdr.bottom - 1, hdr.right, hdr.bottom};
    FillRect(dc, &line, Brush(kBorder));
    const int x = Scale(18);
    if (iconHeader_) DrawIconEx(dc, x, (hdr.top + hdr.bottom - Scale(28)) / 2, iconHeader_, Scale(28), Scale(28), 0, nullptr, DI_NORMAL);
    RECT title{x + Scale(38), hdr.top, x + Scale(300), hdr.bottom};
    DrawTextIn(dc, L"IXC Camera", title, fonts_.title, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        // LIVE badge: the camera is streaming into the preview (and so into the shared pipeline).
        HGDIOBJ of = SelectObject(dc, fonts_.title);
        SIZE ts{};
        GetTextExtentPoint32W(dc, L"IXC Camera", 10, &ts);
        SelectObject(dc, of);
        const bool live = previewing_ && session_ && session_->State() == CaptureState::Streaming;
        const int bx = title.left + ts.cx + Scale(14), bh = Scale(22), by = (hdr.top + hdr.bottom - bh) / 2;
        RECT badge{bx, by, bx + Scale(62), by + bh};
        FillRound(dc, badge, bh / 2, live ? Mix(kBg, kBad, 70) : kSurface, live ? kBad : kBorder);
        const int dy = (badge.top + badge.bottom) / 2;
        FillRound(dc, RECT{badge.left + Scale(9), dy - Scale(3), badge.left + Scale(15), dy + Scale(3)}, Scale(3), live ? kBad : kTextFaint, CLR_INVALID);
        RECT bt{badge.left + Scale(19), badge.top, badge.right - Scale(4), badge.bottom};
        DrawTextIn(dc, live ? L"LIVE" : L"OFF", bt, fonts_.caption, live ? kText : kTextFaint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // IXC Camera (system camera) status pill, right-aligned.
    HGDIOBJ old = SelectObject(dc, fonts_.caption);
    SIZE sz{};
    GetTextExtentPoint32W(dc, vcamPill_.c_str(), static_cast<int>(vcamPill_.size()), &sz);
    SelectObject(dc, old);
    const int pillW = sz.cx + Scale(34), pillH = Scale(28);
    RECT pill{hdr.right - Scale(18) - pillW, (hdr.top + hdr.bottom - pillH) / 2, hdr.right - Scale(18), (hdr.top + hdr.bottom + pillH) / 2};
    FillRound(dc, pill, pillH / 2, kSurface, kBorder);
    const COLORREF dot = vcamState_ == VcamState::Active ? kGood : vcamState_ == VcamState::Inactive ? kWarn : kBad;
    const int cy = (pill.top + pill.bottom) / 2;
    FillRound(dc, RECT{pill.left + Scale(12), cy - Scale(4), pill.left + Scale(20), cy + Scale(4)}, Scale(4), dot, CLR_INVALID);
    RECT pt{pill.left + Scale(26), pill.top, pill.right - Scale(8), pill.bottom};
    DrawTextIn(dc, vcamPill_, pt, fonts_.caption, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Navigation rail: separator and the version under the entries.
    RECT sep{rail_.right - 1, rail_.top, rail_.right, rail_.bottom};
    FillRect(dc, &sep, Brush(kBorder));
    RECT ver{rail_.left + Scale(12), rail_.bottom - Scale(54), rail_.right - Scale(8), rail_.bottom - Scale(12)};
    DrawTextIn(dc, L"Version " IXC_VERSION_STRING L"\nLocal processing only", ver, fonts_.caption, kTextFaint, DT_LEFT | DT_BOTTOM | DT_WORDBREAK);
    // Quick bar label.
    RECT ql{quickBar_.left, quickBar_.top, quickBar_.left + Scale(88), quickBar_.bottom};
    DrawTextIn(dc, L"Background", ql, fonts_.caption, kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Preview frame and status dot.
    FillRound(dc, previewFrame_, Scale(6), RGB(0, 0, 0), kBorder);
    const CaptureState st = session_ ? session_->State() : CaptureState::Stopped;
    const COLORREF sd = !previewing_ ? kTextFaint : st == CaptureState::Streaming ? kGood : kWarn;
    FillRound(dc, statusDot_, Scale(4), sd, CLR_INVALID);
    // Hint banner background.
    if (IsWindowVisible(hint_)) {
        RECT hr;
        GetWindowRect(hint_, &hr);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&hr), 2);
        InflateRect(&hr, Scale(12), Scale(7));
        FillRound(dc, hr, Scale(8), Mix(kBg, kWarn, 28), Mix(kBg, kWarn, 90));
    }
}

void MainWindow::SetHint(const std::wstring& text) {
    wchar_t current[512] = L"";
    GetWindowTextW(hint_, current, 512);
    if (text == current) return;
    SetWindowTextW(hint_, text.c_str());
    Layout();
}

// Live state texts of the feature switches: what's actually running, not just what's ticked.
void MainWindow::RefreshFeatureStates() {
    const bool faceEffects = profile_.effectsEnabled && std::any_of(profile_.effects.begin(), profile_.effects.end(), [](const EffectEntry& e) {
                                 const auto* info = effects::Find(e.id);
                                 return info && info->needsFace;
                             });
    std::wstring face;
    const bool neededBy = (faceEffects || profile_.autoFraming) && !profile_.faceTracking.enabled;
    const wchar_t* neededFor = faceEffects ? L"a face effect" : L"auto-framing";
    if (!profile_.faceTracking.enabled && !faceEffects && !profile_.autoFraming) {
        face = L"Off: not running";
    } else if (!previewing_) {
        face = neededBy ? std::wstring(L"Needed by ") + neededFor + L" · runs with the camera" : std::wstring(L"On · runs with the camera");
    } else {
        const face::EngineStatus fs = preview_.FaceStatus();
        wchar_t b[160];
        if (fs.state == face::EngineState::Tracking) {
            swprintf_s(b, L"Tracking %d face%s · %.1f/s · %hs", fs.faces, fs.faces == 1 ? L"" : L"s", fs.detectHz, face::ToString(fs.path));
        } else {
            swprintf_s(b, L"%hs", fs.state == face::EngineState::Searching ? "Searching for a face" : face::ToString(fs.state));
        }
        face = b;
        if (neededBy) face += std::wstring(L" · for ") + neededFor;
    }
    panel_.SetFaceTrackingState(face);
}

LRESULT MainWindow::ControlColor(HWND control, HDC dc) {
    using namespace app::theme;
    SetBkMode(dc, TRANSPARENT);
    COLORREF bg = kBg, text = kTextDim;
    if (GetParent(control) != hwnd_) bg = kSurface;  // inside a settings card
    if (control == hint_) {
        bg = Mix(kBg, kWarn, 28);
        text = kWarn;
    }
    SetTextColor(dc, text);
    SetBkColor(dc, bg);
    return reinterpret_cast<LRESULT>(Brush(bg));
}

// ---- IXC Camera system camera ----------------------------------------------------------------------

void MainWindow::RefreshVcamStatus() {
    vcam_ = vcam::QueryStatus();
    std::wstring text;
    if (!vcam_.comRegistered || !vcam_.dllFileExists) {
        text = L"IXC Camera is not installed. Run the IXC Camera installer to make it available to other apps.";
    } else if (!vcam_.cameraPresent) {
        text = L"IXC Camera is installed but not active. Choose a webcam and select \"Use this webcam for IXC Camera\".";
    } else {
        text = L"IXC Camera is available to other apps";
        text += vcam_.wrappedCameraName.empty() ? L"." : L" and uses " + W(vcam_.wrappedCameraName) + L".";
    }
    SetVcamText(text.c_str());
    if (!vcam_.comRegistered || !vcam_.dllFileExists) {
        vcamState_ = VcamState::Missing;
        vcamPill_ = L"IXC Camera not installed";
    } else if (!vcam_.cameraPresent) {
        vcamState_ = VcamState::Inactive;
        vcamPill_ = L"IXC Camera inactive";
    } else {
        vcamState_ = VcamState::Active;
        vcamPill_ = L"IXC Camera on";
        if (!vcam_.wrappedCameraName.empty()) vcamPill_ += L" · " + W(vcam_.wrappedCameraName);
    }
    panel_.Relayout();  // the status text may wrap differently
    InvalidateRect(hwnd_, &header_, FALSE);
    if (updatePill_ && (GetWindowLongPtrW(updatePill_, GWL_STYLE) & WS_VISIBLE)) Layout();  // it sits next to the status pill
    UpdateVcamControls();
}

void MainWindow::UpdateVcamControls() {
    const int sel = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    bool enable = vcamProcess_ == nullptr && vcam_.comRegistered && vcam_.dllFileExists && sel >= 0 &&
                  static_cast<size_t>(sel) < cameras_.size();
    if (enable) {
        const CameraInfo& c = cameras_[static_cast<size_t>(sel)];
        // Only physical cameras can feed IXC Camera; it's already using this one if they match.
        enable = !c.isSoftwareDevice && !(vcam_.cameraPresent && SameDevice(c.symbolicLink, vcam_.wrappedCameraLink));
    }
    EnableWindow(vcamUse_, enable);
}

void MainWindow::UseSelectedForIxcCamera() {
    const int sel = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<size_t>(sel) >= cameras_.size() || vcamProcess_) return;
    const CameraInfo& cam = cameras_[static_cast<size_t>(sel)];
    const std::filesystem::path tool = std::filesystem::path(vcam_.registeredDllPath).parent_path() / L"ixc_vcam.exe";
    if (!std::filesystem::exists(tool)) {
        SetVcamText(L"The IXC Camera installation is incomplete (ixc_vcam.exe is missing). Reinstall IXC Camera.");
        return;
    }
    // The preview holds the webcam; release it so the change and apps can use it.
    if (previewing_) StopPreview(L"Preview stopped while IXC Camera is being updated.");

    const std::wstring args = L"register --camera \"" + cam.symbolicLink + L"\"";
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = hwnd_;
    sei.lpVerb = L"runas";  // Windows shows its administrator approval prompt
    sei.lpFile = tool.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD err = GetLastError();
        SetVcamText(err == ERROR_CANCELLED ? L"No change made: administrator approval was declined."
                                                           : W(Error{HRESULT_FROM_WIN32(err), "LaunchIxcVcam",
                                                                     "IXC Camera could not start the camera update."}.Describe()).c_str());
        return;
    }
    vcamProcess_ = sei.hProcess;
    SetVcamText((L"Switching IXC Camera to " + W(cam.name) + L"…").c_str());
    UpdateVcamControls();
    // Event-driven: a thread-pool wait fires once when the tool exits (no polling).
    if (!RegisterWaitForSingleObject(&vcamWait_, vcamProcess_, &MainWindow::OnVcamProcessExit, this, INFINITE, WT_EXECUTEONLYONCE)) {
        vcamWait_ = nullptr;
        WaitForSingleObject(vcamProcess_, 30000);
        DWORD code = 1;
        GetExitCodeProcess(vcamProcess_, &code);
        OnVcamDone(code);
    }
}

void CALLBACK MainWindow::OnVcamProcessExit(void* ctx, BOOLEAN) {
    auto* self = static_cast<MainWindow*>(ctx);
    DWORD code = 1;
    GetExitCodeProcess(self->vcamProcess_, &code);
    PostMessageW(self->hwnd_, kVcamDoneMessage, code, 0);
}

void MainWindow::OnVcamDone(DWORD exitCode) {
    if (vcamWait_) {
        UnregisterWaitEx(vcamWait_, nullptr);  // fired once already; don't block
        vcamWait_ = nullptr;
    }
    if (vcamProcess_) {
        CloseHandle(vcamProcess_);
        vcamProcess_ = nullptr;
    }
    RefreshVcamStatus();
    if (exitCode != 0) {
        const std::wstring msg = L"IXC Camera could not switch webcams (ixc_vcam exit code " + std::to_wstring(exitCode) +
                                 L"). Details: %TEMP%\\ixc-install.log or run \"ixc_vcam status\".";
        SetVcamText(msg.c_str());
        log::Error("vcam", "ixc_vcam register failed with exit code " + std::to_string(exitCode));
    } else {
        log::Info("vcam", "IXC Camera now uses " + vcam_.wrappedCameraName);
    }
}

// ---- profile ------------------------------------------------------------------------------------

void MainWindow::LoadProfile() {
    std::string text;
    if (SUCCEEDED(ReadFileLimited(dataDir_ / L"app-settings.json", 16 * 1024, text))) app_ = AppSettingsFromJson(text);
    stem_ = app_.activeProfile;
    if (stem_ != "default" && !std::filesystem::exists(store_.directory() / Utf8ToWide(stem_ + ".json"))) stem_ = "default";
    ProfileLoadResult r = store_.Load(stem_);
    if (r.ok) {
        profile_ = r.profile;
        profileLoaded_ = true;
        for (const auto& w : r.warnings) log::Warn("profiles", stem_ + ": " + w);
        return;
    }
    if (std::filesystem::exists(store_.directory() / Utf8ToWide(stem_ + ".json"))) {
        // Never overwrite a profile we couldn't read. Keep it on disk and run with defaults
        // without saving over it.
        log::Error("profiles", "default profile unreadable, not overwriting: " + r.error);
        SetHint((L"Your saved profile could not be read and was left unchanged: " + W(r.error)).c_str());
        profileLoaded_ = false;
        return;
    }
    profileLoaded_ = true;  // fresh install: defaults, saved on first preview
}

void MainWindow::SaveProfile() {
    if (!profileLoaded_) return;
    std::string stem = stem_;
    const HRESULT hr = store_.Save(profile_, stem);
    if (FAILED(hr)) log::Error("profiles", Error{hr, "SaveProfile", "IXC Camera could not save the profile."}.Describe());
}

// ---- profiles and hotkeys ---------------------------------------------------------------------------

void MainWindow::SaveAppSettings() {
    app_.activeProfile = stem_;
    const HRESULT hr = WriteFileAtomic(dataDir_ / L"app-settings.json", AppSettingsToJson(app_));
    if (FAILED(hr)) log::Warn("app", "could not save app settings: " + HResultHex(hr));
}

void MainWindow::RefreshProfileList() {
    SendMessageW(profileCombo_, CB_RESETCONTENT, 0, 0);
    auto entries = store_.List();
    if (std::none_of(entries.begin(), entries.end(), [&](const auto& e) { return e.stem == stem_; })) entries.push_back({stem_, {}});
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.stem < b.stem; });
    for (const auto& e : entries) {
        const int i = static_cast<int>(SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(W(e.stem).c_str())));
        if (e.stem == stem_) SendMessageW(profileCombo_, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
    }
    EnableWindow(profileDelete_, entries.size() > 1);
}

std::string MainWindow::UniqueStem(std::string_view name) const {
    const std::string base = ProfileStore::MakeStem(name);
    std::string stem = base;
    for (int n = 2; std::filesystem::exists(store_.directory() / Utf8ToWide(stem + ".json")); ++n) stem = base + "-" + std::to_string(n);
    return stem;
}

void MainWindow::SwitchProfile(const std::string& stem) {
    if (stem == stem_) return;
    ProfileLoadResult r = store_.Load(stem);
    if (!r.ok) {
        SetHint((L"Profile \"" + W(stem) + L"\" could not be read and was left unchanged: " + W(r.error)).c_str());
        RefreshProfileList();
        return;
    }
    cycledLens_.clear();  // a different effect list: the lens hotkeys start afresh
    if (sync_.PublishArmed() || sync_.SavePending()) SaveAndPublish();  // keep the old profile's last change
    // The camera and format in use stay as they are; everything else comes from the profile.
    Profile next = r.profile;
    next.sourceCameraId = profile_.sourceCameraId;
    next.width = profile_.width;
    next.height = profile_.height;
    next.fpsNumerator = profile_.fpsNumerator;
    next.fpsDenominator = profile_.fpsDenominator;
    profile_ = std::move(next);
    stem_ = stem;
    SaveAppSettings();
    panel_.Refresh();
    UpdatePipeline();
    RefreshQuickBar();
    SaveAndPublish();
    RefreshProfileList();
    SetHint((L"Profile: " + W(stem_)).c_str());
}

void MainWindow::CycleProfile(int step) {
    auto entries = store_.List();
    if (entries.size() < 2) return;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.stem < b.stem; });
    auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.stem == stem_; });
    const int n = static_cast<int>(entries.size());
    const int cur = it == entries.end() ? 0 : static_cast<int>(it - entries.begin());
    SwitchProfile(entries[static_cast<size_t>(((cur + step) % n + n) % n)].stem);
}

void MainWindow::SaveProfileAs() {
    wchar_t buf[128] = L"";
    GetWindowTextW(profileCombo_, buf, 128);
    const std::string name = WideToUtf8(buf);
    if (!IsValidProfileName(name)) {
        SetHint(L"Type a profile name (up to 64 characters) in the Profile box, then select Save.");
        return;
    }
    const std::string stem = ProfileStore::MakeStem(name);
    if (stem != stem_) {
        stem_ = std::filesystem::exists(store_.directory() / Utf8ToWide(stem + ".json")) ? stem : UniqueStem(name);
        profile_.name = name;
    }
    profileLoaded_ = true;
    SaveProfile();
    SaveAppSettings();
    RefreshProfileList();
    SetHint((L"Saved profile \"" + W(stem_) + L"\".").c_str());
}

void MainWindow::DeleteProfile() {
    auto entries = store_.List();
    if (entries.size() < 2) return;
    const std::wstring msg = L"Delete the profile \"" + W(stem_) + L"\"? This can't be undone.";
    if (MessageBoxW(hwnd_, msg.c_str(), L"IXC Camera", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const std::string doomed = stem_;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.stem < b.stem; });
    const auto next = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.stem != doomed; });
    SwitchProfile(next->stem);
    if (FAILED(store_.Remove(doomed))) SetHint(L"The profile file could not be deleted.");
    RefreshProfileList();
}

void MainWindow::ImportProfile() {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"IXC profile (*.json)\0*.json\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string text;
    HRESULT hr = ReadFileLimited(path, ProfileStore::kMaxProfileFileBytes, text);
    ProfileLoadResult r = SUCCEEDED(hr) ? ProfileFromJson(text) : ProfileLoadResult{};
    if (!r.ok) {
        const std::wstring why = FAILED(hr) ? W(Error{hr, "ImportProfile", ""}.Describe()) : W(r.error);
        MessageBoxW(hwnd_, (L"This file isn't a valid IXC profile and wasn't imported.\n\n" + why).c_str(), L"IXC Camera", MB_OK | MB_ICONWARNING);
        return;
    }
    std::string stem = UniqueStem(r.profile.name);  // never overwrites an existing profile
    hr = store_.Save(r.profile, stem);
    if (FAILED(hr)) {
        SetHint(L"The imported profile could not be saved.");
        return;
    }
    SwitchProfile(stem);
    if (!r.warnings.empty()) {
        SetHint((L"Imported \"" + W(stem) + L"\" with " + std::to_wstring(r.warnings.size()) +
                               L" value(s) adjusted to valid ranges.").c_str());
    }
}

void MainWindow::ExportProfile() {
    std::wstring name = W(stem_) + L".json";
    wchar_t path[MAX_PATH];
    wcsncpy_s(path, name.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"IXC profile (*.json)\0*.json\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return;
    const HRESULT hr = WriteFileAtomic(path, ProfileToJson(profile_));
    SetHint(SUCCEEDED(hr) ? L"Profile exported." : L"The profile could not be exported.");
}

// Global hotkeys (RegisterHotKey: no keyboard hook, no polling). Ctrl+Alt combinations so plain
// F-keys stay free for games; they work while the IXC app runs, even minimized.
enum HotkeyId { kHkEffects = 1, kHkNextProfile, kHkPrevProfile, kHkMirror, kHkNextLens, kHkPrevLens };

void MainWindow::RegisterHotkeys() {
    UnregisterHotkeys();
    if (!app_.hotkeysEnabled) return;
    const struct { int id; UINT vk; const wchar_t* name; } keys[] = {
        {kHkEffects, VK_F8, L"Ctrl+Alt+F8"}, {kHkNextProfile, VK_F9, L"Ctrl+Alt+F9"},
        {kHkPrevProfile, VK_F10, L"Ctrl+Alt+F10"}, {kHkMirror, VK_F11, L"Ctrl+Alt+F11"},
        {kHkNextLens, VK_F7, L"Ctrl+Alt+F7"}, {kHkPrevLens, VK_F6, L"Ctrl+Alt+F6"}};
    std::wstring taken;
    for (const auto& k : keys) {
        if (!RegisterHotKey(hwnd_, k.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, k.vk)) taken += (taken.empty() ? L"" : L", ") + std::wstring(k.name);
    }
    hotkeysRegistered_ = true;
    if (!taken.empty()) SetHint((L"Another app already uses " + taken + L"; those IXC hotkeys are unavailable.").c_str());
}

void MainWindow::UnregisterHotkeys() {
    if (!hotkeysRegistered_) return;
    for (int id = kHkEffects; id <= kHkPrevLens; ++id) UnregisterHotKey(hwnd_, id);
    hotkeysRegistered_ = false;
}

void MainWindow::OnHotkey(int id) {
    switch (id) {
        case kHkEffects:
            profile_.effectsEnabled = !profile_.effectsEnabled;
            SetHint(profile_.effectsEnabled ? L"Effects on (Ctrl+Alt+F8)" : L"Effects off (Ctrl+Alt+F8)");
            panel_.Refresh();  // the switch follows the hotkey
            break;
        case kHkNextProfile: CycleProfile(1); return;
        case kHkPrevProfile: CycleProfile(-1); return;
        case kHkMirror:
            profile_.mirror = !profile_.mirror;
            panel_.Refresh();
            break;
        case kHkNextLens:
        case kHkPrevLens: {
            // Snap-style lens cycling. Only the lens these hotkeys added is replaced: effects
            // switched on by hand stay as they are (and are skipped by the cycle).
            auto& fx = profile_.effects;
            const std::string next = effects::NextLens(cycledLens_, id == kHkNextLens ? 1 : -1, fx);
            fx.erase(std::remove_if(fx.begin(), fx.end(), [&](const EffectEntry& e) { return !cycledLens_.empty() && e.id == cycledLens_; }),
                     fx.end());
            cycledLens_ = next;
            if (!next.empty() && fx.size() < kMaxEnabledEffects) {
                fx.push_back({next, 70});
                profile_.effectsEnabled = true;
            }
            const effects::EffectInfo* info = effects::Find(next);
            SetHint((std::wstring(L"Lens: ") + (info ? info->name : L"none") + L"  (Ctrl+Alt+F7 next, Ctrl+Alt+F6 previous)").c_str());
            panel_.Refresh();
            break;
        }
        default: return;
    }
    UpdatePipeline();
    SaveAndPublish();
}

// ---- cameras and formats ------------------------------------------------------------------------

void MainWindow::RefreshCameras() {
    std::wstring current;
    const int sel = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    if (sel >= 0 && static_cast<size_t>(sel) < cameras_.size()) current = cameras_[static_cast<size_t>(sel)].symbolicLink;
    if (current.empty()) current = Utf8ToWide(profile_.sourceCameraId);

    std::vector<CameraInfo> all;
    const HRESULT hr = EnumerateCameras(all);
    if (FAILED(hr)) {
        SetWindowTextW(status_, W(Error{hr, "EnumerateCameras", "IXC Camera could not list cameras."}.Describe()).c_str());
    }
    // Physical cameras first; virtual cameras (e.g. other apps' software cameras) after.
    cameras_.clear();
    for (const auto& c : all) if (!c.isSoftwareDevice) cameras_.push_back(c);
    for (const auto& c : all) if (c.isSoftwareDevice) cameras_.push_back(c);

    SendMessageW(camera_, CB_RESETCONTENT, 0, 0);
    int select = cameras_.empty() ? -1 : 0;
    for (size_t i = 0; i < cameras_.size(); ++i) {
        std::wstring label = W(cameras_[i].name);
        if (cameras_[i].isSoftwareDevice) label += L"  (virtual camera)";
        SendMessageW(camera_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (!current.empty() && SameDevice(cameras_[i].symbolicLink, current)) select = static_cast<int>(i);
    }
    if (cameras_.empty()) {
        SendMessageW(format_, CB_RESETCONTENT, 0, 0);
        EnableWindow(startStop_, FALSE);
        if (!previewing_) preview_.Clear(L"No camera found. Connect a webcam; it will appear here automatically.");
        return;
    }
    EnableWindow(startStop_, TRUE);
    const bool changed = select < 0 || static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0)) != select || formats_.empty();
    SendMessageW(camera_, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
    if (changed && !previewing_) OnCameraSelected();
}

void MainWindow::OnCameraSelected() {
    const int sel = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    SendMessageW(format_, CB_RESETCONTENT, 0, 0);
    formats_.clear();
    if (sel < 0) return;
    const CameraInfo& cam = cameras_[static_cast<size_t>(sel)];

    std::vector<CaptureFormat> raw;
    const HRESULT hr = EnumerateFormats(cam.symbolicLink, raw);
    if (FAILED(hr)) {
        const Error e{hr, "EnumerateFormats", "IXC Camera could not read the formats of " + cam.name + "."};
        SetWindowTextW(status_, W(e.Describe()).c_str());
        ShowPrivacyHelp(ClassifyHResult(hr) == ErrorClass::AccessDenied);
        return;
    }
    ShowPrivacyHelp(false);
    formats_ = NormalizeFormats(raw);

    const auto autoPick = SelectFormat(formats_, RequestForTier(profile_.tier));
    std::wstring autoLabel = L"Auto (best for this camera)";
    if (autoPick) autoLabel += L": " + W(Describe(formats_[*autoPick]));
    SendMessageW(format_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(autoLabel.c_str()));
    for (const auto& f : formats_) {
        const std::wstring label = W(Describe(f));
        SendMessageW(format_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }

    // Restore the profile's saved size/rate on the same camera; otherwise Auto.
    int select = 0;
    if (SameDevice(Utf8ToWide(profile_.sourceCameraId), cam.symbolicLink)) {
        const FormatRequest saved{profile_.width, profile_.height,
                                  static_cast<double>(profile_.fpsNumerator) / profile_.fpsDenominator};
        if (auto i = SelectFormat(formats_, saved)) {
            const auto& f = formats_[*i];
            if (f.width == profile_.width && f.height == profile_.height && autoPick &&
                !formats_[*autoPick].SameMode(f)) {
                select = static_cast<int>(*i) + 1;
            }
        }
    }
    SendMessageW(format_, CB_SETCURSEL, static_cast<WPARAM>(select), 0);
    SetWindowTextW(status_, (W(cam.name) + L" • " + std::to_wstring(formats_.size()) + L" native modes").c_str());
}

// ---- preview ------------------------------------------------------------------------------------

void MainWindow::StartPreview() {
    const int camSel = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    const int fmtSel = static_cast<int>(SendMessageW(format_, CB_GETCURSEL, 0, 0));
    if (camSel < 0 || formats_.empty()) return;

    std::optional<size_t> idx;
    if (fmtSel <= 0) idx = SelectFormat(formats_, RequestForTier(profile_.tier));
    else idx = static_cast<size_t>(fmtSel - 1);
    if (!idx || *idx >= formats_.size()) return;

    const CameraInfo& cam = cameras_[static_cast<size_t>(camSel)];
    CaptureConfig cfg;
    cfg.symbolicLink = cam.symbolicLink;
    cfg.cameraName = cam.name;
    cfg.format = formats_[*idx];
    cfg.output = OutputFormat::Nv12;  // pipeline format; the preview converts only displayed pixels
    cfg.smoothMotion = profile_.smoothMotion;
    session_->SetAntiFlicker(ResolveAntiFlicker(profile_.antiFlicker, UserRegion()));

    SetWindowTextW(status_, (L"Opening " + W(cam.name) + L"…").c_str());
    const Error err = session_->Start(cfg);
    if (FAILED(err.hr)) {
        const bool denied = ClassifyHResult(err.hr) == ErrorClass::AccessDenied;
        preview_.Clear(denied ? L"Windows is blocking camera access for desktop apps."
                              : L"The camera could not be started. See the message below.");
        SetWindowTextW(status_, W(err.Describe()).c_str());
        if (denied) {
            SetHint(L"Turn on \"Camera access\" and \"Let desktop apps access your camera\" in Windows "
                                  L"Settings, then select Start preview again.");
        } else if (ClassifyHResult(err.hr) == ErrorClass::Transient) {
            SetHint(L"Another app may be using the camera exclusively. Close it and try again.");
        }
        ShowPrivacyHelp(denied);
        return;
    }

    previewing_ = true;
    ShowPrivacyHelp(false);
    SetHint(L"");
    SetWindowTextW(startStop_, L"Stop preview");
    SetTimer(hwnd_, kStatusTimer, 1000, nullptr);
    InvalidateRect(hwnd_, &statusDot_, FALSE);

    const CaptureFormat active = session_->ActiveFormat();
    profile_.sourceCameraId = WideToUtf8(cam.symbolicLink);
    profile_.width = active.width;
    profile_.height = active.height;
    profile_.fpsNumerator = active.fpsNumerator;
    profile_.fpsDenominator = active.fpsDenominator;
    SaveProfile();
    UpdatePipeline();
    UpdateStatus();
}

// ---- picture settings -------------------------------------------------------------------------------

void MainWindow::OnPictureChanged() {
    if (session_) {
        session_->SetSmoothMotion(profile_.smoothMotion);
        session_->SetAntiFlicker(ResolveAntiFlicker(profile_.antiFlicker, UserRegion()));
    }
    UpdatePipeline();                            // preview reflects the change immediately
    if (panel_.FaceOverlay() != app_.showFaceMarkers) {  // app-level preference (not part of a profile)
        app_.showFaceMarkers = panel_.FaceOverlay();
        SaveAppSettings();
    }
    RefreshFeatureStates();
    RefreshQuickBar();
    // IXC Camera follows the change live (coalesced to ~25 updates/s while dragging); the profile
    // file is written once the control settles.
    if (sync_.OnChange()) SetTimer(hwnd_, kPublishTimer, SettingsSync::kPublishMs, nullptr);
    SetTimer(hwnd_, kSaveTimer, SettingsSync::kSaveMs, nullptr);
}

void MainWindow::PublishNow() {
    KillTimer(hwnd_, kPublishTimer);
    sync_.OnPublished();
    // Publish for the IXC Camera source (inside the Windows camera service). Apps using IXC
    // Camera pick the change up on their next frame.
    if (!vcam_.comRegistered) return;
    const HRESULT hr = PublishActiveProfile(ForPublishing(profile_));
    if (FAILED(hr) && !publishWarned_) {
        publishWarned_ = true;
        const Error e{hr, "PublishActiveProfile", "IXC Camera could not share these settings with the system camera."};
        log::Error("profiles", e.Describe());
        SetVcamText((W(e.Describe()) + L" Reinstalling IXC Camera repairs the settings folder.").c_str());
    }
}

// Once a second: the status-bar readout, and the diagnostics view when it's open.
void MainWindow::UpdatePerf() {
    const bool diag = panel_.Diagnostics();
    perfMonitor_.EnableGpuCounters(diag);
    const app::PerfSample s = perfMonitor_.Sample();
    SetWindowTextW(perf_, app::PerfMonitor::Format(s).c_str());
    if (!diag) return;
    std::wstring t;
    wchar_t b[256];
    if (previewing_ && session_) {
        const FrameStatsSnapshot fs = session_->Stats();
        const FrameLayout l = session_->Layout();
        swprintf_s(b, L"Preview: %ux%u · %.1f FPS (camera mode %.0f) · %llu dropped\n", l.width, l.height, fs.fps, fs.nominalFps,
                   static_cast<unsigned long long>(session_->DroppedFrames()));
        t += b;
    } else {
        t += L"Preview: stopped\n";
    }
    swprintf_s(b, L"App: CPU %.1f%% · RAM %.0f MB", s.cpuPercent, s.ramMB);
    t += b;
    if (s.gpuPercent >= 0) {
        swprintf_s(b, L" · GPU %.0f%%", s.gpuPercent);
        t += b;
    }
    if (s.vramMB >= 0) {
        swprintf_s(b, L" · VRAM %.0f MB", s.vramMB);
        t += b;
    }
    t += L"\n";
    const auto ps = preview_.PipelineStats();
    const wchar_t* mode = profile_.processing == ProcessingMode::Gpu ? L"GPU" : profile_.processing == ProcessingMode::Cpu ? L"CPU" : L"Auto";
    swprintf_s(b, L"Processing %s · picture on %s (CPU %.1f ms, GPU %.1f ms)%s%s\n", mode, ps.backend == processing::Backend::Gpu ? L"GPU" : L"CPU",
               ps.cpuAvgMs, ps.gpuAvgMs, ps.adapter.empty() ? L"" : L" · ", ps.adapter.c_str());
    t += b;
    const seg::SegStatus ss = preview_.SegmentationStatus();
    if (ss.state == seg::SegState::Off) {
        t += L"Segmentation: off (no background effect)";
    } else {
        swprintf_s(b, L"Segmentation: %hs on %hs (%s) · %.1f ms network · %.1f masks/s · %.1f MB", seg::ToString(ss.state), seg::ToString(ss.backend),
                   Utf8ToWide(ss.device).c_str(), ss.avgNetMs, ss.masksPerSecond, static_cast<double>(ss.memoryBytes) / 1048576.0);
        t += b;
        if (!ss.gpuNote.empty()) t += L"\n  " + Utf8ToWide(ss.gpuNote);
    }
    static const wchar_t* const kModes[] = {L"Original", L"Blur", L"Replace", L"Colour", L"Custom"};
    swprintf_s(b, L"\nBackground: %s · face tracking: %hs", kModes[static_cast<int>(profile_.background.mode)], face::ToString(preview_.FaceStatus().state));
    t += b;
    t += L"\nIXC Camera in other apps runs inside the Windows camera service (see scripts/trace-vcam.ps1).";
    panel_.SetDiagnosticsText(t);
}

void MainWindow::UpdatePipeline() {
    if (!previewing_ || !session_) {
        preview_.SetPipeline(nullptr);
        return;
    }
    const FrameLayout l = session_->Layout();
    const bool fullRange = l.nominalRange == MFNominalRange_0_255;
    // Same as the IXC Camera source: Smooth motion's brightness gain adds to the user's exposure
    // (and raises temporal denoise to match).
    smoothEv_ = session_->SmoothCompensationEv();
    const Profile effective = processing::WithSmoothMotionGain(profile_, smoothEv_);
    preview_.SetPipeline(std::make_shared<const processing::PipelineParams>(processing::CompileParams(effective, l.width, l.height, fullRange)));
    auto fx = effects::CompileEffects(effective, fullRange, bgSource_.Resolve(profile_.background, BackgroundsDirectory()));
    const bool effectsNeedFaces = fx->needsFaces;
    preview_.SetEffects(fx->Active() ? std::move(fx) : nullptr);
    // Face tracking follows the profile, or face-aware effects (off = no thread, no memory).
    preview_.SetAutoFraming(profile_.autoFraming);
    preview_.SetProcessingMode(profile_.processing, static_cast<float>(profile_.background.temporal / 100.0),
                               static_cast<float>(profile_.background.hair / 100.0));
    if (profile_.faceTracking.enabled || profile_.autoFraming || effectsNeedFaces) {
        const face::EngineConfig fc = face::EngineConfigFor(profile_);
        preview_.SetFaceTracking(&fc);
    } else {
        preview_.SetFaceTracking(nullptr);
    }
    preview_.SetFaceOverlay(panel_.FaceOverlay());
}

void MainWindow::SaveAndPublish() {
    KillTimer(hwnd_, kSaveTimer);
    sync_.OnSaved();
    SaveProfile();
    PublishNow();
}

void MainWindow::StopPreview(const wchar_t* placeholder) {
    KillTimer(hwnd_, kStatusTimer);
    if (session_) session_->Stop();  // releases the physical camera
    previewing_ = false;
    preview_.Clear(placeholder);
    SetWindowTextW(startStop_, L"Start preview");
    SetWindowTextW(status_, L"Preview stopped  ·  the camera is released");
    UpdateStatus();
    SetHint(L"");
}

void MainWindow::UpdateStatus() {
    RefreshFeatureStates();
    InvalidateRect(hwnd_, &statusDot_, FALSE);
    InvalidateRect(hwnd_, &header_, FALSE);  // LIVE badge
    if (!previewing_) return;
    const auto st = session_->Stats();
    const auto fmt = session_->ActiveFormat();
    const int cam = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    const std::wstring name = cam >= 0 ? W(cameras_[static_cast<size_t>(cam)].name) : L"";
    // One line: what's live, how well, and what's processing it.
    std::wstring s = L"Live  ·  " + name + L"  ·  " + W(Describe(fmt));
    wchar_t b[160];
    swprintf_s(b, L"  ·  receiving %.1f FPS", st.fps);
    s += b;
    if (st.meanLatencyMs >= 0) {
        swprintf_s(b, L"  ·  latency %.0f ms", st.meanLatencyMs);
        s += b;
    }
    swprintf_s(b, L"  ·  dropped %llu  ·  %s processing", static_cast<unsigned long long>(session_->DroppedFrames()),
               preview_.ProcessingBackend().c_str());
    s += b;
    const face::EngineStatus fs = preview_.FaceStatus();
    if (fs.state == face::EngineState::Tracking || fs.state == face::EngineState::Searching) {
        swprintf_s(b, L"  ·  face: %hs %d", face::ToString(fs.state), fs.faces);
        s += b;
    }
    const seg::SegStatus ss = preview_.SegmentationStatus();
    if (ss.state == seg::SegState::Running && ss.masks > 0) {
        swprintf_s(b, L"  ·  background: %.0f/s, %.0f ms", ss.masksPerSecond, ss.avgRunMs);
        s += b;
    } else if (ss.state == seg::SegState::TooSlow) {
        s += L"  ·  background: off (CPU too slow)";
    }
    SetWindowTextW(status_, s.c_str());

    if (st.underSpeed) {
        wchar_t hb[400];
        swprintf_s(hb, L"The camera is delivering %.0f of %.0f FPS: usually low light (the camera lengthens its exposure). "
                      L"More light, or Smooth motion, restores the full frame rate.", st.fps, st.nominalFps);
        SetHint(hb);
    } else if (session_->State() == CaptureState::Streaming) {
        SetHint(L"");
    }
}

void MainWindow::OnCaptureState(CaptureState s) {
    if (!previewing_) return;
    switch (s) {
        case CaptureState::Reconnecting:
            preview_.Clear(L"Camera disconnected. Reconnecting…");
            SetHint(L"IXC Camera will reconnect automatically when the camera is available again.");
            break;
        case CaptureState::WaitingForDevice:
            preview_.Clear(L"Waiting for the camera to be connected again.");
            SetHint(L"Plug the camera back in. IXC Camera resumes automatically and uses no CPU while waiting.");
            break;
        case CaptureState::Streaming:
            SetHint(L"");
            break;
        case CaptureState::Failed: {
            const Error e = session_->LastError();
            const bool denied = ClassifyHResult(e.hr) == ErrorClass::AccessDenied;
            StopPreview(denied ? L"Windows is blocking camera access for desktop apps." : L"The camera stopped.");
            SetWindowTextW(status_, W(e.Describe()).c_str());
            ShowPrivacyHelp(denied);
            break;
        }
        case CaptureState::Starting:
        case CaptureState::Stopped:
            break;
    }
}

void MainWindow::ShowPrivacyHelp(bool show) {
    ShowWindow(privacy_, show ? SW_SHOW : SW_HIDE);
    Layout();
}

// ---- window procedure -----------------------------------------------------------------------------

// ---- Navigation, quick bar ------------------------------------------------------------------------

void MainWindow::SelectSection(PanelSection s) {
    for (int i = 0; i < app::kPanelSections; ++i)
        app::SetButtonStyle(nav_[i], i == static_cast<int>(s) ? app::ButtonStyle::NavSelected : app::ButtonStyle::Nav);
    panel_.SetSection(s);
}

void MainWindow::RefreshQuickBar() {
    for (int i = 0; i < kQuickCount; ++i)
        app::SetButtonStyle(quick_[i], profile_.background.mode == kQuickModes[i] ? app::ButtonStyle::Primary : app::ButtonStyle::Secondary);
    app::SetButtonStyle(quick_[kQuickCount], profile_.effectsEnabled ? app::ButtonStyle::Primary : app::ButtonStyle::Secondary);
    SetWindowTextW(quick_[kQuickCount], profile_.effectsEnabled ? L"Effects on" : L"Effects off");
}

// The quick bar edits the same profile as the inspector (and so the preview and IXC Camera).
void MainWindow::OnQuick(int index) {
    if (index == kQuickCount) {
        profile_.effectsEnabled = !profile_.effectsEnabled;
    } else {
        const BackgroundMode mode = kQuickModes[index];
        if (mode == BackgroundMode::Replace && profile_.background.builtin.empty()) {
            const auto& builtins = effects::BuiltinBackgrounds();
            if (!builtins.empty()) profile_.background.builtin = builtins.front().id;
        }
        profile_.background.mode = mode;
    }
    panel_.Refresh();
    OnPictureChanged();
}

// ---- Updates ---------------------------------------------------------------------------------------

void MainWindow::OnUpdateChecked(const app::UpdateCheckResult& r) {
    if (!r.reached) {
        // Offline, blocked or rate-limited: an automatic check stays silent.
        panel_.SetUpdateState(r.manual ? L"Couldn't reach GitHub: " + r.error : std::wstring(L"IXC Camera " IXC_VERSION_STRING), update_.ok,
                              update_.ok, false);
        return;
    }
    app_.lastUpdateCheck = static_cast<double>(std::time(nullptr));
    SaveAppSettings();
    const SemVer current = ParseSemVer(IXC_VERSION_STRING);
    if (!r.release.ok) {
        log::Warn("update", "release information rejected: " + r.release.error);
        if (r.manual) panel_.SetUpdateState(L"The latest release couldn't be used: " + W(r.release.error), false, false, false);
        return;
    }
    if (CompareSemVer(ParseSemVer(r.release.version), current) <= 0) {
        update_ = {};
        ShowWindow(updatePill_, SW_HIDE);
        panel_.SetUpdateState(L"IXC Camera " IXC_VERSION_STRING L" is up to date.", false, false, false);
        return;
    }
    update_ = r.release;
    const bool skipped = !r.manual && r.release.version == app_.skippedVersion;
    std::wstring text = L"IXC Camera " + W(update_.version) + L" is available (you have " IXC_VERSION_STRING L").";
    if (!update_.notes.empty()) text += L"\n\n" + W(update_.notes);
    panel_.SetUpdateState(text, true, true, false);
    ShowWindow(updatePill_, skipped ? SW_HIDE : SW_SHOWNA);
    Layout();
}

void MainWindow::InstallUpdate() {
    if (!update_.ok || updater_.Busy()) return;
    const std::wstring ask = L"Download IXC Camera " + W(update_.version) +
                             L" from the official GitHub release and install it?\n\nThe installer is checked against the release's "
                             L"SHA-256 checksum before it runs. IXC Camera closes while the installer runs.";
    if (MessageBoxW(hwnd_, ask.c_str(), L"Update IXC Camera", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    panel_.SetUpdateState(L"Downloading IXC Camera " + W(update_.version) + L"…", false, false, true);
    updater_.Download(hwnd_, update_);
}

void MainWindow::OnUpdateDownloaded(const app::UpdateDownloadResult& r) {
    if (!r.ok) {
        log::Warn("update", "download failed: " + WideToUtf8(r.error));
        panel_.SetUpdateState(L"The update couldn't be downloaded: " + r.error, update_.ok, update_.ok, false);
        return;
    }
    // Hash it once more right before starting it (nothing may have changed the file since).
    if (!app::Updater::VerifyFile(r.installer, r.sha256)) {
        panel_.SetUpdateState(L"The downloaded installer failed verification and was not started.", update_.ok, update_.ok, false);
        return;
    }
    log::Info("update", "starting verified installer for " + r.version);
    if (sync_.PublishArmed() || sync_.SavePending()) SaveAndPublish();
    const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", r.installer.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        panel_.SetUpdateState(L"The installer couldn't be started (cancelled?).", update_.ok, update_.ok, false);
        return;
    }
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);  // the installer replaces the app; it starts again afterwards
}

void MainWindow::SkipUpdate() {
    if (!update_.ok) return;
    app_.skippedVersion = update_.version;
    SaveAppSettings();
    ShowWindow(updatePill_, SW_HIDE);
    panel_.SetUpdateState(L"Version " + W(update_.version) + L" skipped. Check again to see it.", false, false, false);
    Layout();
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            dpi_ = GetDpiForWindow(hwnd_);
            CreateControls();
            SetTimer(hwnd_, kPerfTimer, 1000, nullptr);  // live CPU/RAM readout at the bottom
            ApplyFont();
            return 0;

        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(fonts_.body);

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            Paint(mem, rc);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, mem,
                   ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case kAutoStartMessage:
            if (!previewing_ && !IsIconic(hwnd_)) StartPreview();
            return 0;

        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                // Release the camera while nobody can see the preview.
                if (previewing_) {
                    resumeOnRestore_ = true;
                    StopPreview(L"");
                }
            } else {
                Layout();
                if (resumeOnRestore_) {
                    resumeOnRestore_ = false;
                    StartPreview();
                }
            }
            return 0;

        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            ApplyFont();
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            panel_.Relayout();  // new font metrics even if the panel's rectangle didn't change
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {Scale(1000), Scale(600)};  // preview + settings column; the column scrolls
            return 0;
        }

        case WM_HSCROLL:
            if (lp && panel_.OnScroll(reinterpret_cast<HWND>(lp))) return 0;
            break;

        case WM_COMMAND:
            if (lp && panel_.OnCommand(reinterpret_cast<HWND>(lp), HIWORD(wp))) return 0;
            switch (LOWORD(wp)) {
                case kIdCamera:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        // Switching while previewing restarts the preview on the new camera.
                        const bool wasPreviewing = previewing_;
                        if (wasPreviewing) StopPreview(L"Switching camera…");
                        OnCameraSelected();
                        UpdateVcamControls();
                        if (wasPreviewing) StartPreview();
                    }
                    return 0;
                case kIdFormat:
                    if (HIWORD(wp) == CBN_SELCHANGE && previewing_) {
                        StopPreview(L"Switching format…");
                        StartPreview();
                    }
                    return 0;
                case kIdStartStop:
                    if (previewing_) StopPreview(L"Preview stopped. The camera is released.");
                    else StartPreview();
                    return 0;
                case kIdVcamUse:
                    UseSelectedForIxcCamera();
                    return 0;
                case kIdProfileCombo:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        const int sel = static_cast<int>(SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0));
                        wchar_t item[128] = L"";
                        if (sel >= 0 && SendMessageW(profileCombo_, CB_GETLBTEXTLEN, static_cast<WPARAM>(sel), 0) < 128) {
                            SendMessageW(profileCombo_, CB_GETLBTEXT, static_cast<WPARAM>(sel), reinterpret_cast<LPARAM>(item));
                            SwitchProfile(WideToUtf8(item));
                        }
                    }
                    return 0;
                case kIdProfileSave: SaveProfileAs(); return 0;
                case kIdProfileDelete: DeleteProfile(); return 0;
                case kIdProfileImport: ImportProfile(); return 0;
                case kIdProfileExport: ExportProfile(); return 0;
                case kIdHotkeys:
                    app_.hotkeysEnabled = SendMessageW(hotkeys_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    SaveAppSettings();
                    RegisterHotkeys();
                    if (!app_.hotkeysEnabled) UnregisterHotkeys();
                    return 0;
                case kIdPrivacy:
                    ShellExecuteW(hwnd_, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                case kIdRefresh:
                    RefreshCameras();
                    RefreshVcamStatus();
                    return 0;
                case kIdUpdatePill:
                    SelectSection(PanelSection::Updates);
                    return 0;
                case app::kIdUpdateCheck:
                    if (!updater_.Busy()) {
                        panel_.SetUpdateState(L"Checking GitHub for a newer release…", false, false, true);
                        updater_.Check(hwnd_, true);
                    }
                    return 0;
                case app::kIdUpdateInstall: InstallUpdate(); return 0;
                case app::kIdUpdateSkip: SkipUpdate(); return 0;
                default:
                    if (LOWORD(wp) >= kIdNav0 && LOWORD(wp) < kIdNav0 + app::kPanelSections) {
                        SelectSection(static_cast<PanelSection>(LOWORD(wp) - kIdNav0));
                        return 0;
                    }
                    if (LOWORD(wp) >= kIdQuick0 && LOWORD(wp) <= kIdQuick0 + kQuickCount) {
                        OnQuick(LOWORD(wp) - kIdQuick0);
                        return 0;
                    }
                    break;
            }
            break;

        case app::PreviewWindow::kFrameMessage: {
            preview_.FrameMessageHandled();
            if (previewing_ && !IsIconic(hwnd_)) {
                if (auto frame = session_->TakeFrame()) {
                    if (session_->SmoothCompensationEv() != smoothEv_) UpdatePipeline();  // Smooth motion gain changed
                    const FrameLayout l = session_->Layout();
                    preview_.ShowFrame(std::move(*frame), l);
                }
            }
            return 0;
        }

        case WM_HOTKEY:
            OnHotkey(static_cast<int>(wp));
            return 0;

        case kVcamDoneMessage:
            OnVcamDone(static_cast<DWORD>(wp));
            return 0;

        case app::Updater::kCheckedMessage: {
            std::unique_ptr<app::UpdateCheckResult> r(reinterpret_cast<app::UpdateCheckResult*>(lp));
            if (r) OnUpdateChecked(*r);
            return 0;
        }
        case app::Updater::kDownloadedMessage: {
            std::unique_ptr<app::UpdateDownloadResult> r(reinterpret_cast<app::UpdateDownloadResult*>(lp));
            if (r) OnUpdateDownloaded(*r);
            return 0;
        }

        case kStateMessage:
            OnCaptureState(static_cast<CaptureState>(wp));
            return 0;

        case WM_TIMER:
            if (wp == kStatusTimer) UpdateStatus();
            if (wp == kPerfTimer) UpdatePerf();
            else if (wp == kPublishTimer) PublishNow();
            else if (wp == kSaveTimer) {
                KillTimer(hwnd_, kSaveTimer);
                sync_.OnSaved();
                SaveProfile();
            }
            else if (wp == kDeviceRefreshTimer) {
                KillTimer(hwnd_, kDeviceRefreshTimer);
                RefreshCameras();
                RefreshVcamStatus();  // IXC Camera itself appears/disappears as a device
            }
            return 0;

        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) {
                SetTimer(hwnd_, kDeviceRefreshTimer, 700, nullptr);  // coalesce bursts of notifications
            }
            return TRUE;

        case WM_CTLCOLORSTATIC:
            return ControlColor(reinterpret_cast<HWND>(lp), reinterpret_cast<HDC>(wp));

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, app::theme::kText);
            SetBkColor(dc, app::theme::kSurfaceHi);
            return reinterpret_cast<LRESULT>(app::theme::Brush(app::theme::kSurfaceHi));
        }

        case WM_DESTROY:
            KillTimer(hwnd_, kStatusTimer);
            KillTimer(hwnd_, kPerfTimer);
            UnregisterHotkeys();
            if (sync_.PublishArmed() || sync_.SavePending()) SaveAndPublish();  // don't lose the last slider move
            if (session_) {
                session_->Close();
                session_.Reset();
            }
            if (devNotify_) UnregisterDeviceNotification(devNotify_);
            if (vcamWait_) UnregisterWaitEx(vcamWait_, INVALID_HANDLE_VALUE);  // waits for an in-flight callback
            if (vcamProcess_) CloseHandle(vcamProcess_);
            for (HICON i : {iconHeader_, iconSmall_, iconLarge_}) if (i) DestroyIcon(i);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
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
        log::Config cfg;
        cfg.directory = root / L"logs";
        log::Init(cfg);
    }
    log::Info("app", std::string("IXC Camera ") + IXC_VERSION_STRING + " starting");

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    int rc = 1;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        MediaFoundationScope mf;
        if (FAILED(mf.hr())) {
            const Error e{mf.hr(), "MFStartup", "Windows Media Foundation is not available on this PC."};
            log::Error("app", e.Describe());
            MessageBoxW(nullptr, Utf8ToWide(e.Describe()).c_str(), L"IXC Camera", MB_ICONERROR);
        } else {
            auto window = std::make_unique<MainWindow>(instance, root);
            if (window->Create(showCmd)) {
                MSG msg;
                while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    HWND top = GetAncestor(msg.hwnd, GA_ROOT);
                    if (top && IsDialogMessageW(top, &msg)) continue;  // Tab / arrow keyboard navigation
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                rc = static_cast<int>(msg.wParam);
            } else {
                const Error e{HRESULT_FROM_WIN32(GetLastError()), "CreateMainWindow", "IXC Camera could not open its window."};
                log::Error("app", e.Describe());
                MessageBoxW(nullptr, Utf8ToWide(e.Describe()).c_str(), L"IXC Camera", MB_ICONERROR);
            }
        }
        CoUninitialize();
    }

    log::Info("app", "exiting");
    log::Shutdown();
    if (mutex) CloseHandle(mutex);
    return rc;
}
