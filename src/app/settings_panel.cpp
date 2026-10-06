#include "app/settings_panel.h"

#include "app/background_import.h"
#include "common/strings.h"
#include "effects/backgrounds.h"
#include "effects/effects.h"
#include "ixc/version.h"

#include <commctrl.h>
#include <commdlg.h>

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
    // One scrolling page per section; only the selected one is shown. Hiding a page hides its
    // controls without touching their own visibility (rows that come and go keep working).
    for (int i = 0; i < kPanelSections; ++i) {
        contents_[i] = CreateWindowExW(WS_EX_CONTROLPARENT, kContentClass, L"", WS_CHILD | WS_CLIPCHILDREN | (i == 0 ? WS_VISIBLE : 0), 0, 0, 0, 0,
                                       host_, nullptr, instance, this);
        if (!contents_[i]) return false;
    }
    if (!host_) return false;
    auto page = [&](PanelSection s) { content_ = contents_[static_cast<int>(s)]; };
    theme::ApplyDarkControl(host_, false);  // dark scroll bar

    const COLORREF card = theme::kSurface;
    auto button = [&](int id, const wchar_t* text, ButtonStyle style) { return CreateButton(content_, id, text, style, fonts_, card); };
    auto toggle = [&](int id, const wchar_t* text, const wchar_t* sub) {
        HWND h = CreateToggle(content_, id, text, fonts_, card);
        if (sub) SetWidgetSubtext(h, sub);
        return h;
    };

    // Profile
    page(PanelSection::Profiles);
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
    hotkeys_ = toggle(kIdHotkeys, L"Global hotkeys", L"Ctrl+Alt+F6/F7 lens · F8 effects · F9/F10 profile · F11 mirror");

    // Picture
    page(PanelSection::Adjust);
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
        {L"Noise reduction", 0, 100, 1.0, L"%.0f", IXC_IMAGE_FIELD(denoise)},
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
    page(PanelSection::Effects);
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
    page(PanelSection::Settings);
    smooth_ = toggle(kIdSmoothMotion, L"Smooth motion", L"Keeps the full frame rate in low light");
    autoFraming_ = toggle(kIdAutoFraming, L"Auto-framing", L"Zooms and pans to keep you in the picture");
    face_ = toggle(kIdFaceTracking, L"Face tracking", L"Off");
    faceMarkers_ = toggle(kIdFaceMarkers, L"Show face markers", L"Preview only, never sent to apps");
    auto choice = [&](int id, const wchar_t* label, std::initializer_list<const wchar_t*> items) {
        Choice ch;
        ch.label = CreateWindowExW(0, WC_STATICW, label, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, 0, 0, 0,
                                   content_, nullptr, instance, nullptr);
        SendMessageW(ch.label, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->body), FALSE);
        ch.combo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 0, 0, 0, 0, content_,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(ch.combo, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->body), FALSE);
        theme::ApplyDarkControl(ch.combo, true);
        SetWindowSubclass(ch.combo, ComboWheel, 1, 0);
        for (const wchar_t* t : items) SendMessageW(ch.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
        return ch;
    };
    processing_ = choice(kIdGpu, L"Processing", {L"Auto (best for this PC)", L"GPU", L"CPU"});
    antiFlicker_ = choice(kIdAntiFlicker, L"Anti-flicker", {L"Auto (your region)", L"50 Hz", L"60 Hz", L"Off"});
    diagnostics_ = toggle(kIdDiagnostics, L"Diagnostics", L"FPS, CPU/GPU use, memory, backends");
    diagText_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, content_, nullptr, instance, nullptr);
    SendMessageW(diagText_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->caption), FALSE);

    // Background
    page(PanelSection::Background);
    bgMode_ = choice(kIdBgMode, L"Background", {L"Original", L"Blur", L"Replace (built-in scene)", L"Solid colour", L"Custom picture"});
    bgBlur_ = choice(kIdBgBlur, L"Style", {L"Soft Blur", L"Standard Blur", L"DSLR Bokeh", L"Strong Bokeh", L"Custom"});
    bgAdvanced_ = toggle(kIdBgAdvanced, L"Advanced blur settings", L"Focus falloff, edges, stability, hair");
    bgBuiltin_ = choice(kIdBgBuiltin, L"Scene", {});
    for (const auto& b : effects::BuiltinBackgrounds()) SendMessageW(bgBuiltin_.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(b.name));
    bgCustom_ = choice(kIdBgCustom, L"Picture", {});
    bgFit_ = choice(kIdBgFit, L"Fit", {L"Fill (crop to the frame)", L"Fit (whole picture)"});
    bgColorLabel_ = CreateWindowExW(0, WC_STATICW, L"Colour", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, 0, 0, 0, 0, content_,
                                    nullptr, instance, nullptr);
    SendMessageW(bgColorLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->body), FALSE);
    bgColor_ = button(kIdBgColor, L"Pick colour…", ButtonStyle::Secondary);
    bgBrowse_ = button(kIdBgBrowse, L"Browse…", ButtonStyle::Secondary);
    bgRemove_ = button(kIdBgRemove, L"Remove", ButtonStyle::Danger);
    bgSliders_ = {
        {L"Picture zoom", 100, 300, 0.01, L"%.2f×", [](const Profile& p) { return p.background.scale; }, [](Profile& p, double v) { p.background.scale = v; }},
        {L"Horizontal position", 0, 100, 0.01, L"%.2f", [](const Profile& p) { return p.background.posX; }, [](Profile& p, double v) { p.background.posX = v; }},
        {L"Vertical position", 0, 100, 0.01, L"%.2f", [](const Profile& p) { return p.background.posY; }, [](Profile& p, double v) { p.background.posY = v; }},
        {L"Blur strength", 0, 100, 1.0, L"%", [](const Profile& p) { return p.background.strength; }, [](Profile& p, double v) { p.background.strength = v; }},
        {L"Focus falloff", 0, 100, 1.0, L"%", [](const Profile& p) { return p.background.falloff; }, [](Profile& p, double v) { p.background.falloff = v; }},
        {L"Edge feather", 0, 100, 1.0, L"%", [](const Profile& p) { return p.background.feather; }, [](Profile& p, double v) { p.background.feather = v; }},
        {L"Edge protection", 0, 100, 1.0, L"%", [](const Profile& p) { return p.background.edgeProtection; }, [](Profile& p, double v) { p.background.edgeProtection = v; }},
        {L"Temporal stability", 0, 100, 1.0, L"%", [](const Profile& p) { return p.background.temporal; }, [](Profile& p, double v) { p.background.temporal = v; }},
        {L"Hair refinement", 0, 100, 1.0, L"%", [](const Profile& p) { return p.background.hair; }, [](Profile& p, double v) { p.background.hair = v; }},
    };
    bgGroup_ = {0, 0, 0, 1, 2, 2, 2, 2, 2};
    for (size_t i = 0; i < bgSliders_.size(); ++i) {
        Slider& s = bgSliders_[i];
        const bool zoom = i == 0;
        const int def = static_cast<int>(std::lround(s.get(Profile{}) / s.scale));
        s.wnd = CreateSlider(content_, kIdBgSlider0 + static_cast<int>(i), s.label, s.min, s.max, def,
                             [zoom](int p) {
                                 wchar_t b[32];
                                 if (zoom) swprintf_s(b, L"%.2f×", p / 100.0);
                                 else swprintf_s(b, L"%d%%", p);
                                 return std::wstring(b);
                             },
                             fonts_, card);
    }

    // IXC Camera (system camera)
    page(PanelSection::Settings);
    vcamStatus_ = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, content_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdVcamStatus)), instance, nullptr);
    SendMessageW(vcamStatus_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->caption), FALSE);
    vcamUse_ = button(kIdVcamUse, L"Use this webcam for IXC Camera", ButtonStyle::Secondary);

    // Updates
    page(PanelSection::Updates);
    updateText_ = CreateWindowExW(0, WC_STATICW, L"IXC Camera " IXC_VERSION_STRING, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0,
                                  content_, nullptr, instance, nullptr);
    SendMessageW(updateText_, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_->caption), FALSE);
    updateCheck_ = button(kIdUpdateCheck, L"Check for updates", ButtonStyle::Secondary);
    updateInstall_ = button(kIdUpdateInstall, L"Update now", ButtonStyle::Primary);
    updateSkip_ = button(kIdUpdateSkip, L"Skip this version", ButtonStyle::Ghost);
    ShowWindow(updateInstall_, SW_HIDE);
    ShowWindow(updateSkip_, SW_HIDE);

    content_ = contents_[0];
    Refresh();
    return true;
}

