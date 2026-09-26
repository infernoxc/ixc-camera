// IXC Camera UI: camera selection, native format selection, live preview, and the IXC Camera
// system camera status (which webcam it uses; changing it asks for administrator approval).
//
// Resource rules this window follows:
//   * The physical camera is opened only while the preview runs, and it's released when the
//     window is minimized or closed.
//   * Frames are painted only when they arrive. There is no render loop.
//   * The 1 s status timer runs only while previewing.

#include "app/adjustments_panel.h"
#include "app/preview_window.h"
#include "camera/capture_session.h"
#include "camera/device_enum.h"
#include "camera/format_select.h"
#include "common/strings.h"
#include "diagnostics/error.h"
#include "diagnostics/log.h"
#include "ixc/version.h"
#include "processing/image_pipeline.h"
#include "profiles/active_profile.h"
#include "profiles/profile_store.h"
#include "virtual_camera/registration.h"

#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <ks.h>
#include <ksmedia.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
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
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\IXCCamera.UI.SingleInstance";

enum ControlId : int {
    kIdCameraLabel = 100,
    kIdCamera,
    kIdFormatLabel,
    kIdFormat,
    kIdStartStop,
    kIdPreview,
    kIdStatus,
    kIdHint,
    kIdPrivacy,
    kIdVcamStatus,
    kIdVcamUse,
};

constexpr UINT kStateMessage = WM_APP + 11;
constexpr UINT kVcamDoneMessage = WM_APP + 12;  // wParam = ixc_vcam.exe exit code
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kDeviceRefreshTimer = 2;
constexpr UINT_PTR kPublishTimer = 3;       // saves/publishes settings shortly after the last slider move
constexpr int kFirstPanelId = 200;

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
    int Scale(int v) const { return MulDiv(v, dpi_, 96); }

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
    static void CALLBACK OnVcamProcessExit(void* ctx, BOOLEAN timedOut);

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    HWND cameraLabel_ = nullptr, camera_ = nullptr, formatLabel_ = nullptr, format_ = nullptr, startStop_ = nullptr;
    HWND status_ = nullptr, hint_ = nullptr, privacy_ = nullptr;
    HWND vcamStatus_ = nullptr, vcamUse_ = nullptr;
    vcam::Status vcam_;
    HANDLE vcamProcess_ = nullptr;  // elevated ixc_vcam.exe while a change is in progress
    HANDLE vcamWait_ = nullptr;
    HFONT font_ = nullptr;
    UINT dpi_ = 96;
    HDEVNOTIFY devNotify_ = nullptr;

    std::filesystem::path dataDir_;
    ProfileStore store_;
    Profile profile_;
    bool profileLoaded_ = false;

    std::vector<CameraInfo> cameras_;
    std::vector<CaptureFormat> formats_;  // normalized; format combo item i+1 ↔ formats_[i] (item 0 = Auto)
    ComPtr<CaptureSession> session_;
    app::PreviewWindow preview_;
    app::AdjustmentsPanel panel_;
    bool publishWarned_ = false;
    bool previewing_ = false;
    bool resumeOnRestore_ = false;
};

bool MainWindow::Create(int showCmd) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &MainWindow::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    app::PreviewWindow::RegisterClass(instance_);

    hwnd_ = CreateWindowExW(0, kWindowClass, L"IXC Camera", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                            CW_USEDEFAULT, 960, 680, nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    if (FAILED(CaptureSession::Create(this, session_))) return false;

    // Refresh the camera list when webcams are plugged in or removed (event-driven).
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = KSCATEGORY_VIDEO_CAMERA;
    devNotify_ = RegisterDeviceNotificationW(hwnd_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    LoadProfile();
    panel_.Refresh();
    RefreshCameras();
    RefreshVcamStatus();
    // Make sure IXC Camera applies this user's current settings (write only when they differ).
    if (vcam_.comRegistered && profileLoaded_) {
        const ProfileLoadResult published = LoadActiveProfile();
        Profile current = profile_;
        Validate(current);
        if (!published.ok || !(published.profile == current)) SaveAndPublish();
    }
    ShowWindow(hwnd_, showCmd);
    return true;
}

void MainWindow::CreateControls() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };
    cameraLabel_ = make(WC_STATICW, L"Camera", SS_LEFT | SS_CENTERIMAGE, kIdCameraLabel);
    camera_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdCamera);
    formatLabel_ = make(WC_STATICW, L"Format", SS_LEFT | SS_CENTERIMAGE, kIdFormatLabel);
    format_ = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdFormat);
    startStop_ = make(WC_BUTTONW, L"Start preview", BS_PUSHBUTTON | WS_TABSTOP, kIdStartStop);
    preview_.Create(hwnd_, instance_, kIdPreview);
    status_ = make(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS, kIdStatus);
    hint_ = make(WC_STATICW, L"", SS_LEFT, kIdHint);
    privacy_ = make(WC_BUTTONW, L"Open camera privacy settings", BS_PUSHBUTTON | WS_TABSTOP, kIdPrivacy);
    ShowWindow(privacy_, SW_HIDE);
    vcamStatus_ = make(WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, kIdVcamStatus);
    vcamUse_ = make(WC_BUTTONW, L"Use this webcam for IXC Camera", BS_PUSHBUTTON | WS_TABSTOP, kIdVcamUse);
    preview_.Clear(L"Choose a camera and select Start preview.");
    panel_.Create(hwnd_, instance_, kFirstPanelId, &profile_, [this] { OnPictureChanged(); });
}

