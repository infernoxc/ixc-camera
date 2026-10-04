#include "app/preview_window.h"

#include "face/face_settings.h"

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
            SetDIBitsToDevice(dc, dx, dy, static_cast<DWORD>(dw), static_cast<DWORD>(dh), 0, 0, 0, static_cast<UINT>(dh),
                              bgra_.data(), &bi, DIB_RGB_COLORS);
            RECT bars[4] = {{0, 0, cw, dy}, {0, dy + dh, cw, ch}, {0, dy, dx, dy + dh}, {dx + dw, dy, cw, dy + dh}};
            for (const auto& b : bars) {
                if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
            }
            if (faceOverlay_ && face_.Running()) PaintFaces(dc, dx, dy, dw, dh);
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
