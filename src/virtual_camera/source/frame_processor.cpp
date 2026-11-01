#include "virtual_camera/source/frame_processor.h"

#include "face/face_settings.h"
#include "processing/yuy2.h"
#include "profiles/active_profile.h"
#include "segmentation/gpu/gpu_selfie_net.h"
#include "virtual_camera/source/trace.h"

#include <mferror.h>

#include <algorithm>
#include <optional>

using Microsoft::WRL::ComPtr;

namespace ixc::vcam {

namespace {

// Locks a video buffer for reading/writing and reports the plane layout. Handles padded
// planes: the UV plane follows the (possibly height-aligned) Y plane.
class LockedNv12 {
public:
    LockedNv12(IMFMediaBuffer* buffer, UINT32 width, UINT32 height, bool write, LONG fallbackPitch = 0) {
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b2d_))) &&
            SUCCEEDED(b2d_->Lock2DSize(write ? MF2DBuffer_LockFlags_Write : MF2DBuffer_LockFlags_Read, &scan0_, &pitch_, &start_, &length_))) {
            locked2d_ = true;
        } else if (SUCCEEDED(buffer->Lock(&start_, nullptr, &length_))) {
            buffer_ = buffer;
            scan0_ = start_;
            // A plain lock gives no pitch: use the type's stride (cameras with padded rows, e.g.
            // 1366x768), else tightly packed rows.
            pitch_ = fallbackPitch >= static_cast<LONG>(width) ? fallbackPitch : static_cast<LONG>(width);
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

// Packed 4:2:2 (YUY2) buffer: two bytes per pixel, no second plane.
class LockedPacked {
public:
    LockedPacked(IMFMediaBuffer* buffer, UINT32 width, UINT32 height, bool write, LONG fallbackPitch) {
        if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&b2d_))) &&
            SUCCEEDED(b2d_->Lock2DSize(write ? MF2DBuffer_LockFlags_Write : MF2DBuffer_LockFlags_Read, &scan0_, &pitch_, &start_, &length_))) {
            locked2d_ = true;
        } else if (SUCCEEDED(buffer->Lock(&start_, nullptr, &length_))) {
            buffer_ = buffer;
            scan0_ = start_;
            pitch_ = fallbackPitch >= static_cast<LONG>(width * 2) ? fallbackPitch : static_cast<LONG>(width * 2);
        } else {
            return;
        }
        if (pitch_ < static_cast<LONG>(width * 2)) return;  // (a bottom-up buffer reports a negative pitch: not handled)
        const size_t used = length_ - static_cast<size_t>(scan0_ - start_);
        if (used < static_cast<size_t>(pitch_) * (height - 1) + static_cast<size_t>(width) * 2) return;
        ok_ = true;
    }
    ~LockedPacked() {
        if (locked2d_) b2d_->Unlock2D();
        else if (buffer_) buffer_->Unlock();
    }
    LockedPacked(const LockedPacked&) = delete;
    LockedPacked& operator=(const LockedPacked&) = delete;
    bool ok() const { return ok_; }
    BYTE* data() const { return scan0_; }
    int pitch() const { return static_cast<int>(pitch_); }

private:
    ComPtr<IMF2DBuffer2> b2d_;
    IMFMediaBuffer* buffer_ = nullptr;
    BYTE* scan0_ = nullptr;
    BYTE* start_ = nullptr;
    LONG pitch_ = 0;
    DWORD length_ = 0;
    bool locked2d_ = false;
    bool ok_ = false;
};

// CPU access to a sample's picture as NV12 planes, whatever the camera's format: NV12 is read in
// place; YUY2 is converted into `scratch` (sized here, reused across frames) and the buffer is
// unlocked again before returning.
class InputPlanes {
public:
    InputPlanes(IMFMediaBuffer* buffer, UINT32 width, UINT32 height, bool yuy2, LONG stride, std::vector<std::uint8_t>& scratch) {
        const int w = static_cast<int>(width & ~1u), h = static_cast<int>(height & ~1u);
        if (!yuy2) {
            nv12_.emplace(buffer, width, height, false, stride);
            if (nv12_->ok()) {
                planes_ = {nv12_->y(), nv12_->uv(), nv12_->pitch(), nv12_->pitch(), w, h};
                ok_ = true;
            }
            return;
        }
        const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
        if (scratch.size() != bytes) scratch.assign(bytes, 0);  // once per session
        LockedPacked packed(buffer, width, height, false, stride);
        if (!packed.ok()) return;
        const processing::Nv12Frame frame{scratch.data(), scratch.data() + static_cast<size_t>(w) * h, w, w, w, h};
        processing::Yuy2ToNv12(packed.data(), packed.pitch(), frame);
        planes_ = {frame.y, frame.uv, w, w, w, h};
        ok_ = true;
    }
    bool ok() const { return ok_; }
    const processing::Nv12Planes& planes() const { return planes_; }
    // Plane pointers for code that wants raw access (smooth motion reads luma).
    int pitch() const { return planes_.yStride; }

private:
    std::optional<LockedNv12> nv12_;
    processing::Nv12Planes planes_;
    bool ok_ = false;
};

