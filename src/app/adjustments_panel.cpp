#include "app/adjustments_panel.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ixc::app {

namespace {

// Profile accessors for each adjustment.
#define IXC_IMAGE_FIELD(field) \
    [](const Profile& p) { return p.image.field; }, [](Profile& p, double v) { p.image.field = v; }

}  // namespace

void AdjustmentsPanel::Create(HWND parent, HINSTANCE instance, int firstId, Profile* profile, std::function<void()> onChange) {
    parent_ = parent;
    profile_ = profile;
    onChange_ = std::move(onChange);
    firstId_ = firstId;

    sliders_ = {
        {L"Brightness", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(brightness)},
        {L"Contrast", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(contrast)},
        {L"Saturation", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(saturation)},
        {L"Warmth", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(temperature)},
        {L"Tint", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(tint)},
        {L"Exposure (EV)", -20, 20, 0.1, L"%+.1f", IXC_IMAGE_FIELD(exposureEv)},
        {L"Highlights", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(highlights)},
        {L"Shadows", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(shadows)},
        {L"Low-light boost", 0, 100, 1.0, L"%.0f", IXC_IMAGE_FIELD(lowLight)},
        {L"Gamma", 20, 300, 0.01, L"%.2f", IXC_IMAGE_FIELD(gamma)},
        {L"Sharpness", 0, 100, 1.0, L"%.0f", IXC_IMAGE_FIELD(sharpness)},
        {L"Digital zoom", 100, 400, 0.01, L"%.2fx", [](const Profile& p) { return p.zoom; }, [](Profile& p, double v) { p.zoom = v; }},
    };

    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    int id = firstId;
    header_ = make(WC_STATICW, L"Picture", SS_LEFT, id++);
    for (auto& s : sliders_) {
        s.labelWnd = make(WC_STATICW, s.label, SS_LEFT | SS_CENTERIMAGE, id++);
        s.track = make(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, id++);
        s.valueWnd = make(WC_STATICW, L"", SS_RIGHT | SS_CENTERIMAGE, id++);
        SendMessageW(s.track, TBM_SETRANGEMIN, FALSE, s.min);
        SendMessageW(s.track, TBM_SETRANGEMAX, FALSE, s.max);
        SendMessageW(s.track, TBM_SETPAGESIZE, 0, std::max(1, (s.max - s.min) / 20));
        SendMessageW(s.track, TBM_SETLINESIZE, 0, 1);
    }
    mirror_ = make(WC_BUTTONW, L"Mirror (flip left-right)", BS_AUTOCHECKBOX | WS_TABSTOP, id++);
    reset_ = make(WC_BUTTONW, L"Reset picture", BS_PUSHBUTTON | WS_TABSTOP, id++);
    gpu_ = make(WC_BUTTONW, L"Use the graphics card for zoom when it's faster", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, id++);
    smooth_ = make(WC_BUTTONW, L"Smooth motion: keep full frame rate in low light", BS_AUTOCHECKBOX | BS_MULTILINE | WS_TABSTOP, id++);
    face_ = make(WC_BUTTONW, L"Face tracking (used by face effects)", BS_AUTOCHECKBOX | WS_TABSTOP, id++);
    faceOverlay_ = make(WC_BUTTONW, L"Show face markers in preview (not sent to apps)", BS_AUTOCHECKBOX | WS_TABSTOP, id++);
    Refresh();
}

void AdjustmentsPanel::SetFont(HFONT font) {
    auto set = [&](HWND h) { SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); };
    set(header_);
    set(mirror_);
    set(reset_);
    set(gpu_);
    set(smooth_);
    set(face_);
    set(faceOverlay_);
    for (auto& s : sliders_) {
        set(s.labelWnd);
        set(s.valueWnd);
    }
}

