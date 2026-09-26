#pragma once

// Child window that shows the newest NV12 camera frame, letterboxed.
//
// The frame is converted to BGRA only at the displayed size (processing::Nv12ToBgraScaler)
// into one reused buffer, then blitted 1:1 with SetDIBitsToDevice. This avoids a
// full-resolution RGB32 conversion (measured at ~26 MB extra private memory at 1080p).
// Frames are painted only on arrival (no render loop), and a new frame message is posted only
// once the previous one has been handled, so a slow UI thread never queues work.
// Phase 6 replaces this with a Direct3D 11 swap chain fed by the GPU pipeline.

#include "camera/capture_session.h"
#include "effects/effects.h"
#include "face/face_engine.h"
#include "processing/color.h"
#include "processing/gpu/adaptive_processor.h"
#include "processing/image_pipeline.h"

#include <memory>

#include <windows.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace ixc::app {

class PreviewWindow {
public:
    ~PreviewWindow();
    static bool RegisterClass(HINSTANCE instance);
    HWND Create(HWND parent, HINSTANCE instance, int id);
    HWND hwnd() const { return hwnd_; }

    // Called from the capture thread. Posts at most one pending repaint request.
    void NotifyFrameAvailable();
    // The UI calls this for a frame message: keeps the frame and repaints.
    void ShowFrame(Microsoft::WRL::ComPtr<IMFSample> sample, const camera::FrameLayout& layout);
    // Drops the held frame and conversion buffer, and shows the placeholder text.
    void Clear(const wchar_t* placeholder);
    void FrameMessageHandled() { framePending_.store(false); }
    // The same image pipeline IXC Camera applies, so the preview shows what apps receive.
    // nullptr or identity params = show the camera frame unchanged.
    void SetPipeline(std::shared_ptr<const processing::PipelineParams> params);

    // Face tracking on the previewed frames: nullptr = off (no thread, no memory). Restarts only
    // when the settings actually change.
    void SetFaceTracking(const face::EngineConfig* config);
    // Draw the tracked faces over the preview (preview only; never sent to apps).
    void SetFaceOverlay(bool on);
    face::EngineStatus FaceStatus() const { return face_.Status(); }
    // "GPU" while the picture pipeline runs on the graphics card, otherwise "CPU".
    std::wstring ProcessingBackend() const { return processor_.GetStats().backend == processing::Backend::Gpu ? L"GPU" : L"CPU"; }
    // IXC effects applied after the pipeline (nullptr = none).
    void SetEffects(std::shared_ptr<const effects::EffectConfig> cfg) { effects_ = std::move(cfg); dirty_ = sample_ != nullptr; }

    static constexpr UINT kFrameMessage = WM_APP + 10;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC dc, const RECT& client);
    void PaintFaces(HDC dc, int dx, int dy, int dw, int dh);
    bool ConvertHeldFrame(int dstW, int dstH);

    HWND hwnd_ = nullptr;
    std::atomic<bool> framePending_{false};
    Microsoft::WRL::ComPtr<IMFSample> sample_;
    camera::FrameLayout layout_;
    bool dirty_ = false;  // held frame not yet converted at the current size

    std::shared_ptr<const processing::PipelineParams> pipeline_;
    processing::AdaptiveNv12Processor processor_;  // CPU-first; GPU only where measured to help
    std::uint64_t pipelineGeneration_ = 0;
    std::vector<std::uint8_t> processed_;  // full-size NV12 output of the pipeline, reused
    processing::Nv12ToBgraScaler scaler_;
    std::vector<std::uint32_t> bgra_;  // display-size conversion buffer, reused
    int bgraW_ = 0, bgraH_ = 0;
    wchar_t placeholder_[160] = L"";

    face::FaceEngine face_;
    face::EngineConfig faceConfig_;
    bool faceOverlay_ = true;
    std::shared_ptr<const effects::EffectConfig> effects_;
    effects::EffectRenderer renderer_;
    HPEN facePen_ = nullptr, landmarkPen_ = nullptr;
};

}  // namespace ixc::app
