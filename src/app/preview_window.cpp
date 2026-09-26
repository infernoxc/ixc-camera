#include "app/preview_window.h"

#include <mfapi.h>

#include <algorithm>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace ixc::app {

namespace {

constexpr wchar_t kClass[] = L"IXCPreview";

processing::YuvFormat YuvFormatFor(const camera::FrameLayout& l) {
    processing::YuvFormat f = processing::DefaultYuvFormat(l.height);
    if (l.yuvMatrix == MFVideoTransferMatrix_BT709) f.matrix = processing::YuvMatrix::Bt709;
    else if (l.yuvMatrix == MFVideoTransferMatrix_BT601) f.matrix = processing::YuvMatrix::Bt601;
    if (l.nominalRange == MFNominalRange_0_255) f.fullRange = true;
    else if (l.nominalRange == MFNominalRange_16_235) f.fullRange = false;
    return f;
}

}  // namespace

bool PreviewWindow::RegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &PreviewWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted entirely in WM_PAINT (avoids flicker)
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND PreviewWindow::Create(HWND parent, HINSTANCE instance, int id) {
    hwnd_ = CreateWindowExW(0, kClass, L"Camera preview", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, this);
    return hwnd_;
}

void PreviewWindow::NotifyFrameAvailable() {
    bool expected = false;
    if (framePending_.compare_exchange_strong(expected, true)) {
        if (!PostMessageW(GetParent(hwnd_), kFrameMessage, 0, 0)) framePending_.store(false);
    }
}

void PreviewWindow::ShowFrame(ComPtr<IMFSample> sample, const camera::FrameLayout& layout) {
    sample_ = std::move(sample);
    layout_ = layout;
    dirty_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::Clear(const wchar_t* placeholder) {
    sample_.Reset();
    dirty_ = false;
    std::vector<std::uint32_t>().swap(bgra_);  // give the memory back while idle
    bgraW_ = bgraH_ = 0;
    wcsncpy_s(placeholder_, placeholder ? placeholder : L"", _TRUNCATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::SetMirror(bool mirror) {
    if (mirror_ != mirror) {
        mirror_ = mirror;
        dirty_ = sample_ != nullptr;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool PreviewWindow::ConvertHeldFrame(int dstW, int dstH) {
    if (!sample_ || !IsEqualGUID(layout_.subtype, MFVideoFormat_NV12) || layout_.width < 2 || layout_.height < 2) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample_->GetBufferByIndex(0, &buffer))) return false;

    // Prefer the 2D lock (gives the true pitch and total length); fall back to a plain lock.
    ComPtr<IMF2DBuffer2> buf2d;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    bool locked2d = false;
    if (SUCCEEDED(buffer.As(&buf2d)) && SUCCEEDED(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length))) {
        locked2d = true;
    } else if (SUCCEEDED(buffer->Lock(&start, nullptr, &length))) {
        scan0 = start;
        pitch = layout_.stride > 0 ? layout_.stride : static_cast<LONG>(layout_.width);
    } else {
        return false;
    }

    bool ok = false;
    if (pitch > 0 && static_cast<std::uint32_t>(pitch) >= layout_.width) {
        // The UV plane follows the (possibly height-padded) Y plane: rows = 2/3 of the buffer.
        const size_t used = length - static_cast<size_t>(scan0 - start);
        const size_t lumaRows = used / static_cast<size_t>(pitch) * 2 / 3;
        const size_t uvOffset = lumaRows * static_cast<size_t>(pitch);
        if (lumaRows >= layout_.height && uvOffset + static_cast<size_t>(pitch) * (layout_.height / 2) <= used) {
            processing::Nv12Planes p;
            p.y = scan0;
            p.uv = scan0 + uvOffset;
            p.yStride = pitch;
            p.uvStride = pitch;
            p.width = static_cast<int>(layout_.width & ~1u);
            p.height = static_cast<int>(layout_.height & ~1u);
            const size_t need = static_cast<size_t>(dstW) * static_cast<size_t>(dstH);
            if (bgra_.size() != need) bgra_.assign(need, 0);  // reallocates only when the preview is resized
            ok = scaler_.Convert(p, YuvFormatFor(layout_), bgra_.data(), dstW, dstH, dstW, mirror_);
            if (ok) { bgraW_ = dstW; bgraH_ = dstH; }
        }
    }
    if (locked2d) buf2d->Unlock2D();
    else buffer->Unlock();
    return ok;
}

void PreviewWindow::Paint(HDC dc, const RECT& rc) {
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;

    bool drawn = false;
    if (sample_ && layout_.width && layout_.height && cw > 0 && ch > 0) {
        // Letterbox: fit the frame inside the client area, preserving aspect ratio.
        const double scale = std::min(static_cast<double>(cw) / layout_.width, static_cast<double>(ch) / layout_.height);
        const int dw = std::max(1, static_cast<int>(layout_.width * scale));
        const int dh = std::max(1, static_cast<int>(layout_.height * scale));
        const int dx = (cw - dw) / 2, dy = (ch - dh) / 2;

        if (dirty_ || bgraW_ != dw || bgraH_ != dh) {
            if (ConvertHeldFrame(dw, dh)) dirty_ = false;
        }
        if (bgraW_ == dw && bgraH_ == dh && !bgra_.empty()) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = dw;
            bi.bmiHeader.biHeight = -dh;  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, dx, dy, static_cast<DWORD>(dw), static_cast<DWORD>(dh), 0, 0, 0, static_cast<UINT>(dh),
                              bgra_.data(), &bi, DIB_RGB_COLORS);
            RECT bars[4] = {{0, 0, cw, dy}, {0, dy + dh, cw, ch}, {0, dy, dx, dy + dh}, {dx + dw, dy, cw, dy + dh}};
            for (const auto& b : bars) {
                if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
            }
            drawn = true;
        }
    }

    if (!drawn) {
        FillRect(dc, &rc, black);
        if (placeholder_[0]) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(200, 200, 200));
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(GetParent(hwnd_), WM_GETFONT, 0, 0)));
            RECT t = rc;
            InflateRect(&t, -24, -24);
            DrawTextW(dc, placeholder_, -1, &t, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            SelectObject(dc, old);
        }
    }
}

LRESULT CALLBACK PreviewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (self) self->Paint(dc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace ixc::app