bool SettingsPanel::IsContent(HWND h) const {
    for (HWND c : contents_)
        if (c && c == h) return true;
    return false;
}

void SettingsPanel::SetSection(PanelSection s) {
    if (s == section_) return;
    ShowWindow(content_, SW_HIDE);
    section_ = s;
    content_ = contents_[static_cast<int>(s)];
    scroll_ = 0;
    Relayout();
    ShowWindow(content_, SW_SHOW);
}

void SettingsPanel::SetUpdateState(const std::wstring& text, bool canInstall, bool canSkip, bool checking) {
    SetWindowTextW(updateText_, text.c_str());
    ShowWindow(updateInstall_, canInstall ? SW_SHOWNA : SW_HIDE);
    ShowWindow(updateSkip_, canSkip ? SW_SHOWNA : SW_HIDE);
    EnableWindow(updateCheck_, !checking);
    EnableWindow(updateInstall_, !checking);
    Relayout();
}

void SettingsPanel::SetBounds(const RECT& r) {
    // The main window lays out often (status hints come and go). Moving and re-laying out every
    // control when nothing changed made the panel flicker and could move a control while it was
    // being clicked, so only real changes are applied.
    if (EqualRect(&r, &bounds_)) return;
    bounds_ = r;
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
    int ys[kPanelSections];
    for (int& v : ys) v = pad;
    int y = pad, cur = 0;
    cards_.clear();
    HDWP dwp = BeginDeferWindowPos(120);
    auto place = [&](HWND h, int px, int py, int pw, int ph) {
        if (dwp) dwp = DeferWindowPos(dwp, h, nullptr, px, py, pw, ph, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto row = [&](HWND h) {
        const int hh = WidgetHeight(h);
        place(h, x, y, w, hh);
        y += hh + gap;
    };
    auto beginCard = [&](const wchar_t* title, HWND headerButton, PanelSection section) {
        cur = static_cast<int>(section);
        y = ys[cur];  // each section's page is laid out on its own
        cards_.push_back({RECT{pad, y, width - pad, y}, title, cur});
        if (headerButton) place(headerButton, x + w - Scale(76), y + Scale(5), Scale(76), Scale(26));
        y += titleH + Scale(4);
    };
    auto endCard = [&] {
        y += inner - gap;
        cards_.back().rc.bottom = y;
        y += cardGap;
        ys[cur] = y;
    };

    beginCard(L"PROFILE", nullptr, PanelSection::Profiles);
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

    beginCard(L"PICTURE", reset_, PanelSection::Adjust);
    for (auto& s : sliders_) row(s.wnd);
    row(mirror_);
    endCard();

    beginCard(L"EFFECTS", effectsAllOff_, PanelSection::Effects);
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

    const int labelW = std::min(Scale(130), w * 2 / 5);
    auto choiceRow = [&](const Choice& ch) {
        if (!(GetWindowLongPtrW(ch.combo, GWL_STYLE) & WS_VISIBLE)) return;
        place(ch.label, x, y, labelW - gap, Scale(30));
        place(ch.combo, x + labelW, y, w - labelW, Scale(300));  // height includes the drop-down list
        y += Scale(30) + gap;
    };
    auto visible = [](HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0; };
    beginCard(L"BACKGROUND", nullptr, PanelSection::Background);
    choiceRow(bgMode_);
    choiceRow(bgBlur_);
    if (visible(bgAdvanced_)) {
        for (size_t i = 0; i < bgSliders_.size(); ++i)
            if (bgGroup_[i] == 1 && visible(bgSliders_[i].wnd)) row(bgSliders_[i].wnd);
        row(bgAdvanced_);
        for (size_t i = 0; i < bgSliders_.size(); ++i)
            if (bgGroup_[i] == 2 && visible(bgSliders_[i].wnd)) row(bgSliders_[i].wnd);
    }
    choiceRow(bgBuiltin_);
    if (visible(bgColor_)) {
        place(bgColorLabel_, x, y, labelW - gap, Scale(30));
        place(bgColor_, x + labelW, y, w - labelW, Scale(30));
        y += Scale(30) + gap;
    }
    choiceRow(bgCustom_);
    if (visible(bgBrowse_)) {
        const int half = (w - labelW - gap) / 2;
        place(bgBrowse_, x + labelW, y, half, Scale(30));
        place(bgRemove_, x + labelW + half + gap, y, w - labelW - half - gap, Scale(30));
        y += Scale(30) + gap;
    }
    choiceRow(bgFit_);
    for (size_t i = 0; i < bgSliders_.size(); ++i)
        if (bgGroup_[i] == 0 && visible(bgSliders_[i].wnd)) row(bgSliders_[i].wnd);
    endCard();

    beginCard(L"CAMERA FEATURES", nullptr, PanelSection::Settings);
    row(smooth_);
    row(autoFraming_);
    row(face_);
    place(faceMarkers_, x + Scale(14), y, w - Scale(14), WidgetHeight(faceMarkers_));
    y += WidgetHeight(faceMarkers_) + gap;
    choiceRow(processing_);
    choiceRow(antiFlicker_);
    row(diagnostics_);
    if (diagnosticsOn_) {
        wchar_t text[1024];
        GetWindowTextW(diagText_, text, 1024);
        HDC dc = GetDC(content_);
        HGDIOBJ old = SelectObject(dc, fonts_->caption);
        RECT calc{0, 0, w, 0};
        DrawTextW(dc, text[0] ? text : L"X", -1, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(content_, dc);
        place(diagText_, x, y, w, calc.bottom);
        y += calc.bottom + gap;
    }
    endCard();

    beginCard(L"IXC CAMERA", nullptr, PanelSection::Settings);
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

    beginCard(L"UPDATES", nullptr, PanelSection::Updates);
    {
        wchar_t text[2048];
        GetWindowTextW(updateText_, text, 2048);
        HDC dc = GetDC(content_);
        HGDIOBJ old = SelectObject(dc, fonts_->caption);
        RECT calc{0, 0, w, 0};
        DrawTextW(dc, text[0] ? text : L"X", -1, &calc, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(content_, dc);
        place(updateText_, x, y, w, calc.bottom);
        y += calc.bottom + gap + Scale(2);
    }
    for (HWND b : {updateInstall_, updateCheck_, updateSkip_}) {
        if (!(GetWindowLongPtrW(b, GWL_STYLE) & WS_VISIBLE)) continue;
        place(b, x, y, w, Scale(32));
        y += Scale(32) + gap;
    }
    endCard();

    for (int i = 0; i < kPanelSections; ++i) heights_[i] = ys[i] + Scale(18) + pad;  // room for the "about" line at the bottom
    contentHeight_ = heights_[static_cast<int>(section_)];
    if (dwp) EndDeferWindowPos(dwp);
    for (int i = 0; i < kPanelSections; ++i) {
        RECT hostRc;
        GetClientRect(host_, &hostRc);
        if (contents_[i] != content_) SetWindowPos(contents_[i], nullptr, 0, 0, width_, std::max(heights_[i], static_cast<int>(hostRc.bottom)), SWP_NOZORDER | SWP_NOACTIVATE);
    }

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

void SettingsPanel::PaintContent(HWND content, HDC dc, const RECT& rc) {
    using namespace theme;
    FillRect(dc, &rc, Brush(kBg));
    int section = 0;
    for (int i = 0; i < kPanelSections; ++i)
        if (contents_[i] == content) section = i;
    const int pageHeight = heights_[section];
    for (const Card& c : cards_) {
        if (c.section != section) continue;
        FillRound(dc, c.rc, Scale(10), kSurface, kBorder);
        RECT t{c.rc.left + Scale(14), c.rc.top + Scale(4), c.rc.right - Scale(90), c.rc.top + Scale(34)};
        DrawTextIn(dc, c.title, t, fonts_->section, kTextDim, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT about{Scale(12), pageHeight - Scale(36), rc.right - Scale(12), pageHeight - Scale(12)};
    DrawTextIn(dc, L"IXC Camera " IXC_VERSION_STRING L" · all processing stays on this PC", about, fonts_->caption, kTextFaint,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void SettingsPanel::Refresh() {
    if (!profile_) return;
    for (auto& s : sliders_) SendMessageW(s.wnd, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(s.get(*profile_) / s.scale)));
    SendMessageW(mirror_, BM_SETCHECK, profile_->mirror ? BST_CHECKED : BST_UNCHECKED, 0);
    for (auto& s : bgSliders_) SendMessageW(s.wnd, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(s.get(*profile_) / s.scale)));
    const int procSel = profile_->processing == ProcessingMode::Gpu ? 1 : profile_->processing == ProcessingMode::Cpu ? 2 : 0;
    SendMessageW(processing_.combo, CB_SETCURSEL, static_cast<WPARAM>(procSel), 0);
    SendMessageW(antiFlicker_.combo, CB_SETCURSEL, static_cast<WPARAM>(profile_->antiFlicker), 0);
    SendMessageW(diagnostics_, BM_SETCHECK, diagnosticsOn_ ? BST_CHECKED : BST_UNCHECKED, 0);
    const BackgroundSettings& bg = profile_->background;
    SendMessageW(bgMode_.combo, CB_SETCURSEL, static_cast<WPARAM>(bg.mode), 0);
    SendMessageW(bgBlur_.combo, CB_SETCURSEL, static_cast<WPARAM>(bg.preset), 0);
    SendMessageW(bgAdvanced_, BM_SETCHECK, bgAdvancedOn_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(bgFit_.combo, CB_SETCURSEL, static_cast<WPARAM>(bg.fit), 0);
    const auto& builtins = effects::BuiltinBackgrounds();
    for (size_t i = 0; i < builtins.size(); ++i)
        if (bg.builtin == builtins[i].id) SendMessageW(bgBuiltin_.combo, CB_SETCURSEL, i, 0);
    recent_ = RecentBackgrounds();
    if (!bg.image.empty() && std::find(recent_.begin(), recent_.end(), bg.image) == recent_.end()) recent_.insert(recent_.begin(), bg.image);
    SendMessageW(bgCustom_.combo, CB_RESETCONTENT, 0, 0);
    for (const auto& n : recent_) SendMessageW(bgCustom_.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utf8ToWide(n).c_str()));
    for (size_t i = 0; i < recent_.size(); ++i)
        if (recent_[i] == bg.image) SendMessageW(bgCustom_.combo, CB_SETCURSEL, i, 0);
    wchar_t hex[48];
    swprintf_s(hex, L"Colour  #%06X", static_cast<unsigned>(bg.color & 0xFFFFFF));
    SetWindowTextW(bgColorLabel_, hex);
    UpdateBackgroundRows();
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
    for (size_t i = 0; i < bgSliders_.size(); ++i) {
        Slider& s = bgSliders_[i];
        if (s.wnd != control) continue;
        const double v = static_cast<double>(SendMessageW(s.wnd, TBM_GETPOS, 0, 0)) * s.scale;
        if (v != s.get(*profile_)) {
            s.set(*profile_, v);
            if (bgGroup_[i] != 0 && profile_->background.preset != BlurPreset::Custom) {  // hand-tuned blur
                profile_->background.preset = BlurPreset::Custom;
                SendMessageW(bgBlur_.combo, CB_SETCURSEL, static_cast<WPARAM>(BlurPreset::Custom), 0);
            }
            Changed();
        }
        return true;
    }
    for (auto& s : sliders_) {
        if (s.wnd != control) continue;
        const double v = static_cast<double>(SendMessageW(s.wnd, TBM_GETPOS, 0, 0)) * s.scale;
        if (v != s.get(*profile_)) {
            s.set(*profile_, v);
            Changed();
        }
        return true;
    }
    if (IsContent(GetParent(control))) {
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
    if (!IsContent(GetParent(control))) return false;
    if (code == CBN_SELCHANGE) {
        const LRESULT sel = SendMessageW(control, CB_GETCURSEL, 0, 0);
        return sel != CB_ERR && OnChoice(GetDlgCtrlID(control), static_cast<int>(sel));
    }
    if (code != BN_CLICKED) return false;
    const auto checked = [&](HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    const int id = GetDlgCtrlID(control);
    switch (id) {
        case kIdMirror: profile_->mirror = checked(mirror_); break;
        case kIdDiagnostics:
            diagnosticsOn_ = checked(diagnostics_);
            ShowWindow(diagText_, diagnosticsOn_ ? SW_SHOWNA : SW_HIDE);
            Relayout();
            return true;  // app-only view: nothing to save
        case kIdBgBrowse:
            if (!BrowseBackground()) return true;
            break;
        case kIdBgRemove: {
            BackgroundSettings& bg = profile_->background;
            if (!bg.image.empty()) RemoveBackground(bg.image);
            bg.image.clear();
            const auto left = RecentBackgrounds();
            if (!left.empty()) bg.image = left.front();
            else bg.mode = BackgroundMode::Blur;  // no picture left: never a silent reset to Original
            Refresh();
            break;
        }
        case kIdBgColor:
            PickColor();
            return true;
        case kIdBgAdvanced:
            bgAdvancedOn_ = checked(bgAdvanced_);
            UpdateBackgroundRows();
            return true;  // view only
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

// Drop-down lists. False for ids this panel doesn't own (the profile list is the owner's).
bool SettingsPanel::OnChoice(int id, int sel) {
    BackgroundSettings& bg = profile_->background;
    switch (id) {
        case kIdGpu: profile_->processing = sel == 1 ? ProcessingMode::Gpu : sel == 2 ? ProcessingMode::Cpu : ProcessingMode::Auto; break;
        case kIdAntiFlicker: profile_->antiFlicker = static_cast<AntiFlicker>(std::clamp(sel, 0, 3)); break;
        case kIdBgMode: {
            const auto mode = static_cast<BackgroundMode>(std::clamp(sel, 0, 4));
            if (mode == BackgroundMode::Custom && bg.image.empty()) {
                if (!recent_.empty()) bg.image = recent_.front();
                else if (!BrowseBackground()) {  // cancelled: stay on the current mode
                    Refresh();
                    return true;
                }
            }
            bg.mode = mode;
            break;
        }
        case kIdBgBlur:
            ApplyBlurPreset(bg, static_cast<BlurPreset>(std::clamp(sel, 0, 4)));
            Refresh();  // the sliders show the preset's values
            break;
        case kIdBgFit: bg.fit = sel == 1 ? BackgroundFit::Fit : BackgroundFit::Fill; break;
        case kIdBgBuiltin: {
            const auto& list = effects::BuiltinBackgrounds();
            if (sel >= static_cast<int>(list.size())) return true;
            bg.builtin = list[static_cast<size_t>(sel)].id;
            break;
        }
        case kIdBgCustom:
            if (sel >= static_cast<int>(recent_.size())) return true;
            bg.image = recent_[static_cast<size_t>(sel)];
            TouchBackground(bg.image);
            break;
        default: return false;
    }
    UpdateBackgroundRows();
    Changed();
    return true;
}

// Browse… : imports a picture and selects it (Custom mode). False when cancelled or failed.
bool SettingsPanel::BrowseBackground() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = owner_;
    ofn.lpstrFilter = L"Pictures (JPG, PNG, WEBP, BMP)\0*.jpg;*.jpeg;*.png;*.webp;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose a background picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring error;
    const std::string name = ImportBackground(file, error);
    SetCursor(old);
    if (name.empty()) {
        MessageBoxW(owner_, error.c_str(), L"IXC Camera", MB_OK | MB_ICONWARNING);
        return false;
    }
    profile_->background.image = name;
    profile_->background.mode = BackgroundMode::Custom;
    Refresh();
    return true;
}

void SettingsPanel::PickColor() {
    static COLORREF custom[16] = {};
    const std::uint32_t rgb = profile_->background.color;
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = owner_;
    cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    profile_->background.color = (static_cast<std::uint32_t>(GetRValue(cc.rgbResult)) << 16) | (static_cast<std::uint32_t>(GetGValue(cc.rgbResult)) << 8) |
                                 GetBValue(cc.rgbResult);
    profile_->background.mode = BackgroundMode::Color;
    Refresh();
    Changed();
}

// Shows only the rows the current background mode uses.
void SettingsPanel::UpdateBackgroundRows() {
    const BackgroundMode m = profile_->background.mode;
    const bool picture = m == BackgroundMode::Replace || m == BackgroundMode::Custom;
    bool changed = false;
    auto show = [&](HWND hwnd, bool on) {
        const bool now = (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;
        if (now != on) {
            ShowWindow(hwnd, on ? SW_SHOWNA : SW_HIDE);
            changed = true;
        }
    };
    auto showChoice = [&](const Choice& ch, bool on) {
        show(ch.label, on);
        show(ch.combo, on);
    };
    showChoice(bgBlur_, m == BackgroundMode::Blur);
    showChoice(bgBuiltin_, m == BackgroundMode::Replace);
    show(bgColorLabel_, m == BackgroundMode::Color);
    show(bgColor_, m == BackgroundMode::Color);
    showChoice(bgCustom_, m == BackgroundMode::Custom);
    show(bgBrowse_, m == BackgroundMode::Custom);
    show(bgRemove_, m == BackgroundMode::Custom);
    showChoice(bgFit_, picture);
    const bool blur = m == BackgroundMode::Blur;
    show(bgAdvanced_, blur);
    for (size_t i = 0; i < bgSliders_.size(); ++i)
        show(bgSliders_[i].wnd, bgGroup_[i] == 0 ? picture : bgGroup_[i] == 1 ? blur : blur && bgAdvancedOn_);
    if (changed) Relayout();
}

void SettingsPanel::SetDiagnosticsText(const std::wstring& text) {
    wchar_t old[1024];
    GetWindowTextW(diagText_, old, 1024);
    if (text == old) return;
    const auto lines = [](const std::wstring& s) { return std::count(s.begin(), s.end(), L'\n'); };
    const bool relayout = lines(text) != lines(old);
    SetWindowTextW(diagText_, text.c_str());
    if (relayout) Relayout();
}

void SettingsPanel::SetFaceOverlay(bool on) {
    faceOverlay_ = on;
    SendMessageW(faceMarkers_, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

void SettingsPanel::SetFaceTrackingState(const std::wstring& text) { SetWidgetSubtext(face_, text); }


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
            NotePanelScrolled();  // sliders under the pointer don't grab the wheel mid-scroll
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
            self->PaintContent(h, mem, rc);
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
