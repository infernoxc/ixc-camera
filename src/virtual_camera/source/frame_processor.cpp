#include "virtual_camera/source/frame_processor.h"

#include "profiles/active_profile.h"
#include "virtual_camera/source/trace.h"

#include <mferror.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace ixc::vcam {

namespace {

// Locks a video buffer for reading/writing and reports the plane layout. Handles padded
// planes: the UV plane follows the (possibly height-aligned) Y plane.
class LockedNv12 {
public:
    LockedNv12(IMFMediaBuffer* buffer, UINT32 width, UINT32 height, bool write) {
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b2d_))) &&
            SUCCEEDED(b2d_->Lock2DSize(write ? MF2DBuffer_LockFlags_Write : MF2DBuffer_LockFlags_Read, &scan0_, &pitch_, &start_, &length_))) {
            locked2d_ = true;
        } else if (SUCCEEDED(buffer->Lock(&start_, nullptr, &length_))) {
            buffer_ = buffer;
            scan0_ = start_;
            pitch_ = static_cast<LONG>(width);
        } else {
            return;
        }
        if (pitch_ <= 0 || static_cast<UINT32>(pitch_) < width) return;
        const size_t used = length_ - static_cast<size_t>(scan0_ - start_);
        const size_t lumaRows = used / static_cast<size_t>(pitch_) * 2 / 3;
        const size_t uvOffset = lumaRows * static_cast<size_t>(pitch_);
        if (lumaRows < height || uvOffset + static_cast<size_t>(pitch_) * (height / 2) > used) return;
        uv_ = scan0_ + uvOffset;
        ok_ = true;
    }
    ~LockedNv12() {
        if (locked2d_) b2d_->Unlock2D();
        else if (buffer_) buffer_->Unlock();
    }
    LockedNv12(const LockedNv12&) = delete;
    LockedNv12& operator=(const LockedNv12&) = delete;

    bool ok() const { return ok_; }
    BYTE* y() const { return scan0_; }
    BYTE* uv() const { return uv_; }
    int pitch() const { return static_cast<int>(pitch_); }

private:
    ComPtr<IMF2DBuffer2> b2d_;
    IMFMediaBuffer* buffer_ = nullptr;
    BYTE* scan0_ = nullptr;
    BYTE* start_ = nullptr;
    BYTE* uv_ = nullptr;
    LONG pitch_ = 0;
    DWORD length_ = 0;
    bool locked2d_ = false;
    bool ok_ = false;
};

}  // namespace

FrameProcessor::~FrameProcessor() { EndSession(); }

void FrameProcessor::BeginSession(IMFMediaType* type) {
    EndSession();
    std::lock_guard session(sessionMu_);
    {
        std::lock_guard lock(mu_);
        type_ = type;
        GUID sub{};
        width_ = height_ = 0;
        nv12_ = type && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &sub)) && IsEqualGUID(sub, MFVideoFormat_NV12);
        if (type) MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width_, &height_);
        fullRange_ = type && MFGetAttributeUINT32(type, MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
        counters_ = {};
    }
    ReloadSettings();

    // Watch the settings folder while streaming only (thread-pool wait, no polling).
    const auto dir = ActiveProfileDirectory();
    if (!dir.empty()) {
        change_ = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (change_ != INVALID_HANDLE_VALUE &&
            !RegisterWaitForSingleObject(&wait_, change_, &FrameProcessor::OnSettingsChanged, this, INFINITE, WT_EXECUTEDEFAULT)) {
            wait_ = nullptr;
        }
    }
    IXC_TRACE("ProcessorSessionStart", TraceLoggingUInt32(width_, "width"), TraceLoggingUInt32(height_, "height"),
              TraceLoggingBoolean(nv12_, "nv12"), TraceLoggingBoolean(fullRange_, "fullRange"),
              TraceLoggingBoolean(wait_ != nullptr, "watching"));
}

