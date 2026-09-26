#include "virtual_camera/source/frame_processor.h"

#include "face/face_settings.h"
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

void FrameProcessor::BeginSession(IMFMediaType* type, IKsControl* ks) {
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
        UINT32 n = 0, d = 1;
        nominalFps_ = type && SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &n, &d)) && d ? static_cast<double>(n) / d : 0;
        frameIntervalMs_ = nominalFps_ > 0 ? 1000.0 / nominalFps_ : 33.3;
        compensationEv_ = 0;
        counters_ = {};
    }
    ks_ = ks;
    ReloadSettings();
    {
        std::lock_guard lock(smoothMu_);
        std::lock_guard l2(mu_);
        smoothEnabled_ = profile_.smoothMotion;
        smooth_.Begin(ks_.Get(), smoothEnabled_ && nv12_, nominalFps_);
    }
    UpdateFaceTracking();

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
    StopFaceTracking();
    int smoothExposure = 0;
    double smoothEv = 0;
    int smoothReasserts = 0;
    HRESULT smoothRestore = S_FALSE;
    {
        std::lock_guard lock(smoothMu_);
        smoothExposure = smooth_.AppliedExposure();
        smoothEv = smooth_.CompensationEv();
        smoothReasserts = smooth_.Reasserts();
        smoothRestore = smooth_.End();  // gives the camera its automatic exposure back
    }
    ks_.Reset();
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
        IXC_TRACE("SmoothMotion", TraceLoggingInt32(smoothExposure, "fixedExposureLog2s"), TraceLoggingFloat64(smoothEv, "compensationEv"),
                  TraceLoggingInt32(smoothReasserts, "reasserts"), TraceLoggingHResult(smoothRestore, "restoreAuto"));
    }
}

void CALLBACK FrameProcessor::OnSettingsChanged(void* ctx, BOOLEAN) {
    auto* self = static_cast<FrameProcessor*>(ctx);
    FindNextChangeNotification(self->change_);  // re-arm before reading, so no change is missed
    self->ReloadSettings();
    {
        // Smooth motion switched on/off mid-session: restart it (turning it off restores auto exposure).
        std::lock_guard lock(self->smoothMu_);
        std::lock_guard l2(self->mu_);
        if (self->smoothSettingChanged_) {
            self->smoothSettingChanged_ = false;
            self->smooth_.End();
            self->smoothEnabled_ = self->profile_.smoothMotion;
            self->compensationEv_ = 0;
            self->Recompile();
            self->smooth_.Begin(self->ks_.Get(), self->smoothEnabled_ && self->nv12_, self->nominalFps_);
        }
    }
    self->UpdateFaceTracking();  // face tracking switched on/off or reconfigured
}

void FrameProcessor::UpdateFaceTracking() {
    bool want = false;
    face::EngineConfig cfg;
    {
        std::lock_guard lock(mu_);
        // Face-aware effects need the tracker even when "face tracking" itself isn't ticked.
        want = (profile_.faceTracking.enabled || (effects_ && effects_->needsFaces)) && nv12_ && type_ != nullptr;
        cfg = face::EngineConfigFor(profile_);
    }
    if (want) {
        std::lock_guard fl(faceMu_);
        if (face_.Running() && face::SameEngineConfig(cfg, faceConfig_)) return;
    }
    StopFaceTracking();  // off, or restarting with new settings
    if (!want) return;
    std::lock_guard fl(faceMu_);
    faceConfig_ = cfg;
    const bool started = face_.Start(cfg);
    IXC_TRACE("FaceTrackingStart", TraceLoggingBoolean(started, "started"), TraceLoggingInt32(cfg.maxFaces, "maxFaces"),
              TraceLoggingFloat64(cfg.cpuBudget, "cpuBudget"), TraceLoggingInt32(cfg.fixedIntervalFrames, "fixedIntervalFrames"));
}

void FrameProcessor::StopFaceTracking() {
    std::lock_guard fl(faceMu_);
    if (!face_.Running()) return;
    const double sessionMs = face::FaceEngine::NowMs() - face_.StartedAtMs();
    face_.Stop();  // joins the worker; its statistics stay readable
    const face::EngineStatus s = face_.Status();
    IXC_TRACE("FaceTracking", TraceLoggingString(face::ToString(s.lastActiveState), "state"),
              TraceLoggingString(face::ToString(s.path), "simd"), TraceLoggingInt32(s.inputWidth, "inputW"),
              TraceLoggingInt32(s.inputHeight, "inputH"), TraceLoggingUInt64(s.framesSeen, "frames"),
              TraceLoggingUInt64(s.detections, "detections"), TraceLoggingFloat64(sessionMs > 0 ? s.detections * 1000.0 / sessionMs : 0, "detectHz"),
              TraceLoggingUInt64(s.detectionsWithFace, "withFace"), TraceLoggingUInt64(s.landmarksValid, "landmarksValid"),
              TraceLoggingFloat64(s.avgDetectMs, "avgDetectMs"), TraceLoggingFloat64(s.maxDetectMs, "maxDetectMs"),
              TraceLoggingFloat64(s.avgStageMs, "avgStageMs"), TraceLoggingFloat64(s.maxStageMs, "maxStageMs"),
              TraceLoggingFloat64(sessionMs > 0 ? 100.0 * s.workerCpuMs / sessionMs : 0, "workerCpuPct"),
              TraceLoggingFloat64(s.warmupMs, "warmupMs"), TraceLoggingFloat64(sessionMs / 1000.0, "seconds"));
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
    smoothSettingChanged_ = profile_.smoothMotion != smoothEnabled_;
    Recompile();
    IXC_TRACE("SettingsLoaded", TraceLoggingBoolean(r.ok, "valid"), TraceLoggingBoolean(missing, "missing"),
              TraceLoggingBoolean(params_ && params_->identity, "identity"),
              TraceLoggingUInt32(static_cast<UINT32>(r.warnings.size()), "warnings"));
}

