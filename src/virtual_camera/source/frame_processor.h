#pragma once

// Applies the user's active profile to frames inside the IXC Camera source.
//
// Lifecycle (driven by the stream):
//   BeginSession()  loads the active profile and starts watching it for changes. The watch is
//                   a thread-pool wait on a directory change notification: no polling thread.
//   Process()       per frame: pass-through when settings are neutral, otherwise processes into
//                   a sample from a small bounded pool (no per-frame allocation).
//   EndSession()    stops watching and releases the pool, so an idle camera holds nothing.
//
// Settings changes apply to the next frame. Parsing happens on the watcher thread, and the
// frame path only swaps in the compiled result.

#include "processing/gpu/adaptive_processor.h"
#include "processing/image_pipeline.h"
#include "profiles/profile.h"

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>

namespace ixc::vcam {

class FrameProcessor {
public:
    FrameProcessor() = default;
    ~FrameProcessor();
    FrameProcessor(const FrameProcessor&) = delete;
    FrameProcessor& operator=(const FrameProcessor&) = delete;

    void BeginSession(IMFMediaType* type);
    void EndSession();
    bool Active() const;  // a session is running (settings watched, pool available)

    // Returns the sample to deliver: the input itself (pass-through) or a processed copy.
    Microsoft::WRL::ComPtr<IMFSample> Process(IMFSample* input);

    struct Counters {
        unsigned long long processed = 0;
        unsigned long long passedThrough = 0;
        unsigned long long poolExhausted = 0;
        unsigned long long errors = 0;
        unsigned long long settingsReloads = 0;
        unsigned long long processUsTotal = 0;  // time spent processing (µs), for real-pipeline cost
        unsigned long long processUsMax = 0;
        bool inputIsGpuSurface = false;         // frames arrive as DXGI (GPU) buffers
        bool inputKnown = false;
    };
    Counters Stats() const;

private:
    static void CALLBACK OnSettingsChanged(void* ctx, BOOLEAN);
    void ReloadSettings();
    void Recompile();  // requires mu_
    HRESULT EnsureAllocator(IMFMediaType* type, Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx>& out);

    std::mutex sessionMu_;  // serializes Begin/EndSession (never held by the frame path)
    mutable std::mutex mu_;
    Profile profile_;
    bool profileValid_ = false;
    std::shared_ptr<const processing::PipelineParams> params_;  // swapped atomically under mu_
    UINT32 width_ = 0, height_ = 0;
    bool fullRange_ = false;
    bool nv12_ = false;
    Counters counters_;

    processing::AdaptiveNv12Processor processor_;  // CPU-first; GPU only where measured to help
    std::uint64_t generation_ = 0;                 // bumped on every settings/session change (under mu_)
    Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    Microsoft::WRL::ComPtr<IMFMediaType> type_;

    HANDLE change_ = INVALID_HANDLE_VALUE;
    HANDLE wait_ = nullptr;
};

}  // namespace ixc::vcam
