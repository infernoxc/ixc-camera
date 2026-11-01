#include "app/preview_window.h"

#include "face/face_settings.h"

#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <windowsx.h>

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

PreviewWindow::~PreviewWindow() {
    face_.Stop();
    seg_.Stop();
    if (facePen_) DeleteObject(facePen_);
    if (landmarkPen_) DeleteObject(landmarkPen_);
}

void PreviewWindow::SetFaceTracking(const face::EngineConfig* config) {
    if (!config) {
        if (face_.Running()) face_.Stop();
        return;
    }
    if (face_.Running() && face::SameEngineConfig(*config, faceConfig_)) return;
    faceConfig_ = *config;
    face_.Start(faceConfig_);  // restarts if running
}

void PreviewWindow::SetFaceOverlay(bool on) {
    faceOverlay_ = on;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::PaintFaces(HDC dc, int dx, int dy, int dw, int dh) {
    face::FaceSnapshot snap;
    face_.Snapshot(face::FaceEngine::NowMs(), snap);
    if (snap.count == 0) return;
    // Faces are tracked in source coordinates; the preview shows the processed output.
    const face::OutputMapping m = shownMap_;  // includes auto-framing
    if (!facePen_) facePen_ = CreatePen(PS_SOLID, 2, RGB(80, 220, 120));
    if (!landmarkPen_) landmarkPen_ = CreatePen(PS_SOLID, 1, RGB(255, 210, 60));
    auto px = [&](face::PointF p) { return POINT{dx + static_cast<int>(p.x * dw), dy + static_cast<int>(p.y * dh)}; };

    const HGDIOBJ oldPen = SelectObject(dc, facePen_);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(80, 220, 120));
    const HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(GetParent(hwnd_), WM_GETFONT, 0, 0)));
    for (int i = 0; i < snap.count; ++i) {
        const face::TrackedFace& f = snap.faces[static_cast<size_t>(i)];
        const face::RectF b = face::MapToOutput(f.box, m);
        const POINT a = px({b.x, b.y}), c = px({b.x + b.w, b.y + b.h});
        SelectObject(dc, facePen_);
        Rectangle(dc, a.x, a.y, c.x, c.y);
        wchar_t label[64];
        swprintf_s(label, L"#%d  %.0f%%  roll %.0f°  yaw %.0f°", f.id, f.confidence * 100, f.rollDeg * (m.mirror ? -1 : 1),
                   f.yawDeg * (m.mirror ? -1 : 1));
        TextOutW(dc, a.x, std::max<int>(dy, a.y - 18), label, static_cast<int>(wcslen(label)));
        if (!f.landmarksValid) continue;
        SelectObject(dc, landmarkPen_);
        for (const face::PointF& p : {f.lm.leftEye, f.lm.rightEye, f.lm.nose, f.lm.mouthLeft, f.lm.mouthRight}) {
            const POINT q = px(face::MapToOutput(p, m));
            Ellipse(dc, q.x - 3, q.y - 3, q.x + 4, q.y + 4);
        }
        for (const face::RectF& br : {f.leftBrow, f.rightBrow}) {
            const face::RectF o = face::MapToOutput(br, m);
            const POINT l = px({o.x, o.y + o.h}), r = px({o.x + o.w, o.y + o.h});
            MoveToEx(dc, l.x, l.y, nullptr);
            LineTo(dc, r.x, r.y);
        }
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

bool PreviewWindow::RegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &PreviewWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // painted entirely in WM_PAINT (avoids flicker)
    wc.style = CS_DBLCLKS;       // double-click in the frame editor: whole picture
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
    std::vector<std::uint8_t>().swap(processed_);
    processor_.ReleaseGpu();
    face_.Stop();  // no preview: no tracking
    seg_.Stop();   // nor segmentation
    ++pipelineGeneration_;
    bgraW_ = bgraH_ = 0;
    wcsncpy_s(placeholder_, placeholder ? placeholder : L"", _TRUNCATE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::SetPipeline(std::shared_ptr<const processing::PipelineParams> params) {
    pipeline_ = std::move(params);
    ++pipelineGeneration_;  // new settings: the CPU/GPU decision is made again
    if (!pipeline_ || pipeline_->identity) std::vector<std::uint8_t>().swap(processed_);  // free when unused
    dirty_ = sample_ != nullptr;
    InvalidateRect(hwnd_, nullptr, FALSE);
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

            // Face tracking samples the camera frame (source coordinates), before processing.
            const double now = face::FaceEngine::NowMs();
            if (face_.WantsFrame(now)) face_.OnFrame(p, YuvFormatFor(layout_), now, 33.3);
            // Segmentation runs only while a background effect is on (started here, on demand).
            const bool wantSeg = effects_ && effects_->Active() && effects_->needsSegmentation;
            if (wantSeg && !seg_.Running()) seg_.Start();
            else if (!wantSeg && seg_.Running()) seg_.Stop();
            const double segNow = seg::SegmentationEngine::NowMs();
            if (seg_.WantsFrame(segNow)) seg_.OnFrame(p, YuvFormatFor(layout_), segNow);

            // Auto-framing: follow the face by moving the source rectangle (as IXC Camera does).
            const processing::PipelineParams* params = pipeline_.get();
            std::uint64_t generation = pipelineGeneration_;
            face::FaceSnapshot snap;
            if (autoFraming_ && params) {
                face_.Snapshot(now, snap);
                const face::ViewRect base{params->srcX, params->srcY, params->srcW, params->srcH};
                const face::ViewRect v = framer_.Update(base, face::LargestFace(snap), now);
                if (framer_.Framing()) {
                    framed_ = *params;
                    framed_.srcX = v.x;
                    framed_.srcY = v.y;
                    framed_.srcW = v.w;
                    framed_.srcH = v.h;
                    framed_.geometryIdentity = false;
                    framed_.identity = false;
                    params = &framed_;
                    generation |= 1ull << 63;
                }
            } else {
                framer_.Reset();
            }
            shownMap_ = params ? face::OutputMapping{static_cast<float>(params->srcX), static_cast<float>(params->srcY),
                                                     static_cast<float>(params->srcW), static_cast<float>(params->srcH), params->mirror}
                               : face::OutputMapping{};

            // Apply IXC's pipeline at full resolution first (exactly what apps receive).
            if (params && !params->identity) {
                const size_t frameBytes = static_cast<size_t>(p.width) * static_cast<size_t>(p.height) * 3 / 2;
                if (processed_.size() != frameBytes) processed_.assign(frameBytes, 0);
                const processing::Nv12Frame out{processed_.data(), processed_.data() + static_cast<size_t>(p.width) * p.height,
                                                p.width, p.width, p.width, p.height};
                if (processor_.Process(p, out, *params, generation)) p = {out.y, out.uv, out.yStride, out.uvStride, p.width, p.height};
            }
            // Effects, exactly as IXC Camera applies them (in place on the processed frame).
            if (effects_ && effects_->Active()) {
                const size_t frameBytes = static_cast<size_t>(p.width) * static_cast<size_t>(p.height) * 3 / 2;
                if (p.y != processed_.data()) {  // pipeline was neutral: work on a copy
                    if (processed_.size() != frameBytes) processed_.assign(frameBytes, 0);
                    for (int y = 0; y < p.height; ++y)
                        memcpy(processed_.data() + static_cast<size_t>(y) * p.width, p.y + static_cast<size_t>(y) * p.yStride, static_cast<size_t>(p.width));
                    std::uint8_t* uvDst = processed_.data() + static_cast<size_t>(p.width) * p.height;
                    for (int y = 0; y < p.height / 2; ++y)
                        memcpy(uvDst + static_cast<size_t>(y) * p.width, p.uv + static_cast<size_t>(y) * p.uvStride, static_cast<size_t>(p.width));
                    p = {processed_.data(), uvDst, p.width, p.width, p.width, p.height};
                }
                if (effects_->needsFaces && !(autoFraming_ && params)) face_.Snapshot(now, snap);
                effects::FrameContext ctx;
                ctx.faces = &snap;
                ctx.fullRange = YuvFormatFor(layout_).fullRange;
                if (effects_->needsSegmentation) {
                    seg_.Snapshot(segMask_);
                    ctx.mask = &segMask_;
                }
                ctx.map = shownMap_;
                const processing::Nv12Frame fx{processed_.data(), processed_.data() + static_cast<size_t>(p.width) * p.height, p.width, p.width,
                                               p.width, p.height};
                renderer_.Apply(fx, *effects_, ctx);
            }

            const size_t need = static_cast<size_t>(dstW) * static_cast<size_t>(dstH);
            if (bgra_.size() != need) bgra_.assign(need, 0);  // reallocates only when the preview is resized
            ok = scaler_.Convert(p, YuvFormatFor(layout_), bgra_.data(), dstW, dstH, dstW, false);
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
            shown_ = {dx, dy, dx + dw, dy + dh};
            const std::uint32_t* pixels = bgra_.data();
            if (editFrame_) {
                // Outside the frame: dimmed to ~40% (a reused copy; the converted frame stays clean).
                if (overlay_.size() != bgra_.size()) overlay_.assign(bgra_.size(), 0);
                const face::ViewRect f = ShownFrame();
                const int fx0 = static_cast<int>(std::lround(f.x * dw)), fx1 = static_cast<int>(std::lround((f.x + f.w) * dw));
                const int fy0 = static_cast<int>(std::lround(f.y * dh)), fy1 = static_cast<int>(std::lround((f.y + f.h) * dh));
                for (int yy = 0; yy < dh; ++yy) {
                    const std::uint32_t* src = bgra_.data() + static_cast<size_t>(yy) * dw;
                    std::uint32_t* dst = overlay_.data() + static_cast<size_t>(yy) * dw;
                    const bool rowIn = yy >= fy0 && yy < fy1;
                    for (int xx = 0; xx < dw; ++xx) {
                        const std::uint32_t px = src[xx];
                        dst[xx] = rowIn && xx >= fx0 && xx < fx1 ? px : ((px >> 2) & 0x3F3F3Fu) + ((px >> 3) & 0x1F1F1Fu);
                    }
                }
                pixels = overlay_.data();
            }
            SetDIBitsToDevice(dc, dx, dy, static_cast<DWORD>(dw), static_cast<DWORD>(dh), 0, 0, 0, static_cast<UINT>(dh),
                              pixels, &bi, DIB_RGB_COLORS);
            RECT bars[4] = {{0, 0, cw, dy}, {0, dy + dh, cw, ch}, {0, dy, dx, dy + dh}, {dx + dw, dy, cw, dy + dh}};
            for (const auto& b : bars) {
                if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
            }
            if (faceOverlay_ && face_.Running()) PaintFaces(dc, dx, dy, dw, dh);
            if (editFrame_) PaintFrameEditor(dc, dx, dy, dw, dh);
            else if (grid_) PaintGrid(dc, dx, dy, dw, dh);
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

// ---- Frame editor ----------------------------------------------------------------------------------

void PreviewWindow::SetFrameEditing(bool on, std::function<void(const face::ViewRect&)> onChange) {
    editFrame_ = on;
    onFrame_ = on ? std::move(onChange) : nullptr;
    if (!on) {
        drag_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        std::vector<std::uint32_t>().swap(overlay_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PreviewWindow::SetFrameRect(const face::ViewRect& r) {
    if (drag_ >= 0 || r.w <= 0 || r.h <= 0) return;  // the user's drag wins over echoes of it
    frame_ = r;
    frameAspect_ = r.w / r.h;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

face::ViewRect PreviewWindow::ShownFrame() const {
    face::ViewRect f = frame_;
    if (pipeline_ && pipeline_->mirror) f.x = 1.0 - f.x - f.w;
    return f;
}

void PreviewWindow::SetShownFrame(face::ViewRect r) {
    // Shape: the output's; size: between the whole picture and a 4x zoom; position: inside.
    const double hMax = std::min(1.0, 1.0 / frameAspect_);
    const double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    r.h = std::clamp(r.h, hMax / 4, hMax);
    r.w = r.h * frameAspect_;
    r.x = std::clamp(cx - r.w / 2, 0.0, std::max(0.0, 1.0 - r.w));
    r.y = std::clamp(cy - r.h / 2, 0.0, std::max(0.0, 1.0 - r.h));
    if (r.w >= 1.0 - 1e-9) r.x = 0;
    if (r.h >= 1.0 - 1e-9) r.y = 0;
    face::ViewRect src = r;
    if (pipeline_ && pipeline_->mirror) src.x = 1.0 - r.x - r.w;
    frame_ = src;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (onFrame_) onFrame_(frame_);
}

int PreviewWindow::HitTest(int x, int y) const {
    const int dw = shown_.right - shown_.left, dh = shown_.bottom - shown_.top;
    if (dw <= 0 || dh <= 0) return -1;
    const face::ViewRect f = ShownFrame();
    const int l = shown_.left + static_cast<int>(f.x * dw), r = shown_.left + static_cast<int>((f.x + f.w) * dw);
    const int t = shown_.top + static_cast<int>(f.y * dh), b = shown_.top + static_cast<int>((f.y + f.h) * dh);
    const int grab = MulDiv(14, static_cast<int>(GetDpiForWindow(hwnd_)), 96);
    const auto within = [grab](int a, int c) { return std::abs(a - c) <= grab; };
    if (within(x, l) && within(y, t)) return 1;
    if (within(x, r) && within(y, t)) return 2;
    if (within(x, l) && within(y, b)) return 3;
    if (within(x, r) && within(y, b)) return 4;
    return x >= l && x < r && y >= t && y < b ? 0 : -1;
}

void PreviewWindow::OnEditorDown(int x, int y) {
    const int dw = shown_.right - shown_.left, dh = shown_.bottom - shown_.top;
    if (dw <= 0 || dh <= 0) return;
    int hit = HitTest(x, y);
    if (hit < 0) {
        // Outside the frame: centre it on the click, then keep dragging it from there.
        face::ViewRect f = ShownFrame();
        f.x = (x - shown_.left) / static_cast<double>(dw) - f.w / 2;
        f.y = (y - shown_.top) / static_cast<double>(dh) - f.h / 2;
        SetShownFrame(f);
        hit = 0;
    }
    drag_ = hit;
    dragFrom_ = {x, y};
    dragStart_ = ShownFrame();
    SetCapture(hwnd_);
}

void PreviewWindow::OnEditorMove(int x, int y) {
    if (drag_ < 0) return;
    const int dw = shown_.right - shown_.left, dh = shown_.bottom - shown_.top;
    if (dw <= 0 || dh <= 0) return;
    face::ViewRect f = dragStart_;
    if (drag_ == 0) {
        f.x += (x - dragFrom_.x) / static_cast<double>(dw);
        f.y += (y - dragFrom_.y) / static_cast<double>(dh);
        // Moving never resizes: clamp the position here (SetShownFrame keeps the centre).
        f.x = std::clamp(f.x, 0.0, std::max(0.0, 1.0 - f.w));
        f.y = std::clamp(f.y, 0.0, std::max(0.0, 1.0 - f.h));
        SetShownFrame(f);
        return;
    }
    // Corner: the opposite corner stays put; the size follows the pointer, keeping the shape.
    const bool left = drag_ == 1 || drag_ == 3, top = drag_ == 1 || drag_ == 2;
    const double ax = left ? dragStart_.x + dragStart_.w : dragStart_.x;
    const double ay = top ? dragStart_.y + dragStart_.h : dragStart_.y;
    const double mx = (x - shown_.left) / static_cast<double>(dw), my = (y - shown_.top) / static_cast<double>(dh);
    const double hMax = std::min(1.0, 1.0 / frameAspect_);
    double h = std::max(std::abs(my - ay), std::abs(mx - ax) / frameAspect_);
    const double roomY = top ? ay : 1.0 - ay, roomX = left ? ax : 1.0 - ax;
    h = std::clamp(h, hMax / 4, std::max(hMax / 4, std::min({hMax, roomY, roomX / frameAspect_})));
    f.h = h;
    f.w = h * frameAspect_;
    f.x = left ? ax - f.w : ax;
    f.y = top ? ay - f.h : ay;
    SetShownFrame(f);
}

void PreviewWindow::OnEditorUp() {
    if (drag_ < 0) return;
    drag_ = -1;
    if (GetCapture() == hwnd_) ReleaseCapture();
}

void PreviewWindow::OnEditorWheel(int delta) {
    // Each notch: 10% tighter (wheel up) or wider, around the frame's centre.
    face::ViewRect f = ShownFrame();
    const double k = std::pow(0.9, delta / static_cast<double>(WHEEL_DELTA));
    const double cx = f.x + f.w / 2, cy = f.y + f.h / 2;
    f.h *= k;
    f.w *= k;
    f.x = cx - f.w / 2;
    f.y = cy - f.h / 2;
    SetShownFrame(f);
}

void PreviewWindow::PaintGrid(HDC dc, int x, int y, int w, int h) {
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    const HGDIOBJ old = SelectObject(dc, pen);
    for (int i = 1; i <= 2; ++i) {
        MoveToEx(dc, x + w * i / 3, y, nullptr);
        LineTo(dc, x + w * i / 3, y + h);
        MoveToEx(dc, x, y + h * i / 3, nullptr);
        LineTo(dc, x + w, y + h * i / 3);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

void PreviewWindow::PaintFrameEditor(HDC dc, int dx, int dy, int dw, int dh) {
    const face::ViewRect f = ShownFrame();
    const int l = dx + static_cast<int>(std::lround(f.x * dw)), r = dx + static_cast<int>(std::lround((f.x + f.w) * dw));
    const int t = dy + static_cast<int>(std::lround(f.y * dh)), b = dy + static_cast<int>(std::lround((f.y + f.h) * dh));
    PaintGrid(dc, l, t, r - l, b - t);
    const int dpi = static_cast<int>(GetDpiForWindow(hwnd_));
    const int px2 = std::max(2, MulDiv(2, dpi, 96)), handle = MulDiv(5, dpi, 96);
    HPEN border = CreatePen(PS_SOLID, px2, RGB(255, 255, 255));
    const HGDIOBJ oldPen = SelectObject(dc, border);
    const HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, l, t, r, b);
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    for (const POINT c : {POINT{l, t}, POINT{r, t}, POINT{l, b}, POINT{r, b}}) Rectangle(dc, c.x - handle, c.y - handle, c.x + handle, c.y + handle);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    // Labels: the frame's size and zoom; how to use the editor.
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(GetParent(hwnd_), WM_GETFONT, 0, 0)));
    wchar_t label[96];
    swprintf_s(label, L"%u × %u  ·  %.2f× zoom", static_cast<unsigned>(std::lround(f.w * layout_.width)),
               static_cast<unsigned>(std::lround(f.h * layout_.height)), f.h > 0 ? std::min(1.0, 1.0 / frameAspect_) / f.h : 1.0);
    RECT lr{l + MulDiv(8, dpi, 96), t + MulDiv(6, dpi, 96), r - 4, t + MulDiv(30, dpi, 96)};
    SetTextColor(dc, RGB(0, 0, 0));
    OffsetRect(&lr, 1, 1);
    DrawTextW(dc, label, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    OffsetRect(&lr, -1, -1);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, label, -1, &lr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    const wchar_t* hint = L"Drag to move  ·  drag a corner or scroll to resize  ·  double-click: whole picture";
    RECT hr{dx + 8, dy + dh - MulDiv(28, dpi, 96), dx + dw - 8, dy + dh - 6};
    SetTextColor(dc, RGB(230, 230, 230));
    DrawTextW(dc, hint, -1, &hr, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
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
        case WM_LBUTTONDOWN:
            if (self && self->editFrame_) self->OnEditorDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONDBLCLK:
            if (self && self->editFrame_) {
                self->SetShownFrame({0.5 - 5e8, 0.5 - 5e8, 1e9, 1e9});  // centred, clamped to the largest frame
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (self && self->editFrame_) self->OnEditorMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (self) self->OnEditorUp();
            return 0;
        case WM_MOUSEWHEEL:
            if (self && self->editFrame_) {
                self->OnEditorWheel(GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (self && self->editFrame_ && LOWORD(lp) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                const int hit = self->drag_ >= 0 ? self->drag_ : self->HitTest(pt.x, pt.y);
                const wchar_t* c = hit == 0 ? IDC_SIZEALL : hit == 1 || hit == 4 ? IDC_SIZENWSE : hit == 2 || hit == 3 ? IDC_SIZENESW : IDC_CROSS;
                SetCursor(LoadCursorW(nullptr, c));
                return TRUE;
            }
            break;
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
