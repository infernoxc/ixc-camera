#pragma once

// The settings column beside the preview: a vertically scrolling stack of cards.
//
//   Profile          profile box, Save / Import / Export / Delete, global hotkeys switch
//   Picture          12 sliders, Mirror switch, Reset
//   Effects          master switch, then one switch per effect with its own strength slider
//                    (shown while the effect is on), "All off"
//   Camera features  Smooth motion, Face tracking, Show face markers, GPU acceleration
//   IXC Camera       system camera status, "Use this webcam for IXC Camera"
//
// Picture/effects/feature controls edit the Profile directly and call onChange. Profile,
// hotkeys and IXC Camera controls are forwarded to the owner window (WM_COMMAND with the ids
// below), which owns that logic. The panel's windows forward WM_COMMAND, WM_HSCROLL and
// WM_CTLCOLOR* to the owner, so the owner handles every control like its own.

#include "app/widgets.h"
#include "profiles/profile.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ixc::app {

enum PanelId : int {
    kIdProfileCombo = 200,
    kIdProfileSave,
    kIdProfileDelete,
    kIdProfileImport,
    kIdProfileExport,
    kIdHotkeys,
    kIdPictureReset,
    kIdMirror,
    kIdPictureSlider0 = 210,  // 12 sliders: 210..221 (order in settings_panel.cpp)
    kIdEffectsMaster = 230,
    kIdEffectsAllOff,
    kIdEffectToggle0 = 240,   // one per effects::Catalog() entry: 240..
    kIdEffectSlider0 = 260,   // 260..
    kIdSmoothMotion = 280,
    kIdFaceTracking,
    kIdFaceMarkers,
    kIdGpu,
    kIdVcamStatus = 290,
    kIdVcamUse,
};

class SettingsPanel {
public:
    // owner: the main window (receives forwarded messages). fonts must outlive the panel.
    bool Create(HWND owner, HINSTANCE instance, Profile* profile, const theme::Fonts* fonts, std::function<void()> onChange);
    HWND hwnd() const { return host_; }
    void SetBounds(const RECT& r);  // position of the scrolling column in the owner
    void Relayout();                // after DPI/font changes or rows appearing
    void Refresh();                 // controls <- profile

    // Messages forwarded from the owner. true = handled here.
    bool OnCommand(HWND control, int code);
    bool OnScroll(HWND control);

    bool FaceOverlay() const { return faceOverlay_; }
    void SetFaceOverlay(bool on);
    // Live state texts (second line of the switches).
    void SetFaceTrackingState(const std::wstring& text);
    void SetGpuState(const std::wstring& text);

    HWND profileCombo() const { return profileCombo_; }
    HWND profileDelete() const { return profileDelete_; }
    HWND hotkeys() const { return hotkeys_; }
    HWND vcamStatus() const { return vcamStatus_; }
    HWND vcamUse() const { return vcamUse_; }

private:
    struct Slider {
        const wchar_t* label;
        int min, max;
        double scale;
        const wchar_t* format;
        std::function<double(const Profile&)> get;
        std::function<void(Profile&, double)> set;
        HWND wnd = nullptr;
    };
    struct Card {
        RECT rc;
        std::wstring title;
    };
    static LRESULT CALLBACK HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ContentProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    int Scale(int v) const;
    void LayoutContent(int width);
    void ScrollTo(int pos);
    void PaintContent(HDC dc, const RECT& rc);
    void ShowEffectSliders();
    int EffectIndex(HWND control, int firstId) const;
    void Changed();

    HWND owner_ = nullptr, host_ = nullptr, content_ = nullptr;
    const theme::Fonts* fonts_ = nullptr;
    Profile* profile_ = nullptr;
    std::function<void()> onChange_;
    std::vector<Slider> sliders_;
    std::vector<Card> cards_;
    HWND profileCombo_ = nullptr, profileSave_ = nullptr, profileDelete_ = nullptr, profileImport_ = nullptr, profileExport_ = nullptr;
    HWND hotkeys_ = nullptr, reset_ = nullptr, mirror_ = nullptr;
    HWND effectsMaster_ = nullptr, effectsAllOff_ = nullptr;
    std::vector<HWND> effectToggles_, effectSliders_;
    std::vector<int> effectStrength_;  // last strength per effect (restored when switched back on)
    HWND smooth_ = nullptr, face_ = nullptr, faceMarkers_ = nullptr, gpu_ = nullptr;
    HWND vcamStatus_ = nullptr, vcamUse_ = nullptr;
    bool faceOverlay_ = true;
    int contentHeight_ = 0, scroll_ = 0, width_ = 0;
};

}  // namespace ixc::app