void MainWindow::ApplyFont() {
    if (font_) DeleteObject(font_);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    for (HWND h : {cameraLabel_, camera_, formatLabel_, format_, startStop_, status_, hint_, privacy_, vcamStatus_, vcamUse_}) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    panel_.SetFont(font_);
}

void MainWindow::Layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int pad = Scale(12), rowH = Scale(28), gap = Scale(8);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;

    int x = pad;
    const int y = pad;
    const int labelW = Scale(56), buttonW = Scale(130);
    const int comboSpace = w - 2 * pad - 2 * labelW - buttonW - 4 * gap;
    const int cameraW = std::max(Scale(120), comboSpace * 45 / 100);
    const int formatW = std::max(Scale(120), comboSpace - cameraW);

    MoveWindow(cameraLabel_, x, y, labelW, rowH, TRUE);
    x += labelW + gap;
    MoveWindow(camera_, x, y, cameraW, Scale(300), TRUE);  // dropdown height
    x += cameraW + gap;
    MoveWindow(formatLabel_, x, y, labelW, rowH, TRUE);
    x += labelW + gap;
    MoveWindow(format_, x, y, formatW, Scale(400), TRUE);
    x += formatW + gap;
    MoveWindow(startStop_, x, y, buttonW, rowH, TRUE);

    const bool privacyVisible = IsWindowVisible(privacy_) != FALSE;
    const int bottomH = rowH * 3 + 2 * gap + (privacyVisible ? rowH + gap : 0);
    const int previewTop = y + rowH + gap;
    const int previewH = std::max(0, h - previewTop - bottomH - pad);
    // Picture adjustments on the right of the preview; the preview stays the visual centre.
    const int panelW = Scale(300);
    const int previewW = std::max(0, w - 2 * pad - panelW - gap);
    MoveWindow(preview_.hwnd(), pad, previewTop, previewW, previewH, TRUE);
    panel_.Layout(pad + previewW + gap, previewTop, panelW, Scale(26), gap);

    int by = previewTop + previewH + gap;
    MoveWindow(status_, pad, by, w - 2 * pad, rowH, TRUE);
    by += rowH;
    MoveWindow(hint_, pad, by, w - 2 * pad, rowH, TRUE);
    by += rowH + gap;
    if (privacyVisible) {
        MoveWindow(privacy_, pad, by, Scale(240), rowH, TRUE);
        by += rowH + gap;
    }
    // IXC Camera row: status text, then the "use this webcam" button on the right.
    const int useW = Scale(230);
    MoveWindow(vcamStatus_, pad, by, std::max(0, w - 2 * pad - useW - gap), rowH, TRUE);
    MoveWindow(vcamUse_, w - pad - useW, by, useW, rowH, TRUE);
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
    SetWindowTextW(vcamStatus_, text.c_str());
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
        SetWindowTextW(vcamStatus_, L"The IXC Camera installation is incomplete (ixc_vcam.exe is missing). Reinstall IXC Camera.");
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
        SetWindowTextW(vcamStatus_, err == ERROR_CANCELLED ? L"No change made: administrator approval was declined."
                                                           : W(Error{HRESULT_FROM_WIN32(err), "LaunchIxcVcam",
                                                                     "IXC Camera could not start the camera update."}.Describe()).c_str());
        return;
    }
    vcamProcess_ = sei.hProcess;
    SetWindowTextW(vcamStatus_, (L"Switching IXC Camera to " + W(cam.name) + L"…").c_str());
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
        SetWindowTextW(vcamStatus_, msg.c_str());
        log::Error("vcam", "ixc_vcam register failed with exit code " + std::to_string(exitCode));
    } else {
        log::Info("vcam", "IXC Camera now uses " + vcam_.wrappedCameraName);
    }
}