// Identity of the settings file's current version (each publish replaces it with a new file).
struct SettingsStamp {
    FILETIME written{};
    DWORD sizeLow = 0;
    bool exists = false;
    bool operator==(const SettingsStamp& o) const {
        return exists == o.exists && sizeLow == o.sizeLow && written.dwLowDateTime == o.written.dwLowDateTime &&
               written.dwHighDateTime == o.written.dwHighDateTime;
    }
};

SettingsStamp StampOf(const std::filesystem::path& file) {
    SettingsStamp s;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!file.empty() && GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &a)) {
        s.exists = true;
        s.written = a.ftLastWriteTime;
        s.sizeLow = a.nFileSizeLow;
    }
    return s;
}

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
        yuy2_ = type && !nv12_ && IsEqualGUID(sub, MFVideoFormat_YUY2);
        if (type) MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width_, &height_);
        {
            const LONG st = type ? static_cast<LONG>(MFGetAttributeUINT32(type, MF_MT_DEFAULT_STRIDE, 0)) : 0;
            stride_ = st < 0 ? -st : st;  // bottom-up types report a negative stride
        }
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
        // ReloadSettings keeps compiled settings when the profile file is unchanged, but this
        // session can have another size, range or format than the last one: always compile for it.
        std::lock_guard lock(mu_);
        Recompile();
    }
    {
        std::lock_guard lock(smoothMu_);
        std::lock_guard l2(mu_);
        smoothEnabled_ = profile_.smoothMotion;
        // Anti-flicker first: the camera's auto exposure (which Smooth motion observes) follows it.
        // The app publishes Auto already resolved for the user's region; Auto here = leave as is.
        powerLine_.Apply(ks_.Get(), profile_.antiFlicker);
        smooth_.Begin(ks_.Get(), smoothEnabled_ && nv12_, nominalFps_);
    }
    UpdateFaceTracking();
    UpdateSegmentation();

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
              TraceLoggingBoolean(nv12_, "nv12"), TraceLoggingBoolean(yuy2_, "yuy2"), TraceLoggingInt32(stride_, "stride"),
              TraceLoggingBoolean(fullRange_, "fullRange"),
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
    StopSegmentation();
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
        powerLine_.Apply(nullptr, AntiFlicker::Auto);  // and its own power-line setting
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
    {
        std::lock_guard sc(scratchMu_);  // (never taken while holding mu_)
        std::vector<std::uint8_t>().swap(inScratch_);  // an idle camera holds no YUY2 working copies
        std::vector<std::uint8_t>().swap(outScratch_);
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
                  TraceLoggingUInt64(c.poolExhausted, "poolExhausted"), TraceLoggingUInt64(c.errors, "errors"), TraceLoggingUInt64(c.sampleFailures, "sampleFailures"),
                  TraceLoggingUInt64(c.settingsReloads, "reloads"), TraceLoggingBoolean(c.inputIsGpuSurface, "gpuInput"),
                  TraceLoggingUInt64(c.processed ? c.processUsTotal / c.processed : 0, "avgUs"), TraceLoggingUInt64(c.processUsMax, "maxUs"));
        IXC_TRACE("SmoothMotion", TraceLoggingInt32(smoothExposure, "fixedExposureLog2s"), TraceLoggingFloat64(smoothEv, "compensationEv"),
                  TraceLoggingInt32(smoothReasserts, "reasserts"), TraceLoggingHResult(smoothRestore, "restoreAuto"));
    }
}