void FrameProcessor::EndSession() {
    // Safe to call repeatedly and from different threads (stream events, SetStreamState,
    // shutdown); the session controls are only touched under sessionMu_.
    std::lock_guard session(sessionMu_);
    if (wait_) {
        UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);  // waits for a running reload to finish
        wait_ = nullptr;
    }
    if (change_ != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(change_);
        change_ = INVALID_HANDLE_VALUE;
    }
    Counters c;
    bool wasActive = false;
    {
        std::lock_guard lock(mu_);
        c = counters_;
        wasActive = type_ != nullptr;
        allocator_.Reset();  // an idle camera holds no frame buffers
        type_.Reset();
        ++generation_;       // the next session decides CPU/GPU afresh
    }
    const processing::AdaptiveNv12Processor::Stats ps = processor_.GetStats();
    processor_.ReleaseGpu();  // and no GPU device either
    if (wasActive) {
        IXC_TRACE("ProcessorBackend", TraceLoggingBoolean(ps.backend == processing::Backend::Gpu, "gpu"),
                  TraceLoggingInt32(static_cast<int>(ps.state), "state"), TraceLoggingUInt64(ps.cpuFrames, "cpuFrames"),
                  TraceLoggingUInt64(ps.gpuFrames, "gpuFrames"), TraceLoggingFloat64(ps.cpuAvgMs, "cpuAvgMs"),
                  TraceLoggingFloat64(ps.gpuAvgMs, "gpuAvgMs"), TraceLoggingBoolean(ps.gpuFailed, "gpuFailed"),
                  TraceLoggingWideString(ps.adapter.c_str(), "adapter"));
    }
    if (wasActive) {
        IXC_TRACE("ProcessorSessionEnd", TraceLoggingUInt64(c.processed, "processed"), TraceLoggingUInt64(c.passedThrough, "passedThrough"),
                  TraceLoggingUInt64(c.poolExhausted, "poolExhausted"), TraceLoggingUInt64(c.errors, "errors"),
                  TraceLoggingUInt64(c.settingsReloads, "reloads"), TraceLoggingBoolean(c.inputIsGpuSurface, "gpuInput"),
                  TraceLoggingUInt64(c.processed ? c.processUsTotal / c.processed : 0, "avgUs"), TraceLoggingUInt64(c.processUsMax, "maxUs"));
    }
}

void CALLBACK FrameProcessor::OnSettingsChanged(void* ctx, BOOLEAN) {
    auto* self = static_cast<FrameProcessor*>(ctx);
    FindNextChangeNotification(self->change_);  // re-arm before reading, so no change is missed
    self->ReloadSettings();
}

void FrameProcessor::ReloadSettings() {
    // Parsing (a few KB of strictly validated JSON) happens here, never on the frame path.
    bool missing = false;
    ProfileLoadResult r = LoadActiveProfile(&missing);
    std::lock_guard lock(mu_);
    // Unchanged profile (the folder also changes for temp files): keep the compiled params.
    if (r.ok && profileValid_ && r.profile == profile_) return;
    if (!r.ok && !missing && params_) {
        // Present but unreadable/invalid (e.g. caught mid-write, or a bad hand edit): keep the
        // last good settings instead of flickering to neutral.
        IXC_TRACE("SettingsRejectedKeptPrevious");
        return;
    }
    profileValid_ = r.ok;
    profile_ = r.ok ? r.profile : Profile{};
    if (!r.ok) {
        // No usable settings at all: pass the camera through unchanged rather than guess.
        profile_.image.sharpness = 0;
    }
    ++counters_.settingsReloads;
    Recompile();
    IXC_TRACE("SettingsLoaded", TraceLoggingBoolean(r.ok, "valid"), TraceLoggingBoolean(missing, "missing"),
              TraceLoggingBoolean(params_ && params_->identity, "identity"),
              TraceLoggingUInt32(static_cast<UINT32>(r.warnings.size()), "warnings"));
}

void FrameProcessor::Recompile() {
    ++generation_;  // new settings: the CPU/GPU decision is made again
    if (!nv12_ || width_ == 0 || height_ == 0) {
        auto identity = std::make_shared<processing::PipelineParams>();
        params_ = std::move(identity);
        return;
    }
    params_ = std::make_shared<const processing::PipelineParams>(processing::CompileParams(profile_, width_, height_, fullRange_));
}

// Returns this session's allocator, creating it on first use. Requires mu_ (EndSession releases
// the pool concurrently from another thread).
HRESULT FrameProcessor::EnsureAllocator(IMFMediaType* type, ComPtr<IMFVideoSampleAllocatorEx>& out) {
    if (!allocator_) {
        ComPtr<IMFVideoSampleAllocatorEx> a;
        HRESULT hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&a));
        // A small bounded pool: 2 samples up front, never more than 6 in flight.
        if (SUCCEEDED(hr)) hr = a->InitializeSampleAllocatorEx(2, 6, nullptr, type);
        if (FAILED(hr)) return hr;
        allocator_ = a;
    }
    out = allocator_;
    return S_OK;
}