// ---- profile ------------------------------------------------------------------------------------

void MainWindow::LoadProfile() {
    ProfileLoadResult r = store_.Load("default");
    if (r.ok) {
        profile_ = r.profile;
        profileLoaded_ = true;
        for (const auto& w : r.warnings) log::Warn("profiles", "default: " + w);
        return;
    }
    if (std::filesystem::exists(store_.directory() / L"default.json")) {
        // Never overwrite a profile we couldn't read. Keep it on disk and run with defaults
        // without saving over it.
        log::Error("profiles", "default profile unreadable, not overwriting: " + r.error);
        SetWindowTextW(hint_, (L"Your saved profile could not be read and was left unchanged: " + W(r.error)).c_str());
        profileLoaded_ = false;
        return;
    }
    profileLoaded_ = true;  // fresh install: defaults, saved on first preview
}

void MainWindow::SaveProfile() {
    if (!profileLoaded_) return;
    std::string stem = "default";
    const HRESULT hr = store_.Save(profile_, stem);
    if (FAILED(hr)) log::Error("profiles", Error{hr, "SaveProfile", "IXC Camera could not save the profile."}.Describe());
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
    std::wstring autoLabel = L"Auto (recommended)";
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

    SetWindowTextW(status_, (L"Opening " + W(cam.name) + L"…").c_str());
    const Error err = session_->Start(cfg);
    if (FAILED(err.hr)) {
        const bool denied = ClassifyHResult(err.hr) == ErrorClass::AccessDenied;
        preview_.Clear(denied ? L"Windows is blocking camera access for desktop apps."
                              : L"The camera could not be started. See the message below.");
        SetWindowTextW(status_, W(err.Describe()).c_str());
        if (denied) {
            SetWindowTextW(hint_, L"Turn on \"Camera access\" and \"Let desktop apps access your camera\" in Windows "
                                  L"Settings, then select Start preview again.");
        } else if (ClassifyHResult(err.hr) == ErrorClass::Transient) {
            SetWindowTextW(hint_, L"Another app may be using the camera exclusively. Close it and try again.");
        }
        ShowPrivacyHelp(denied);
        return;
    }

    previewing_ = true;
    ShowPrivacyHelp(false);
    SetWindowTextW(hint_, L"");
    SetWindowTextW(startStop_, L"Stop preview");
    EnableWindow(camera_, FALSE);
    EnableWindow(format_, FALSE);
    SetTimer(hwnd_, kStatusTimer, 1000, nullptr);

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
    UpdatePipeline();                            // preview reflects the change immediately
    SetTimer(hwnd_, kPublishTimer, 250, nullptr);  // disk writes only after the slider settles
}

void MainWindow::UpdatePipeline() {
    if (!previewing_ || !session_) {
        preview_.SetPipeline(nullptr);
        return;
    }
    const FrameLayout l = session_->Layout();
    const bool fullRange = l.nominalRange == MFNominalRange_0_255;
    preview_.SetPipeline(std::make_shared<const processing::PipelineParams>(processing::CompileParams(profile_, l.width, l.height, fullRange)));
}

void MainWindow::SaveAndPublish() {
    KillTimer(hwnd_, kPublishTimer);
    SaveProfile();
    // Publish for the IXC Camera source (inside the Windows camera service). Apps using IXC
    // Camera pick the change up on their next frame.
    if (!vcam_.comRegistered) return;
    const HRESULT hr = PublishActiveProfile(profile_);
    if (FAILED(hr) && !publishWarned_) {
        publishWarned_ = true;
        const Error e{hr, "PublishActiveProfile", "IXC Camera could not share these settings with the system camera."};
        log::Error("profiles", e.Describe());
        SetWindowTextW(vcamStatus_, (W(e.Describe()) + L" Reinstalling IXC Camera repairs the settings folder.").c_str());
    }
}

