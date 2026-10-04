#include "app/settings_panel.h"

#include "effects/effects.h"
#include "ixc/version.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace ixc::app {

namespace {

constexpr wchar_t kHostClass[] = L"IXCSettingsHost";
constexpr wchar_t kContentClass[] = L"IXCSettingsContent";
constexpr int kDefaultStrength = 70;

#define IXC_IMAGE_FIELD(field) [](const Profile& p) { return p.image.field; }, [](Profile& p, double v) { p.image.field = v; }

// Mouse wheel over a closed combo box scrolls the panel instead of changing the selection.
LRESULT CALLBACK ComboWheel(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEWHEEL && !SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0)) return SendMessageW(GetParent(h), msg, wp, lp);
    return DefSubclassProc(h, msg, wp, lp);
}

const wchar_t* CategoryText(const effects::EffectInfo& e) {
    if (e.needsFace) return L"Face effect · uses face tracking";
    if (e.needsSegmentation) return L"Background · finds you in the picture";
    if (std::string_view(e.category) == "lighting") return L"Lighting look";
    return L"Colour look";
}

}  // namespace

int SettingsPanel::Scale(int v) const { return MulDiv(v, static_cast<int>(GetDpiForWindow(owner_)), 96); }

bool SettingsPanel::Create(HWND owner, HINSTANCE instance, Profile* profile, const theme::Fonts* fonts, std::function<void()> onChange) {
    owner_ = owner;
    profile_ = profile;
    fonts_ = fonts;
    onChange_ = std::move(onChange);

    for (const auto& [name, proc] : {std::pair{kHostClass, &SettingsPanel::HostProc}, std::pair{kContentClass, &SettingsPanel::ContentProc}}) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = name;
        RegisterClassExW(&wc);
    }
    host_ = CreateWindowExW(WS_EX_CONTROLPARENT, kHostClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 0, 0,
                            owner, nullptr, instance, this);
    content_ = CreateWindowExW(WS_EX_CONTROLPARENT, kContentClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, host_,
                               nullptr, instance, this);
    if (!host_ || !content_) return false;
    theme::ApplyDarkControl(host_, false);  // dark scroll bar

    const COLORREF card = theme::kSurface;
    auto button = [&](int id, const wchar_t* text, ButtonStyle style) { return CreateButton(content_, id, text, style, fonts_, card); };
    auto toggle = [&](int id, const wchar_t* text, const wchar_t* sub) {
        HWND h = CreateToggle(content_, id, text, fonts_, card);
        if (sub) SetWidgetSubtext(h, sub);
        return h;
    };

    // Profile
    profileCombo_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN | CBS_AUTOHSCROLL, 0,
                                    0, 0, 0, content_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdProfileCombo)), instance, nullptr);
    SendMessageW(profileCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->body), FALSE);
    SendMessageW(profileCombo_, CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Profile name"));
    theme::ApplyDarkControl(profileCombo_, true);
    SetWindowSubclass(profileCombo_, ComboWheel, 1, 0);
    profileSave_ = button(kIdProfileSave, L"Save", ButtonStyle::Primary);
    profileImport_ = button(kIdProfileImport, L"Import…", ButtonStyle::Secondary);
    profileExport_ = button(kIdProfileExport, L"Export…", ButtonStyle::Secondary);
    profileDelete_ = button(kIdProfileDelete, L"Delete", ButtonStyle::Danger);
    hotkeys_ = toggle(kIdHotkeys, L"Global hotkeys", L"Ctrl+Alt+F8 effects · F9/F10 profile · F11 mirror");

    // Picture
    reset_ = button(kIdPictureReset, L"Reset", ButtonStyle::Ghost);
    sliders_ = {
        {L"Brightness", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(brightness)},
        {L"Contrast", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(contrast)},
        {L"Saturation", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(saturation)},
        {L"Warmth", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(temperature)},
        {L"Tint", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(tint)},
        {L"Exposure", -20, 20, 0.1, L"%+.1f EV", IXC_IMAGE_FIELD(exposureEv)},
        {L"Highlights", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(highlights)},
        {L"Shadows", -100, 100, 1.0, L"%+.0f", IXC_IMAGE_FIELD(shadows)},
        {L"Low-light boost", 0, 100, 1.0, L"%.0f", IXC_IMAGE_FIELD(lowLight)},
        {L"Gamma", 20, 300, 0.01, L"%.2f", IXC_IMAGE_FIELD(gamma)},
        {L"Sharpness", 0, 100, 1.0, L"%.0f", IXC_IMAGE_FIELD(sharpness)},
        {L"Digital zoom", 100, 400, 0.01, L"%.2f×", [](const Profile& p) { return p.zoom; }, [](Profile& p, double v) { p.zoom = v; }},
    };
    const Profile defaults;
    for (size_t i = 0; i < sliders_.size(); ++i) {
        Slider& s = sliders_[i];
        const int def = static_cast<int>(std::lround(s.get(defaults) / s.scale));
        const double scale = s.scale;
        const wchar_t* fmt = s.format;
        s.wnd = CreateSlider(content_, kIdPictureSlider0 + static_cast<int>(i), s.label, s.min, s.max, def,
                             [scale, fmt](int p) {
                                 wchar_t b[32];
                                 swprintf_s(b, fmt, p * scale);
                                 return std::wstring(b);
                             },
                             fonts_, card);
    }
    mirror_ = toggle(kIdMirror, L"Mirror", L"Flip left-right");

    // Effects
    effectsAllOff_ = button(kIdEffectsAllOff, L"All off", ButtonStyle::Ghost);
    effectsMaster_ = toggle(kIdEffectsMaster, L"Effects", L"Ctrl+Alt+F8 switches all effects");
    const auto& catalog = effects::Catalog();
    effectStrength_.assign(catalog.size(), kDefaultStrength);
    for (size_t i = 0; i < catalog.size(); ++i) {
        effectToggles_.push_back(toggle(kIdEffectToggle0 + static_cast<int>(i), catalog[i].name, CategoryText(catalog[i])));
        effectSliders_.push_back(CreateSlider(content_, kIdEffectSlider0 + static_cast<int>(i), L"Strength", 0, 100, kDefaultStrength,
                                              [](int p) { return std::to_wstring(p) + L"%"; }, fonts_, card));
    }

    // Camera features
    smooth_ = toggle(kIdSmoothMotion, L"Smooth motion", L"Keeps the full frame rate in low light");
    autoFraming_ = toggle(kIdAutoFraming, L"Auto-framing", L"Zooms and pans to keep you in the picture");
    face_ = toggle(kIdFaceTracking, L"Face tracking", L"Off");
    faceMarkers_ = toggle(kIdFaceMarkers, L"Show face markers", L"Preview only, never sent to apps");
    gpu_ = toggle(kIdGpu, L"GPU acceleration", L"Zoom only, when measured faster");

    // IXC Camera (system camera)
    vcamStatus_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, content_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdVcamStatus)), instance, nullptr);
    SendMessageW(vcamStatus_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->caption), FALSE);
    vcamUse_ = button(kIdVcamUse, L"Use this webcam for IXC Camera", ButtonStyle::Secondary);

    Refresh();
    return true;
}

