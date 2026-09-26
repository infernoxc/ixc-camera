#pragma once

// Picture adjustment controls (sliders + mirror + reset) bound to a Profile.
// Plain Win32 trackbars: no animation, keyboard accessible (Tab / arrow keys / Page keys).

#include "profiles/profile.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace ixc::app {

class AdjustmentsPanel {
public:
    // onChange runs on the UI thread after the profile has been modified.
    void Create(HWND parent, HINSTANCE instance, int firstId, Profile* profile, std::function<void()> onChange);
    void SetFont(HFONT font);
    // Lays the panel out in the given rectangle; returns the height it used.
    int Layout(int x, int y, int width, int rowHeight, int gap);
    void Refresh();  // reload control positions from the profile
    // App-only option (not part of the profile): draw tracked faces over the preview.
    bool FaceOverlay() const { return faceOverlayOn_; }

    // Message routing from the parent window. Return true when handled.
    bool OnScroll(HWND control);
    bool OnCommand(HWND control, int code);

    static constexpr int kIdCount = 64;  // ids reserved from firstId

private:
    struct Slider {
        const wchar_t* label;
        int min, max;      // trackbar units
        double scale;      // profile value = position * scale
        const wchar_t* format;  // value display, e.g. L"%+.0f"
        std::function<double(const Profile&)> get;
        std::function<void(Profile&, double)> set;
        HWND labelWnd = nullptr, track = nullptr, valueWnd = nullptr;
    };
    void UpdateValueText(Slider& s);

    HWND parent_ = nullptr;
    Profile* profile_ = nullptr;
    std::function<void()> onChange_;
    std::vector<Slider> sliders_;
    HWND header_ = nullptr, mirror_ = nullptr, reset_ = nullptr, gpu_ = nullptr, smooth_ = nullptr;
    HWND face_ = nullptr, faceOverlay_ = nullptr;
    bool faceOverlayOn_ = true;
    int firstId_ = 0;
};

}  // namespace ixc::app