int AdjustmentsPanel::Layout(int x, int y, int width, int rowHeight, int gap) {
    const int labelW = width * 40 / 100, valueW = width * 16 / 100, trackW = width - labelW - valueW;
    int cy = y;
    MoveWindow(header_, x, cy, width, rowHeight, TRUE);
    cy += rowHeight;
    for (auto& s : sliders_) {
        MoveWindow(s.labelWnd, x, cy, labelW, rowHeight, TRUE);
        MoveWindow(s.track, x + labelW, cy, trackW, rowHeight, TRUE);
        MoveWindow(s.valueWnd, x + labelW + trackW, cy, valueW, rowHeight, TRUE);
        cy += rowHeight;
    }
    cy += gap / 2;
    MoveWindow(mirror_, x, cy, width, rowHeight, TRUE);
    cy += rowHeight + gap / 2;
    MoveWindow(reset_, x, cy, width / 2, rowHeight, TRUE);
    cy += rowHeight + gap;
    MoveWindow(gpu_, x, cy, width, rowHeight * 2, TRUE);  // two lines: it's a sentence
    cy += rowHeight * 2;
    MoveWindow(smooth_, x, cy, width, rowHeight * 2, TRUE);
    cy += rowHeight * 2;
    MoveWindow(face_, x, cy, width, rowHeight, TRUE);
    cy += rowHeight;
    MoveWindow(faceOverlay_, x + 16, cy, width - 16, rowHeight, TRUE);
    cy += rowHeight;
    return cy - y;
}

void AdjustmentsPanel::UpdateValueText(Slider& s) {
    wchar_t buf[32];
    swprintf_s(buf, s.format, s.get(*profile_));
    SetWindowTextW(s.valueWnd, buf);
}

void AdjustmentsPanel::Refresh() {
    if (!profile_) return;
    for (auto& s : sliders_) {
        const int pos = static_cast<int>(std::lround(s.get(*profile_) / s.scale));
        SendMessageW(s.track, TBM_SETPOS, TRUE, pos);
        UpdateValueText(s);
    }
    SendMessageW(mirror_, BM_SETCHECK, profile_->mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(gpu_, BM_SETCHECK, profile_->gpu == GpuMode::Auto ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(smooth_, BM_SETCHECK, profile_->smoothMotion ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(face_, BM_SETCHECK, profile_->faceTracking.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(faceOverlay_, BM_SETCHECK, faceOverlayOn_ ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(faceOverlay_, profile_->faceTracking.enabled);
}

bool AdjustmentsPanel::OnScroll(HWND control) {
    for (auto& s : sliders_) {
        if (s.track != control) continue;
        const int pos = static_cast<int>(SendMessageW(s.track, TBM_GETPOS, 0, 0));
        const double v = pos * s.scale;
        if (v != s.get(*profile_)) {
            s.set(*profile_, v);
            UpdateValueText(s);
            if (onChange_) onChange_();
        }
        return true;
    }
    return false;
}

bool AdjustmentsPanel::OnCommand(HWND control, int code) {
    if (control == mirror_ && code == BN_CLICKED) {
        profile_->mirror = SendMessageW(mirror_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (onChange_) onChange_();
        return true;
    }
    if (control == gpu_ && code == BN_CLICKED) {
        profile_->gpu = SendMessageW(gpu_, BM_GETCHECK, 0, 0) == BST_CHECKED ? GpuMode::Auto : GpuMode::Off;
        if (onChange_) onChange_();
        return true;
    }
    if (control == face_ && code == BN_CLICKED) {
        profile_->faceTracking.enabled = SendMessageW(face_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        EnableWindow(faceOverlay_, profile_->faceTracking.enabled);
        if (onChange_) onChange_();
        return true;
    }
    if (control == faceOverlay_ && code == BN_CLICKED) {
        faceOverlayOn_ = SendMessageW(faceOverlay_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (onChange_) onChange_();
        return true;
    }
    if (control == smooth_ && code == BN_CLICKED) {
        profile_->smoothMotion = SendMessageW(smooth_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (onChange_) onChange_();
        return true;
    }
    if (control == reset_ && code == BN_CLICKED) {
        // Picture settings only; camera, format and hotkeys are kept.
        const Profile defaults;
        profile_->image = defaults.image;
        profile_->zoom = defaults.zoom;
        profile_->crop = defaults.crop;
        profile_->mirror = defaults.mirror;
        Refresh();
        if (onChange_) onChange_();
        return true;
    }
    return false;
}

}  // namespace ixc::app