void SettingsPanel::SetBounds(const RECT& r) {
    MoveWindow(host_, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    Relayout();
}

void SettingsPanel::Relayout() {
    RECT rc;
    GetClientRect(host_, &rc);
    LayoutContent(rc.right - rc.left);
}

// Places every control and computes the card rectangles; the content window is as tall as
// its content and the host scrolls it.
void SettingsPanel::LayoutContent(int width) {
    if (width <= 0) return;
    width_ = width;
    const int pad = Scale(12), inner = Scale(14), gap = Scale(6), titleH = Scale(30), cardGap = Scale(12);
    const int x = pad + inner, w = std::max(Scale(120), width - 2 * (pad + inner));
    int y = pad;
    cards_.clear();
    HDWP dwp = BeginDeferWindowPos(80);
    auto place = [&](HWND h, int px, int py, int pw, int ph) {
        if (dwp) dwp = DeferWindowPos(dwp, h, nullptr, px, py, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto row = [&](HWND h) {
        const int hh = WidgetHeight(h);
        place(h, x, y, w, hh);
        y += hh + gap;
    };
    auto beginCard = [&](const wchar_t* title, HWND headerButton) {
        cards_.push_back({RECT{pad, y, width - pad, y}, title});
        if (headerButton) place(headerButton, x + w - Scale(76), y + Scale(5), Scale(76), Scale(26));
        y += titleH + Scale(4);
    };
    auto endCard = [&] {
        y += inner - gap;
        cards_.back().rc.bottom = y;
        y += cardGap;
    };

    beginCard(L"PROFILE", nullptr);
    const int saveW = Scale(72);
    place(profileCombo_, x, y, w - saveW - gap, Scale(300));  // height includes the drop-down list
    place(profileSave_, x + w - saveW, y, saveW, Scale(30));
    y += Scale(30) + gap;
    const int third = (w - 2 * gap) / 3;
    place(profileImport_, x, y, third, Scale(30));
    place(profileExport_, x + third + gap, y, third, Scale(30));
    place(profileDelete_, x + 2 * (third + gap), y, w - 2 * (third + gap), Scale(30));
    y += Scale(30) + gap;
    row(hotkeys_);
    endCard();

    beginCard(L"PICTURE", reset_);
    for (auto& s : sliders_) row(s.wnd);
    row(mirror_);
    endCard();

    beginCard(L"EFFECTS", effectsAllOff_);
    row(effectsMaster_);
    for (size_t i = 0; i < effectToggles_.size(); ++i) {
        row(effectToggles_[i]);
        if (GetWindowLongPtrW(effectSliders_[i], GWL_STYLE) & WS_VISIBLE) {
            const int hh = WidgetHeight(effectSliders_[i]);
            place(effectSliders_[i], x + Scale(14), y - gap / 2, w - Scale(14), hh);  // indented under its switch
            y += hh + gap;
        }
    }
    endCard();

    beginCard(L"CAMERA FEATURES", nullptr);
    row(smooth_);
    row(autoFraming_);
    row(face_);
    place(faceMarkers_, x + Scale(14), y, w - Scale(14), WidgetHeight(faceMarkers_));
    y += WidgetHeight(faceMarkers_) + gap;
    row(gpu_);
    endCard();

    beginCard(L"IXC CAMERA", nullptr);
    {
        // Height of the wrapped status text at this width.
        wchar_t text[512];
        GetWindowTextW(vcamStatus_, text, 512);
        HDC dc = GetDC(content_);
        HGDIOBJ old = SelectObject(dc, fonts_->caption);
        RECT calc{0, 0, w, 0};
        DrawTextW(dc, text[0] ? text : L"X", -1, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(content_, dc);
        place(vcamStatus_, x, y, w, calc.bottom);
        y += calc.bottom + gap + Scale(2);
    }
    place(vcamUse_, x, y, w, Scale(32));
    y += Scale(32) + gap;
    endCard();

    y += Scale(18);  // room for the "about" line painted at the bottom
    contentHeight_ = y + pad;
    if (dwp) EndDeferWindowPos(dwp);

    RECT host;
    GetClientRect(host_, &host);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_DISABLENOSCROLL};
    si.nMin = 0;
    si.nMax = contentHeight_ - 1;
    si.nPage = static_cast<UINT>(std::max(0L, host.bottom));
    SetScrollInfo(host_, SB_VERT, &si, TRUE);
    ScrollTo(scroll_);
    InvalidateRect(content_, nullptr, FALSE);
}

void SettingsPanel::ScrollTo(int pos) {
    RECT host;
    GetClientRect(host_, &host);
    const int maxPos = std::max(0, contentHeight_ - static_cast<int>(host.bottom));
    scroll_ = std::clamp(pos, 0, maxPos);
    SCROLLINFO si{sizeof(si), SIF_POS};
    si.nPos = scroll_;
    SetScrollInfo(host_, SB_VERT, &si, TRUE);
    SetWindowPos(content_, nullptr, 0, -scroll_, width_, std::max(contentHeight_, static_cast<int>(host.bottom)), SWP_NOZORDER | SWP_NOACTIVATE);
}

void SettingsPanel::PaintContent(HDC dc, const RECT& rc) {
    using namespace theme;
    FillRect(dc, &rc, Brush(kBg));
    for (const Card& c : cards_) {
        FillRound(dc, c.rc, Scale(10), kSurface, kBorder);
        RECT t{c.rc.left + Scale(14), c.rc.top + Scale(4), c.rc.right - Scale(90), c.rc.top + Scale(34)};
        DrawTextIn(dc, c.title, t, fonts_->section, kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT about{Scale(12), contentHeight_ - Scale(36), rc.right - Scale(12), contentHeight_ - Scale(12)};
    DrawTextIn(dc, L"IXC Camera " IXC_VERSION_STRING L" · all processing stays on this PC", about, fonts_->caption, kTextFaint,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void SettingsPanel::Refresh() {
    if (!profile_) return;
    for (auto& s : sliders_) SendMessageW(s.wnd, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(s.get(*profile_) / s.scale)));
    SendMessageW(mirror_, BM_SETCHECK, profile_->mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(gpu_, BM_SETCHECK, profile_->gpu == GpuMode::Auto ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(smooth_, BM_SETCHECK, profile_->smoothMotion ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(autoFraming_, BM_SETCHECK, profile_->autoFraming ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(face_, BM_SETCHECK, profile_->faceTracking.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(faceMarkers_, BM_SETCHECK, faceOverlay_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(effectsMaster_, BM_SETCHECK, profile_->effectsEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    const auto& catalog = effects::Catalog();
    for (size_t i = 0; i < catalog.size(); ++i) {
        const auto it = std::find_if(profile_->effects.begin(), profile_->effects.end(), [&](const EffectEntry& e) { return e.id == catalog[i].id; });
        const bool on = it != profile_->effects.end();
        if (on) effectStrength_[i] = static_cast<int>(std::lround(it->strength));
        SendMessageW(effectToggles_[i], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(effectSliders_[i], TBM_SETPOS, TRUE, effectStrength_[i]);
    }
    ShowEffectSliders();
}

// Effect rows: a strength slider appears under an effect while it's on; with the master switch
// off every effect row is disabled (nothing is processed).
void SettingsPanel::ShowEffectSliders() {
    const bool master = profile_->effectsEnabled;
    bool changed = false;
    for (size_t i = 0; i < effectToggles_.size(); ++i) {
        const bool on = SendMessageW(effectToggles_[i], BM_GETCHECK, 0, 0) == BST_CHECKED;
        EnableWindow(effectToggles_[i], master);
        EnableWindow(effectSliders_[i], master && on);
        const bool visible = (GetWindowLongPtrW(effectSliders_[i], GWL_STYLE) & WS_VISIBLE) != 0;  // own flag (parent may be hidden)
        if (visible != on) {
            ShowWindow(effectSliders_[i], on ? SW_SHOWNA : SW_HIDE);
            changed = true;
        }
    }
    EnableWindow(faceMarkers_, TRUE);
    if (changed) Relayout();
}

int SettingsPanel::EffectIndex(HWND control, int firstId) const {
    const int id = GetDlgCtrlID(control) - firstId;
    return id >= 0 && id < static_cast<int>(effectToggles_.size()) ? id : -1;
}

void SettingsPanel::Changed() {
    if (onChange_) onChange_();
}

bool SettingsPanel::OnScroll(HWND control) {
    for (auto& s : sliders_) {
        if (s.wnd != control) continue;
        const double v = static_cast<double>(SendMessageW(s.wnd, TBM_GETPOS, 0, 0)) * s.scale;
        if (v != s.get(*profile_)) {
            s.set(*profile_, v);
            Changed();
        }
        return true;
    }
    if (GetParent(control) == content_) {
        const int i = EffectIndex(control, kIdEffectSlider0);
        if (i < 0) return false;
        const int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
        effectStrength_[static_cast<size_t>(i)] = pos;
        const std::string id = effects::Catalog()[static_cast<size_t>(i)].id;
        for (auto& e : profile_->effects) {
            if (e.id == id && e.strength != pos) {
                e.strength = pos;
                Changed();
            }
        }
        return true;
    }
    return false;
}

bool SettingsPanel::OnCommand(HWND control, int code) {
    if (code != BN_CLICKED || GetParent(control) != content_) return false;
    const auto checked = [&](HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    const int id = GetDlgCtrlID(control);
    switch (id) {
        case kIdMirror: profile_->mirror = checked(mirror_); break;
        case kIdGpu: profile_->gpu = checked(gpu_) ? GpuMode::Auto : GpuMode::Off; break;
        case kIdSmoothMotion: profile_->smoothMotion = checked(smooth_); break;
        case kIdAutoFraming: profile_->autoFraming = checked(autoFraming_); break;
        case kIdFaceTracking: profile_->faceTracking.enabled = checked(face_); break;
        case kIdFaceMarkers: faceOverlay_ = checked(faceMarkers_); break;
        case kIdEffectsMaster:
            profile_->effectsEnabled = checked(effectsMaster_);
            ShowEffectSliders();
            break;
        case kIdEffectsAllOff:
            profile_->effects.clear();
            Refresh();
            break;
        case kIdPictureReset: {
            // Picture only: camera, format, effects and features are kept.
            const Profile defaults;
            profile_->image = defaults.image;
            profile_->zoom = defaults.zoom;
            profile_->crop = defaults.crop;
            profile_->mirror = defaults.mirror;
            Refresh();
            break;
        }
        default: {
            const int i = EffectIndex(control, kIdEffectToggle0);
            if (i < 0) return false;  // profile / hotkeys / IXC Camera controls: the owner's
            const std::string eid = effects::Catalog()[static_cast<size_t>(i)].id;
            auto& list = profile_->effects;
            std::erase_if(list, [&](const EffectEntry& e) { return e.id == eid; });
            if (checked(control) && list.size() < kMaxEnabledEffects) list.push_back({eid, static_cast<double>(effectStrength_[static_cast<size_t>(i)])});
            ShowEffectSliders();
            break;
        }
    }
    Changed();
    return true;
}

void SettingsPanel::SetFaceOverlay(bool on) {
    faceOverlay_ = on;
    SendMessageW(faceMarkers_, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SettingsPanel::SetFaceTrackingState(const std::wstring& text) { SetWidgetSubtext(face_, text); }
void SettingsPanel::SetGpuState(const std::wstring& text) { SetWidgetSubtext(gpu_, text); }

// ---- window procedures -----------------------------------------------------------------------------

LRESULT CALLBACK SettingsPanel::HostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<SettingsPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (self->content_) self->Relayout();
            return 0;
        case WM_MOUSEWHEEL:
            self->ScrollTo(self->scroll_ - GET_WHEEL_DELTA_WPARAM(wp) * self->Scale(60) / WHEEL_DELTA);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO si{sizeof(si), SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= self->Scale(30); break;
                case SB_LINEDOWN: pos += self->Scale(30); break;
                case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: pos = si.nTrackPos; break;
                case SB_TOP: pos = 0; break;
                case SB_BOTTOM: pos = si.nMax; break;
                default: break;
            }
            self->ScrollTo(pos);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK SettingsPanel::ContentProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<SettingsPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            // Paint only the invalid band (the content can be tall), double-buffered.
            const RECT& u = ps.rcPaint;
            const int bw = std::max(1L, u.right - u.left), bh = std::max(1L, u.bottom - u.top);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, bw, bh);
            HGDIOBJ old = SelectObject(mem, bmp);
            SetViewportOrgEx(mem, -u.left, -u.top, nullptr);
            self->PaintContent(mem, rc);
            SetViewportOrgEx(mem, 0, 0, nullptr);
            BitBlt(dc, u.left, u.top, bw, bh, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_COMMAND:
        case WM_HSCROLL:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            return SendMessageW(self->owner_, msg, wp, lp);  // the owner handles every control
        case WM_MOUSEWHEEL:
            return SendMessageW(self->host_, msg, wp, lp);
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace ixc::app
