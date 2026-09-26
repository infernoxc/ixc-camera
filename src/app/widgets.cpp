#include "app/widgets.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <map>
#include <memory>

namespace ixc::app {

namespace theme {

HBRUSH Brush(COLORREF c) {
    static std::map<COLORREF, HBRUSH> cache;  // UI thread only
    auto it = cache.find(c);
    if (it != cache.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    cache.emplace(c, b);
    return b;
}

COLORREF Mix(COLORREF a, COLORREF b, int t) {
    auto ch = [&](int shift) { return (((a >> shift) & 0xFF) * (256 - t) + ((b >> shift) & 0xFF) * t) >> 8; };
    return RGB(ch(0), ch(8), ch(16));
}

void Fonts::Create(UINT dpi) {
    Destroy();
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfQuality = CLEARTYPE_QUALITY;
    auto make = [&](int pt, int weight) {
        LOGFONTW f = lf;
        f.lfHeight = -MulDiv(pt, static_cast<int>(dpi), 72);
        f.lfWeight = weight;
        return CreateFontIndirectW(&f);
    };
    body = make(10, FW_NORMAL);
    bold = make(10, FW_SEMIBOLD);
    caption = make(9, FW_NORMAL);
    section = make(9, FW_BOLD);
    title = make(13, FW_SEMIBOLD);
}

void Fonts::Destroy() {
    for (HFONT* f : {&body, &bold, &caption, &section, &title}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border) {
    HGDIOBJ oldBrush = SelectObject(dc, Brush(fill));
    HPEN pen = CreatePen(PS_SOLID, 1, border == CLR_INVALID ? fill : border);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void DrawTextIn(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, format | DT_NOPREFIX);
    SelectObject(dc, old);
}

void ApplyDarkWindow(HWND top) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(top, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = kBg;
    DwmSetWindowAttribute(top, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));
}

void ApplyDarkControl(HWND control, bool comboBox) {
    SetWindowTheme(control, comboBox ? L"DarkMode_CFD" : L"DarkMode_Explorer", nullptr);
}

}  // namespace theme

namespace {

constexpr wchar_t kClass[] = L"IXCWidget";

enum class Kind { Toggle, Slider, Button };

struct Widget {
    Kind kind = Kind::Button;
    ButtonStyle style = ButtonStyle::Secondary;
    const theme::Fonts* fonts = nullptr;
    COLORREF bg = theme::kSurface;
    std::wstring sub;
    bool checked = false;
    bool hover = false, pressed = false, dragging = false;
    int min = 0, max = 100, pos = 0, def = 0;
    std::function<std::wstring(int)> format;
};

Widget* Get(HWND h) { return reinterpret_cast<Widget*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }
int Dpi(HWND h, int v) { return MulDiv(v, static_cast<int>(GetDpiForWindow(h)), 96); }

std::wstring Text(HWND h) {
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);
    return buf;
}

void Notify(HWND h, UINT msg, WPARAM wp) { SendMessageW(GetParent(h), msg, wp, reinterpret_cast<LPARAM>(h)); }

// Slider geometry: track rectangle (client coordinates).
RECT TrackRect(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    const int thumb = Dpi(h, 16);
    const int y = rc.bottom - Dpi(h, 11);
    return {rc.left + thumb / 2, y - Dpi(h, 2), rc.right - thumb / 2, y + Dpi(h, 2)};
}

int PosFromX(HWND h, Widget* w, int x) {
    const RECT t = TrackRect(h);
    const int span = std::max(1L, t.right - t.left);
    const double f = std::clamp(static_cast<double>(x - t.left) / span, 0.0, 1.0);
    return w->min + static_cast<int>(std::lround(f * (w->max - w->min)));
}

void SetPos(HWND h, Widget* w, int p, bool notify, bool end) {
    p = std::clamp(p, w->min, w->max);
    const bool changed = p != w->pos;
    w->pos = p;
    if (changed) InvalidateRect(h, nullptr, FALSE);
    if (notify && (changed || end)) Notify(h, WM_HSCROLL, MAKEWPARAM(end ? TB_ENDTRACK : TB_THUMBTRACK, p));
}

void Click(HWND h, Widget* w) {
    if (!IsWindowEnabled(h)) return;
    if (w->kind == Kind::Toggle) {
        w->checked = !w->checked;
        InvalidateRect(h, nullptr, FALSE);
    }
    Notify(h, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED));
}

void Paint(HWND h, Widget* w, HDC dc, const RECT& rc) {
    using namespace theme;
    FillRect(dc, &rc, Brush(w->bg));
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const bool focused = GetFocus() == h;
    auto dim = [&](COLORREF c) { return enabled ? c : Mix(c, w->bg, 150); };
    const std::wstring label = Text(h);

    if (w->kind == Kind::Button) {
        const int r = Dpi(h, 6);
        COLORREF fill = kSurfaceHi, border = kBorder, text = kText;
        switch (w->style) {
            case ButtonStyle::Primary:
                fill = w->pressed ? kAccentLo : (w->hover ? kAccentHi : kAccent);
                border = fill;
                text = RGB(255, 255, 255);
                break;
            case ButtonStyle::Secondary:
                fill = w->pressed ? kSurface : (w->hover ? Mix(kSurfaceHi, kText, 20) : kSurfaceHi);
                break;
            case ButtonStyle::Ghost:
                fill = w->hover || w->pressed ? kSurfaceHi : w->bg;
                border = fill;
                text = kAccentHi;
                break;
            case ButtonStyle::Danger:
                fill = w->hover || w->pressed ? Mix(w->bg, kBad, 40) : w->bg;
                border = kBad;
                text = kBad;
                break;
        }
        RECT b = rc;
        InflateRect(&b, -1, -1);
        FillRound(dc, b, r, dim(fill), focused ? kAccentHi : dim(border));
        DrawTextIn(dc, label, rc, w->fonts->bold, dim(text), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }

    if (w->kind == Kind::Toggle) {
        const int sw = Dpi(h, 38), sh = Dpi(h, 20), pad = Dpi(h, 2);
        const RECT sr{rc.right - sw - 1, (rc.top + rc.bottom - sh) / 2, rc.right - 1, (rc.top + rc.bottom + sh) / 2};
        const COLORREF track = w->checked ? (w->hover ? kAccentHi : kAccent) : (w->hover ? Mix(kSurfaceHi, kText, 25) : kSurfaceHi);
        FillRound(dc, sr, sh / 2, dim(track), focused ? kAccentHi : dim(w->checked ? track : kBorder));
        const int knob = sh - 2 * pad - 2;
        const int kx = w->checked ? sr.right - pad - 1 - knob : sr.left + pad + 1;
        const RECT kr{kx, sr.top + pad + 1, kx + knob, sr.top + pad + 1 + knob};
        FillRound(dc, kr, knob / 2, dim(w->checked ? RGB(255, 255, 255) : kTextDim), CLR_INVALID);
        RECT tr = rc;
        tr.right = sr.left - Dpi(h, 10);
        if (w->sub.empty()) {
            DrawTextIn(dc, label, tr, w->fonts->body, dim(kText), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            RECT top = tr, bottom = tr;
            top.bottom = (rc.top + rc.bottom) / 2 + Dpi(h, 1);
            bottom.top = top.bottom;
            DrawTextIn(dc, label, top, w->fonts->body, dim(kText), DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextIn(dc, w->sub, bottom, w->fonts->caption, dim(kTextDim), DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        return;
    }

    // Slider: label + value on the first line, track + thumb below.
    RECT line = rc;
    line.bottom = rc.top + Dpi(h, 20);
    const std::wstring value = w->format ? w->format(w->pos) : std::to_wstring(w->pos);
    DrawTextIn(dc, label, line, w->fonts->body, dim(kText), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawTextIn(dc, value, line, w->fonts->bold, dim(w->pos != w->def ? kAccentHi : kTextDim), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    const RECT t = TrackRect(h);
    FillRound(dc, t, Dpi(h, 2), dim(kSurfaceHi), CLR_INVALID);
    const double span = std::max(1, w->max - w->min);
    auto xOf = [&](int p) { return t.left + static_cast<int>(std::lround((p - w->min) / span * (t.right - t.left))); };
    const int x = xOf(w->pos);
    const int origin = xOf(std::clamp(w->def, w->min, w->max));  // fill from the default (centre for ± sliders)
    RECT fill{std::min(x, origin), t.top, std::max(x, origin), t.bottom};
    if (fill.right > fill.left) FillRound(dc, fill, Dpi(h, 2), dim(kAccent), CLR_INVALID);
    const int tr = Dpi(h, w->dragging || w->hover ? 8 : 7);
    const int cy = (t.top + t.bottom) / 2;
    FillRound(dc, RECT{x - tr, cy - tr, x + tr, cy + tr}, tr, dim(RGB(255, 255, 255)), focused ? kAccentHi : dim(kAccent));
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Widget* w = Get(h);
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_NCDESTROY:
            delete w;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bmp);
            if (w) Paint(h, w, mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETTEXT:
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS: {
            const LRESULT r = DefWindowProcW(h, msg, wp, lp);
            InvalidateRect(h, nullptr, FALSE);
            return r;
        }
        case WM_GETDLGCODE:
            return w && w->kind == Kind::Slider ? DLGC_WANTARROWS : DLGC_BUTTON | (w && w->kind == Kind::Button ? DLGC_UNDEFPUSHBUTTON : 0);
        case WM_MOUSEMOVE: {
            if (!w) break;
            if (!w->hover) {
                w->hover = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(h, nullptr, FALSE);
            }
            if (w->dragging) SetPos(h, w, PosFromX(h, w, GET_X_LPARAM(lp)), true, false);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (w) {
                w->hover = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (!w || !IsWindowEnabled(h)) return 0;
            SetFocus(h);
            SetCapture(h);
            w->pressed = true;
            if (w->kind == Kind::Slider) {
                w->dragging = true;
                SetPos(h, w, PosFromX(h, w, GET_X_LPARAM(lp)), true, false);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (!w || !w->pressed) return 0;
            ReleaseCapture();
            w->pressed = false;
            if (w->kind == Kind::Slider) {
                w->dragging = false;
                SetPos(h, w, w->pos, true, true);
            } else {
                RECT rc;
                GetClientRect(h, &rc);
                const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (PtInRect(&rc, p)) Click(h, w);
            }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            // Double-click a slider: back to its default value.
            if (w && w->kind == Kind::Slider && IsWindowEnabled(h)) SetPos(h, w, w->def, true, true);
            else if (w && w->kind != Kind::Slider) SendMessageW(h, WM_LBUTTONDOWN, wp, lp);
            return 0;
        case WM_CAPTURECHANGED:
            if (w && w->dragging) {
                w->dragging = false;
                w->pressed = false;
                SetPos(h, w, w->pos, true, true);
            }
            return 0;
        case WM_KEYDOWN:
            if (!w) break;
            if (w->kind == Kind::Slider) {
                const int page = std::max(1, (w->max - w->min) / 20);
                int p = w->pos;
                switch (wp) {
                    case VK_LEFT: case VK_DOWN: p -= 1; break;
                    case VK_RIGHT: case VK_UP: p += 1; break;
                    case VK_PRIOR: p += page; break;
                    case VK_NEXT: p -= page; break;
                    case VK_HOME: p = w->min; break;
                    case VK_END: p = w->max; break;
                    case VK_DELETE: case VK_BACK: p = w->def; break;
                    default: return DefWindowProcW(h, msg, wp, lp);
                }
                SetPos(h, w, p, true, true);
                return 0;
            }
            if (wp == VK_SPACE || wp == VK_RETURN) {
                Click(h, w);
                return 0;
            }
            break;
        case BM_CLICK:
            if (w) Click(h, w);
            return 0;
        case BM_GETCHECK:
            return w && w->checked ? BST_CHECKED : BST_UNCHECKED;
        case BM_SETCHECK:
            if (w && w->checked != (wp == BST_CHECKED)) {
                w->checked = wp == BST_CHECKED;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case TBM_GETPOS:
            return w ? w->pos : 0;
        case TBM_SETPOS:
            if (w) SetPos(h, w, static_cast<int>(lp), false, false);
            return 0;
        case TBM_SETRANGEMIN:
            if (w) w->min = static_cast<int>(lp);
            return 0;
        case TBM_SETRANGEMAX:
            if (w) w->max = static_cast<int>(lp);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND Make(HWND parent, int id, const wchar_t* text, Widget* w) {
    HWND h = CreateWindowExW(0, kClass, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                             w);
    if (!h) delete w;
    return h;
}

}  // namespace

bool RegisterWidgetClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WidgetProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateToggle(HWND parent, int id, const wchar_t* label, const theme::Fonts* fonts, COLORREF background) {
    auto* w = new Widget;
    w->kind = Kind::Toggle;
    w->fonts = fonts;
    w->bg = background;
    return Make(parent, id, label, w);
}

HWND CreateSlider(HWND parent, int id, const wchar_t* label, int min, int max, int defaultPos, std::function<std::wstring(int)> format,
                  const theme::Fonts* fonts, COLORREF background) {
    auto* w = new Widget;
    w->kind = Kind::Slider;
    w->fonts = fonts;
    w->bg = background;
    w->min = min;
    w->max = max;
    w->pos = w->def = std::clamp(defaultPos, min, max);
    w->format = std::move(format);
    return Make(parent, id, label, w);
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, ButtonStyle style, const theme::Fonts* fonts, COLORREF background) {
    auto* w = new Widget;
    w->kind = Kind::Button;
    w->style = style;
    w->fonts = fonts;
    w->bg = background;
    return Make(parent, id, text, w);
}

void SetWidgetSubtext(HWND widget, const std::wstring& text) {
    Widget* w = Get(widget);
    if (!w || w->sub == text) return;
    w->sub = text;
    InvalidateRect(widget, nullptr, FALSE);
}

int WidgetHeight(HWND widget) {
    Widget* w = Get(widget);
    if (!w) return Dpi(widget, 32);
    switch (w->kind) {
        case Kind::Toggle: return Dpi(widget, w->sub.empty() ? 30 : 38);
        case Kind::Slider: return Dpi(widget, 38);
        case Kind::Button: return Dpi(widget, 32);
    }
    return Dpi(widget, 32);
}

}  // namespace ixc::app
