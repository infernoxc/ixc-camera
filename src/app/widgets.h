#pragma once

// IXC Camera's dark UI toolkit: a colour palette, DPI-aware fonts and three owner-painted
// controls (toggle switch, slider, button).
//
// The controls behave like the standard ones towards their parent and towards automation:
//   * toggle/button: BM_CLICK, BM_GETCHECK/BM_SETCHECK; the parent gets WM_COMMAND(BN_CLICKED);
//   * slider: TBM_SETRANGEMIN/MAX, TBM_SETPOS/TBM_GETPOS; the parent gets WM_HSCROLL
//     (TB_THUMBTRACK while dragging, TB_ENDTRACK when released or after a key press).
// The label is the window text (SetWindowText/GetWindowText). Everything is painted
// double-buffered on demand; nothing animates or polls.

#include <windows.h>

#include <functional>
#include <string>

namespace ixc::app {

namespace theme {
// COLORREF is 0x00BBGGRR: always use RGB().
inline constexpr COLORREF kBg = RGB(17, 19, 23);          // window background
inline constexpr COLORREF kSurface = RGB(27, 30, 36);     // cards
inline constexpr COLORREF kSurfaceHi = RGB(38, 42, 50);   // inputs, secondary buttons, switch track (off)
inline constexpr COLORREF kBorder = RGB(50, 55, 64);
inline constexpr COLORREF kText = RGB(233, 235, 238);
inline constexpr COLORREF kTextDim = RGB(158, 164, 174);
inline constexpr COLORREF kTextFaint = RGB(104, 110, 121);
inline constexpr COLORREF kAccent = RGB(124, 108, 242);   // IXC violet
inline constexpr COLORREF kAccentHi = RGB(148, 134, 255);
inline constexpr COLORREF kAccentLo = RGB(98, 84, 210);
inline constexpr COLORREF kRose = RGB(255, 110, 158);     // brand second colour
inline constexpr COLORREF kGood = RGB(62, 207, 125);
inline constexpr COLORREF kWarn = RGB(240, 180, 70);
inline constexpr COLORREF kBad = RGB(238, 86, 94);

// Cached solid brushes (a handful of colours, kept for the process lifetime).
HBRUSH Brush(COLORREF c);
COLORREF Mix(COLORREF a, COLORREF b, int t256);  // t256 = weight of b (0..256)

struct Fonts {
    HFONT body = nullptr, bold = nullptr, caption = nullptr, section = nullptr, title = nullptr;
    void Create(UINT dpi);
    void Destroy();
    ~Fonts() { Destroy(); }
};

void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF border);
void DrawTextIn(HDC dc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT format);
// Dark title bar and dark scroll bars / combo boxes (Windows 11 themes).
void ApplyDarkWindow(HWND top);
void ApplyDarkControl(HWND control, bool comboBox);
}  // namespace theme

enum class ButtonStyle { Primary, Secondary, Ghost, Danger };

HWND CreateToggle(HWND parent, int id, const wchar_t* label, const theme::Fonts* fonts, COLORREF background);
HWND CreateSlider(HWND parent, int id, const wchar_t* label, int min, int max, int defaultPos, std::function<std::wstring(int)> format,
                  const theme::Fonts* fonts, COLORREF background);
HWND CreateButton(HWND parent, int id, const wchar_t* text, ButtonStyle style, const theme::Fonts* fonts, COLORREF background);

// Second, dimmer line under a toggle's label (e.g. its live state). Empty = one line.
void SetWidgetSubtext(HWND widget, const std::wstring& text);
// Preferred height of a widget at the window's DPI.
int WidgetHeight(HWND widget);

bool RegisterWidgetClass(HINSTANCE instance);

}  // namespace ixc::app