void CALLBACK FrameProcessor::OnSettingsChanged(void* ctx, BOOLEAN) {
    auto* self = static_cast<FrameProcessor*>(ctx);
    FindNextChangeNotification(self->change_);  // re-arm before reading, so no change is missed
    // The app's temp file wakes this before the new settings are renamed into place. If the file
    // changed while it was being read, read it again right away instead of relying on the next
    // notification (a missed one would leave the previous settings until the next change).
    for (int pass = 0; pass < 4; ++pass) {
        const SettingsStamp before = StampOf(ActiveProfilePath());
        self->ReloadSettings();
        if (StampOf(ActiveProfilePath()) == before) break;
    }
    {
        // Smooth motion switched on/off mid-session: restart it (turning it off restores auto exposure).
        std::lock_guard lock(self->smoothMu_);
        std::lock_guard l2(self->mu_);
        if (self->profile_.antiFlicker != self->powerLine_.applied()) self->powerLine_.Apply(self->ks_.Get(), self->profile_.antiFlicker);
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
    self->UpdateSegmentation();  // a background effect switched on/off
}

void FrameProcessor::UpdateSegmentation() {
    bool want = false;
    ProcessingMode mode = ProcessingMode::Auto;
    float temporal = 0.5f, hair = 0.5f;
    {
        std::lock_guard lock(mu_);
        want = effects_ && effects_->needsSegmentation && Processable() && type_ != nullptr;
        mode = profile_.processing;
        temporal = static_cast<float>(profile_.background.temporal / 100.0);
        hair = static_cast<float>(profile_.background.hair / 100.0);
    }
    {
        std::lock_guard sl(segMu_);
        seg_.SetGpuFactory(&seg::MakeGpuRunner);
        seg_.SetMode(mode);  // live: the worker switches between two masks
        seg_.SetRefinement(temporal, hair);
        if (want == seg_.Running()) return;
    }
    if (!want) {
        StopSegmentation();
        return;
    }
    std::lock_guard sl(segMu_);
    const bool started = seg_.Start();
    IXC_TRACE("SegmentationStart", TraceLoggingBoolean(started, "started"));
}

void FrameProcessor::StopSegmentation() {
    std::lock_guard sl(segMu_);
    if (!seg_.Running()) return;
    const seg::SegStatus s = seg_.Status();
    seg_.Stop();
    IXC_TRACE("Segmentation", TraceLoggingString(seg::ToString(s.state), "state"), TraceLoggingString(seg::ToString(s.backend), "backend"),
              TraceLoggingString(s.device.c_str(), "device"), TraceLoggingString(s.gpuNote.c_str(), "gpuNote"),
              TraceLoggingFloat64(s.avgNetMs, "avgNetMs"), TraceLoggingUInt64(s.masks, "masks"),
              TraceLoggingFloat64(s.avgRunMs, "avgRunMs"), TraceLoggingFloat64(s.maxRunMs, "maxRunMs"),
              TraceLoggingFloat64(s.avgStageMs, "avgStageMs"), TraceLoggingFloat64(s.masksPerSecond, "masksPerSecond"));
}

void FrameProcessor::UpdateFaceTracking() {
    bool want = false;
    face::EngineConfig cfg;
    {
        std::lock_guard lock(mu_);
        // Face-aware effects need the tracker even when "face tracking" itself isn't ticked.
        want = (profile_.faceTracking.enabled || profile_.autoFraming || (effects_ && effects_->needsFaces)) && Processable() && type_ != nullptr;
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
    // One reload at a time: a write-then-rename produces several change notifications, and their
    // callbacks may overlap on the thread pool. Each reload reads the whole, final file.
    std::lock_guard reload(reloadMu_);
    // Parsing (a few KB of strictly validated JSON) happens here, never on the frame path.
    bool missing = false;
    ProfileLoadResult r = LoadActiveProfile(&missing);
    // The background picture is read from disk here (settings thread), outside the frame lock.
    std::shared_ptr<const effects::BackgroundImage> picture;
    if (r.ok) picture = bgSource_.Resolve(r.profile.background, BackgroundsDirectory());
    Profile next = r.ok ? r.profile : Profile{};
    if (!r.ok) next.image.sharpness = 0;  // no usable settings at all: pass the camera through unchanged
    // What compilation depends on, read under the lock; then compile outside it, so a frame never
    // waits for the LUTs and effect tables to be built.
    bool processable = false, full = false;
    UINT32 w = 0, h = 0;
    double ev = 0;
    {
        std::lock_guard lock(mu_);
        // Unchanged profile (the folder also changes for temp files): keep the compiled params.
        if (r.ok && profileValid_ && r.profile == profile_ && picture == bgPicture_) return;
        if (!r.ok && !missing && params_) {
            // Present but unreadable/invalid (e.g. caught mid-write, or a bad hand edit): keep the
            // last good settings instead of flickering to neutral.
            IXC_TRACE("SettingsRejectedKeptPrevious");
            return;
        }
        processable = Processable();
        w = width_;
        h = height_;
        full = fullRange_;
        ev = compensationEv_;
    }
    std::shared_ptr<const processing::PipelineParams> params;
    std::shared_ptr<const effects::EffectConfig> fx;
    if (processable && w && h) {
        const Profile effective = processing::WithSmoothMotionGain(next, ev);
        params = std::make_shared<const processing::PipelineParams>(processing::CompileParams(effective, w, h, full));
        fx = effects::CompileEffects(effective, full, picture);
    }
    std::lock_guard lock(mu_);
    profileValid_ = r.ok;
    profile_ = std::move(next);
    bgPicture_ = std::move(picture);
    ++counters_.settingsReloads;
    smoothSettingChanged_ = profile_.smoothMotion != smoothEnabled_;
    if (params && Processable() == processable && width_ == w && height_ == h && fullRange_ == full && compensationEv_ == ev) {
        // The compiled snapshot is swapped in whole: a frame uses either the old or the new one.
        ++generation_;
        params_ = std::move(params);
        effects_ = std::move(fx);
    } else {
        Recompile();  // the session changed meanwhile (rare): compile for the current one
    }
    IXC_TRACE("SettingsLoaded", TraceLoggingBoolean(r.ok, "valid"), TraceLoggingBoolean(missing, "missing"),
              TraceLoggingBoolean(params_ && params_->identity, "identity"),
              TraceLoggingUInt32(static_cast<UINT32>(r.warnings.size()), "warnings"));
}

void FrameProcessor::Recompile() {
    ++generation_;  // new settings: the CPU/GPU decision is made again
    if (!Processable() || width_ == 0 || height_ == 0) {
        effects_.reset();
        auto identity = std::make_shared<processing::PipelineParams>();
        params_ = std::move(identity);
        return;
    }
    // Smooth motion's software brightness compensation rides on the exposure tone step (and
    // raises temporal denoise to match).
    const Profile effective = processing::WithSmoothMotionGain(profile_, compensationEv_);
    params_ = std::make_shared<const processing::PipelineParams>(processing::CompileParams(effective, width_, height_, fullRange_));
    effects_ = effects::CompileEffects(effective, fullRange_, bgPicture_);
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
            bool full = false, yuy2 = false;
            LONG stride = 0;
            double interval = 33.3;
            {
                std::lock_guard l2(mu_);
                w = width_;
                h = height_;
                full = fullRange_;
                yuy2 = yuy2_;
                stride = stride_;
                interval = frameIntervalMs_;
            }
            ComPtr<IMFMediaBuffer> b;
            bool unreadable = false;
            if (SUCCEEDED(input->GetBufferByIndex(0, &b))) {
                std::lock_guard sc(scratchMu_);
                const InputPlanes frame(b.Get(), w, h, yuy2, stride, inScratch_);
                if (frame.ok()) {
                    processing::YuvFormat fmt = processing::DefaultYuvFormat(h);
                    fmt.fullRange = full;
                    face_.OnFrame(frame.planes(), fmt, now, interval);  // (copies what it needs before returning)
                } else {
                    unreadable = true;
                }
            }
            if (unreadable) {
                std::lock_guard l2(mu_);
                ++counters_.sampleFailures;  // tracking can't see this camera's frames: shown in the trace
            }
        }
    }

    // Person segmentation: same pattern as face tracking (sampled only when a mask is due).
    if (input) {
        std::lock_guard sl(segMu_);
        const double now = seg::SegmentationEngine::NowMs();
        if (seg_.WantsFrame(now)) {
            UINT32 w = 0, h = 0;
            bool full = false, yuy2 = false;
            LONG stride = 0;
            {
                std::lock_guard l2(mu_);
                w = width_;
                h = height_;
                full = fullRange_;
                yuy2 = yuy2_;
                stride = stride_;
            }
            ComPtr<IMFMediaBuffer> b;
            bool unreadable = false;
            if (SUCCEEDED(input->GetBufferByIndex(0, &b))) {
                std::lock_guard sc(scratchMu_);
                const InputPlanes frame(b.Get(), w, h, yuy2, stride, inScratch_);
                if (frame.ok()) {
                    processing::YuvFormat fmt = processing::DefaultYuvFormat(h);
                    fmt.fullRange = full;
                    seg_.OnFrame(frame.planes(), fmt, now);
                } else {
                    unreadable = true;
                }
            }
            if (unreadable) {
                std::lock_guard l2(mu_);
                ++counters_.sampleFailures;
            }
        }
    }

    std::shared_ptr<const processing::PipelineParams> params;
    ComPtr<IMFMediaType> type;
    UINT32 width = 0, height = 0;
    bool processable = false, yuy2 = false;
    LONG stride = 0;
    std::uint64_t generation = 0;
    std::shared_ptr<const effects::EffectConfig> fx;
    bool autoFraming = false;
    face::FramerParams framing;
    {
        std::lock_guard lock(mu_);
        params = params_;
        generation = generation_;
        autoFraming = profile_.autoFraming;
        framing = face::FramerParamsFor(profile_.framing.tightness, profile_.framing.speed, profile_.framing.maxZoom);
        type = type_;
        width = width_;
        height = height_;
        processable = Processable();
        yuy2 = yuy2_;
        stride = stride_;
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
    // Faces for this frame (auto-framing and face effects share one snapshot).
    face::FaceSnapshot snap;
    if ((autoFraming || (fx && fx->needsFaces)) && input) {
        std::lock_guard fl(faceMu_);
        face_.Snapshot(face::FaceEngine::NowMs(), snap);
    }
    // Auto-framing: a per-frame copy of the parameters with the framed source rectangle (a stack
    // copy, no allocation). Its own generation key lets the CPU/GPU choice see the zoom.
    processing::PipelineParams framed;
    if (autoFraming && processable && params) {
        const face::ViewRect base{params->srcX, params->srcY, params->srcW, params->srcH};
        framer_.SetParams(framing);
        const face::ViewRect v = framer_.Update(base, face::LargestFace(snap), face::FaceEngine::NowMs());
        if (framer_.Framing()) {
            framed = *params;
            framed.srcX = v.x;
            framed.srcY = v.y;
            framed.srcW = v.w;
            framed.srcH = v.h;
            framed.geometryIdentity = false;
            framed.identity = false;
            params = std::shared_ptr<const processing::PipelineParams>(params, &framed);  // aliasing: no allocation
            generation |= 1ull << 63;
        }
    } else if (!autoFraming) {
        framer_.Reset();
    }

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);

    auto passThrough = [&] {
        std::lock_guard lock(mu_);
        ++counters_.passedThrough;
        return ComPtr<IMFSample>(input);
    };
    if (!fx) renderer_.ReleaseBackground();  // no effects: hold no background buffers
    if (!input || !params || (params->identity && !fx) || !processable || !type) return passThrough();

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
        // YUY2 cameras are processed through NV12 working copies (scratchMu_ guards them).
        std::unique_lock<std::mutex> scratch(scratchMu_, std::defer_lock);
        if (yuy2) scratch.lock();
        const InputPlanes src(inBuf.Get(), width, height, yuy2, stride, inScratch_);
        std::optional<LockedNv12> dst;
        processing::Nv12Frame o{};
        ok = src.ok();
        if (ok) {
            const int w = static_cast<int>(width) & ~1, h = static_cast<int>(height) & ~1;
            if (yuy2) {
                const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
                if (outScratch_.size() != bytes) outScratch_.assign(bytes, 0);  // once per session
                o = {outScratch_.data(), outScratch_.data() + static_cast<size_t>(w) * h, w, w, w, h};
            } else {
                dst.emplace(outBuf.Get(), width, height, true, stride);
                ok = dst->ok();
                if (ok) o = {dst->y(), dst->uv(), dst->pitch(), dst->pitch(), w, h};
            }
        }
        if (ok) {
            const processing::Nv12Planes& in = src.planes();
            if (params->identity) {
                // Effects only: start from an exact copy of the camera frame.
                for (int y = 0; y < o.height; ++y)
                    memcpy(o.y + static_cast<size_t>(y) * o.yStride, in.y + static_cast<size_t>(y) * in.yStride, static_cast<size_t>(o.width));
                for (int y = 0; y < o.height / 2; ++y)
                    memcpy(o.uv + static_cast<size_t>(y) * o.uvStride, in.uv + static_cast<size_t>(y) * in.uvStride, static_cast<size_t>(o.width));
            } else {
                ok = processor_.Process(in, o, *params, generation);
            }
            if (ok && fx) {
                effects::FrameContext ctx;
                ctx.faces = &snap;
                ctx.fullRange = fullRange_;
                if (fx->needsSegmentation) {
                    seg_.Snapshot(segMask_);  // copies only when a new mask exists
                    ctx.mask = &segMask_;
                }
                if (!params->geometryIdentity) {
                    ctx.map = {static_cast<float>(params->srcX), static_cast<float>(params->srcY), static_cast<float>(params->srcW),
                               static_cast<float>(params->srcH), params->mirror};
                } else {
                    ctx.map.mirror = params->mirror;
                }
                renderer_.Apply(o, *fx, ctx);
            }
            if (ok && yuy2) {
                LockedPacked packed(outBuf.Get(), width, height, true, stride);
                ok = packed.ok();
                if (ok) processing::Nv12ToYuy2({o.y, o.uv, o.yStride, o.uvStride, o.width, o.height}, packed.data(), packed.pitch());
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