void MainWindow::StopPreview(const wchar_t* placeholder) {
    KillTimer(hwnd_, kStatusTimer);
    if (session_) session_->Stop();  // releases the physical camera
    previewing_ = false;
    preview_.Clear(placeholder);
    SetWindowTextW(startStop_, L"Start preview");
    EnableWindow(camera_, TRUE);
    EnableWindow(format_, TRUE);
    SetWindowTextW(hint_, L"");
}

void MainWindow::UpdateStatus() {
    if (!previewing_) return;
    const auto st = session_->Stats();
    const auto fmt = session_->ActiveFormat();
    wchar_t buf[320];
    const int cam = static_cast<int>(SendMessageW(camera_, CB_GETCURSEL, 0, 0));
    const std::wstring name = cam >= 0 ? W(cameras_[static_cast<size_t>(cam)].name) : L"";
    if (st.meanLatencyMs >= 0) {
        swprintf_s(buf, L"%s • %s • receiving %.1f FPS • dropped %llu • latency %.0f ms", name.c_str(), W(Describe(fmt)).c_str(),
                   st.fps, static_cast<unsigned long long>(session_->DroppedFrames()), st.meanLatencyMs);
    } else {
        swprintf_s(buf, L"%s • %s • receiving %.1f FPS • dropped %llu", name.c_str(), W(Describe(fmt)).c_str(), st.fps,
                   static_cast<unsigned long long>(session_->DroppedFrames()));
    }
    SetWindowTextW(status_, buf);

    if (st.underSpeed) {
        swprintf_s(buf, L"The camera is delivering %.0f of %.0f FPS. This usually means low light: the camera lengthens its "
                        L"exposure. More light restores the full frame rate.", st.fps, st.nominalFps);
        SetWindowTextW(hint_, buf);
    } else if (session_->State() == CaptureState::Streaming) {
        SetWindowTextW(hint_, L"");
    }
}

void MainWindow::OnCaptureState(CaptureState s) {
    if (!previewing_) return;
    switch (s) {
        case CaptureState::Reconnecting:
            preview_.Clear(L"Camera disconnected. Reconnecting…");
            SetWindowTextW(hint_, L"IXC Camera will reconnect automatically when the camera is available again.");
            break;
        case CaptureState::WaitingForDevice:
            preview_.Clear(L"Waiting for the camera to be connected again.");
            SetWindowTextW(hint_, L"Plug the camera back in. IXC Camera resumes automatically and uses no CPU while waiting.");
            break;
        case CaptureState::Streaming:
            SetWindowTextW(hint_, L"");
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
            ApplyFont();
            return 0;

        case WM_GETFONT:
            return reinterpret_cast<LRESULT>(font_);

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
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {Scale(900), Scale(660)};  // room for preview + adjustment panel
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
                        OnCameraSelected();
                        UpdateVcamControls();
                    }
                    return 0;
                case kIdStartStop:
                    if (previewing_) StopPreview(L"Preview stopped. The camera is released.");
                    else StartPreview();
                    return 0;
                case kIdVcamUse:
                    UseSelectedForIxcCamera();
                    return 0;
                case kIdPrivacy:
                    ShellExecuteW(hwnd_, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                default:
                    break;
            }
            break;

        case app::PreviewWindow::kFrameMessage: {
            preview_.FrameMessageHandled();
            if (previewing_ && !IsIconic(hwnd_)) {
                if (auto frame = session_->TakeFrame()) {
                    const FrameLayout l = session_->Layout();
                    preview_.ShowFrame(std::move(*frame), l);
                }
            }
            return 0;
        }

        case kVcamDoneMessage:
            OnVcamDone(static_cast<DWORD>(wp));
            return 0;

        case kStateMessage:
            OnCaptureState(static_cast<CaptureState>(wp));
            return 0;

        case WM_TIMER:
            if (wp == kStatusTimer) UpdateStatus();
            else if (wp == kPublishTimer) SaveAndPublish();
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
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

        case WM_DESTROY:
            KillTimer(hwnd_, kStatusTimer);
            if (KillTimer(hwnd_, kPublishTimer)) SaveAndPublish();  // don't lose the last slider move
            if (session_) {
                session_->Close();
                session_.Reset();
            }
            if (devNotify_) UnregisterDeviceNotification(devNotify_);
            if (vcamWait_) UnregisterWaitEx(vcamWait_, INVALID_HANDLE_VALUE);  // waits for an in-flight callback
            if (vcamProcess_) CloseHandle(vcamProcess_);
            if (font_) DeleteObject(font_);
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