void FrameProcessor::Recompile() {
    ++generation_;  // new settings: the CPU/GPU decision is made again
    if (!nv12_ || width_ == 0 || height_ == 0) {
        effects_.reset();
        auto identity = std::make_shared<processing::PipelineParams>();
        params_ = std::move(identity);
        return;
    }
    // Smooth motion's software brightness compensation rides on the exposure tone step.
    Profile effective = profile_;
    effective.image.exposureEv += compensationEv_;
    params_ = std::make_shared<const processing::PipelineParams>(processing::CompileParams(effective, width_, height_, fullRange_));
    effects_ = effects::CompileEffects(profile_, fullRange_);
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
    // Smooth motion: watch the real frame rate/brightness until a decision is made (then this
    // costs nothing). A changed compensation recompiles the parameters for the next frames.
    {
        std::lock_guard lock(smoothMu_);
        if (smooth_.Active() && input) {
            UINT32 w = 0, h = 0;
            {
                std::lock_guard l2(mu_);
                w = width_;
                h = height_;
            }
            ComPtr<IMFMediaBuffer> b;
            if (SUCCEEDED(input->GetBufferByIndex(0, &b))) {
                LockedNv12 frame(b.Get(), w, h, false);
                if (frame.ok() && smooth_.OnFrame(frame.y(), frame.pitch(), static_cast<int>(w), static_cast<int>(h))) {
                    std::lock_guard l2(mu_);
                    compensationEv_ = smooth_.CompensationEv();
                    Recompile();
                }
            }
        }
    }

    // Face tracking: the buffer is locked only on the frames the engine will actually sample
    // (a few per second); every other frame costs two atomic reads.
    if (input) {
        std::lock_guard fl(faceMu_);
        const double now = face::FaceEngine::NowMs();
        if (face_.WantsFrame(now)) {
            UINT32 w = 0, h = 0;
            bool full = false;
            double interval = 33.3;
            {
                std::lock_guard l2(mu_);
                w = width_;
                h = height_;
                full = fullRange_;
                interval = frameIntervalMs_;
            }
            ComPtr<IMFMediaBuffer> b;
            if (SUCCEEDED(input->GetBufferByIndex(0, &b))) {
                LockedNv12 frame(b.Get(), w, h, false);
                if (frame.ok()) {
                    processing::YuvFormat fmt = processing::DefaultYuvFormat(h);
                    fmt.fullRange = full;
                    const processing::Nv12Planes planes{frame.y(), frame.uv(), frame.pitch(), frame.pitch(), static_cast<int>(w & ~1u),
                                                        static_cast<int>(h & ~1u)};
                    face_.OnFrame(planes, fmt, now, interval);
                }
            }
        }
    }

    std::shared_ptr<const processing::PipelineParams> params;
    ComPtr<IMFMediaType> type;
    UINT32 width = 0, height = 0;
    bool nv12 = false;
    std::uint64_t generation = 0;
    std::shared_ptr<const effects::EffectConfig> fx;
    {
        std::lock_guard lock(mu_);
        params = params_;
        generation = generation_;
        type = type_;
        width = width_;
        height = height_;
        nv12 = nv12_;
        fx = effects_ && effects_->Active() ? effects_ : nullptr;
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
    if (!input || !params || (params->identity && !fx) || !nv12 || !type) return passThrough();

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
            if (params->identity) {
                // Effects only: start from an exact copy of the camera frame.
                for (UINT32 y = 0; y < height; ++y) memcpy(o.y + static_cast<size_t>(y) * o.yStride, in.y + static_cast<size_t>(y) * in.yStride, width);
                for (UINT32 y = 0; y < height / 2; ++y) memcpy(o.uv + static_cast<size_t>(y) * o.uvStride, in.uv + static_cast<size_t>(y) * in.uvStride, width);
            } else {
                ok = processor_.Process(in, o, *params, generation);
            }
            if (ok && fx) {
                face::FaceSnapshot snap;
                if (fx->needsFaces) {
                    std::lock_guard fl(faceMu_);
                    face_.Snapshot(face::FaceEngine::NowMs(), snap);
                }
                effects::FrameContext ctx;
                ctx.faces = &snap;
                ctx.fullRange = fullRange_;
                if (!params->geometryIdentity) {
                    ctx.map = {static_cast<float>(params->srcX), static_cast<float>(params->srcY), static_cast<float>(params->srcW),
                               static_cast<float>(params->srcH), params->mirror};
                } else {
                    ctx.map.mirror = params->mirror;
                }
                renderer_.Apply(o, *fx, ctx);
            }
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