ComPtr<IMFSample> FrameProcessor::Process(IMFSample* input) {
    std::shared_ptr<const processing::PipelineParams> params;
    ComPtr<IMFMediaType> type;
    UINT32 width = 0, height = 0;
    bool nv12 = false;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mu_);
        params = params_;
        generation = generation_;
        type = type_;
        width = width_;
        height = height_;
        nv12 = nv12_;
    }
    // Record once per session whether frames live in GPU memory (decides the GPU strategy).
    bool knowInput;
    {
        std::lock_guard lock(mu_);
        knowInput = counters_.inputKnown;
    }
    if (!knowInput && input) {
        ComPtr<IMFMediaBuffer> b;
        ComPtr<IMFDXGIBuffer> dxgi;
        const bool gpu = SUCCEEDED(input->GetBufferByIndex(0, &b)) && SUCCEEDED(b.As(&dxgi));
        std::lock_guard lock(mu_);
        counters_.inputKnown = true;
        counters_.inputIsGpuSurface = gpu;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    auto passThrough = [&] {
        std::lock_guard lock(mu_);
        ++counters_.passedThrough;
        return ComPtr<IMFSample>(input);
    };
    if (!input || !params || params->identity || !nv12 || !type) return passThrough();

    ComPtr<IMFVideoSampleAllocatorEx> allocator;
    HRESULT hr;
    {
        std::lock_guard lock(mu_);
        hr = type_ ? EnsureAllocator(type.Get(), allocator) : MF_E_SHUTDOWN;  // session may have just ended
    }
    ComPtr<IMFSample> out;
    if (SUCCEEDED(hr)) hr = allocator->AllocateSample(&out);
    if (FAILED(hr)) {
        std::lock_guard lock(mu_);
        if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) ++counters_.poolExhausted;
        else ++counters_.errors;
        ++counters_.passedThrough;
        return ComPtr<IMFSample>(input);  // never stall the app: deliver the original frame
    }

    ComPtr<IMFMediaBuffer> inBuf, outBuf;
    bool ok = SUCCEEDED(input->GetBufferByIndex(0, &inBuf)) && SUCCEEDED(out->GetBufferByIndex(0, &outBuf));
    if (ok) {
        LockedNv12 src(inBuf.Get(), width, height, false);
        LockedNv12 dst(outBuf.Get(), width, height, true);
        ok = src.ok() && dst.ok();
        if (ok) {
            const processing::Nv12Planes in{src.y(), src.uv(), src.pitch(), src.pitch(), static_cast<int>(width), static_cast<int>(height)};
            const processing::Nv12Frame o{dst.y(), dst.uv(), dst.pitch(), dst.pitch(), static_cast<int>(width), static_cast<int>(height)};
            ok = processor_.Process(in, o, *params, generation);
        }
    }
    if (!ok) {
        std::lock_guard lock(mu_);
        ++counters_.errors;
        ++counters_.passedThrough;
        return ComPtr<IMFSample>(input);
    }

    // Carry timing and all sample attributes (device timestamp, flags) over to the output.
    input->CopyAllItems(out.Get());
    LONGLONG t = 0;
    if (SUCCEEDED(input->GetSampleTime(&t))) out->SetSampleTime(t);
    if (SUCCEEDED(input->GetSampleDuration(&t))) out->SetSampleDuration(t);
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const auto us = static_cast<unsigned long long>((t1.QuadPart - t0.QuadPart) * 1'000'000 / f.QuadPart);
    {
        std::lock_guard lock(mu_);
        ++counters_.processed;
        counters_.processUsTotal += us;
        counters_.processUsMax = std::max(counters_.processUsMax, us);
    }
    return out;
}

bool FrameProcessor::Active() const {
    std::lock_guard lock(mu_);
    return type_ != nullptr;
}

FrameProcessor::Counters FrameProcessor::Stats() const {
    std::lock_guard lock(mu_);
    return counters_;
}

}  // namespace ixc::vcam
