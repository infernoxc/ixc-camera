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
#include "processing/color.h"

#include <windows.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace ixc::app {

class PreviewWindow {
public:
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
    void SetMirror(bool mirror);

    static constexpr UINT kFrameMessage = WM_APP + 10;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void Paint(HDC dc, const RECT& client);
    bool ConvertHeldFrame(int dstW, int dstH);

    HWND hwnd_ = nullptr;
    std::atomic<bool> framePending_{false};
    Microsoft::WRL::ComPtr<IMFSample> sample_;
    camera::FrameLayout layout_;
    bool mirror_ = false;
    bool dirty_ = false;  // held frame not yet converted at the current size

    processing::Nv12ToBgraScaler scaler_;
    std::vector<std::uint32_t> bgra_;  // display-size conversion buffer, reused
    int bgraW_ = 0, bgraH_ = 0;
    wchar_t placeholder_[160] = L"";
};

}  // namespace ixc::app
