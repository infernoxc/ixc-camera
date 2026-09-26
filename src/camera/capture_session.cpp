#include "camera/capture_session.h"

#include "camera/device_enum.h"
#include "camera/format_select.h"
#include "common/strings.h"
#include "diagnostics/log.h"

#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>

#include <vector>

using Microsoft::WRL::ComPtr;

namespace ixc::camera {

namespace {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kFlushTimeoutMs = 3000;
constexpr LONGLONG kArrivalSettleMs = 500;  // let a re-plugged camera finish initializing

Error MakeError(HRESULT hr, const char* stage, const std::string& summary) { return Error{hr, stage, summary}; }

void ArmTimer(PTP_TIMER timer, std::chrono::milliseconds delay) {
    // Negative FILETIME = relative time in 100 ns units.
    ULARGE_INTEGER due;
    due.QuadPart = static_cast<ULONGLONG>(-(static_cast<LONGLONG>(delay.count()) * 10000));
    FILETIME ft;
    ft.dwLowDateTime = due.LowPart;
    ft.dwHighDateTime = due.HighPart;
    SetThreadpoolTimer(timer, &ft, 0, 50);  // one-shot, 50 ms window lets Windows coalesce wakeups
}

}  // namespace

const char* ToString(CaptureState s) {
    switch (s) {
        case CaptureState::Stopped: return "Stopped";
        case CaptureState::Starting: return "Starting";
        case CaptureState::Streaming: return "Streaming";
        case CaptureState::Reconnecting: return "Reconnecting";
        case CaptureState::WaitingForDevice: return "WaitingForDevice";
        case CaptureState::Failed: return "Failed";
    }
    return "Unknown";
}

// ---- lifetime ---------------------------------------------------------------------------------

CaptureSession::CaptureSession(ICaptureListener* listener) : listener_(listener) {
    QueryPerformanceFrequency(&qpcFreq_);
}

CaptureSession::~CaptureSession() {
    // Close() must have run; these are defensive only.
    if (lossWork_) CloseThreadpoolWork(lossWork_);
    if (retryTimer_) CloseThreadpoolTimer(retryTimer_);
    if (flushEvent_) CloseHandle(flushEvent_);
}

HRESULT CaptureSession::Create(ICaptureListener* listener, ComPtr<CaptureSession>& out) {
    ComPtr<CaptureSession> s;
    s.Attach(new (std::nothrow) CaptureSession(listener));
    if (!s) return E_OUTOFMEMORY;
    const HRESULT hr = s->Init();
    if (FAILED(hr)) return hr;
    out = std::move(s);
    return S_OK;
}

HRESULT CaptureSession::Init() {
    flushEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    lossWork_ = CreateThreadpoolWork(&CaptureSession::LossWorkCallback, this, nullptr);
    retryTimer_ = CreateThreadpoolTimer(&CaptureSession::RetryTimerCallback, this, nullptr);
    if (!flushEvent_ || !lossWork_ || !retryTimer_) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

void CaptureSession::Close() {
    Stop();
    {
        std::lock_guard lock(listenerMu_);
        listener_ = nullptr;
    }
    if (retryTimer_) {
        SetThreadpoolTimer(retryTimer_, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(retryTimer_, TRUE);
        CloseThreadpoolTimer(retryTimer_);
        retryTimer_ = nullptr;
    }
    if (lossWork_) {
        WaitForThreadpoolWorkCallbacks(lossWork_, TRUE);
        CloseThreadpoolWork(lossWork_);
        lossWork_ = nullptr;
    }
}

// ---- IUnknown -----------------------------------------------------------------------------------

STDMETHODIMP CaptureSession::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CaptureSession::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) CaptureSession::Release() {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
}

// ---- state --------------------------------------------------------------------------------------

CaptureState CaptureSession::State() const {
    std::lock_guard lock(mu_);
    return state_;
}

Error CaptureSession::LastError() const {
    std::lock_guard lock(mu_);
    return lastError_;
}

CaptureFormat CaptureSession::ActiveFormat() const {
    std::lock_guard lock(mu_);
    return activeFormat_;
}

FrameLayout CaptureSession::Layout() const {
    std::lock_guard lock(mu_);
    return layout_;
}

FrameStatsSnapshot CaptureSession::Stats() const {
    std::lock_guard lock(statsMu_);
    return stats_.Snapshot();
}

int CaptureSession::ReconnectAttempts() const {
    std::lock_guard lock(mu_);
    return policy_.Attempts();
}

void CaptureSession::SetState(CaptureState s, const Error& e) {
    state_ = s;
    if (FAILED(e.hr)) lastError_ = e;
}

void CaptureSession::Notify(CaptureState s, const Error& e) {
    log::Info("capture", std::string("state -> ") + ToString(s) + (FAILED(e.hr) ? ": " + e.Describe() : std::string()));
    std::lock_guard lock(listenerMu_);
    if (listener_) listener_->OnStateChanged(s, e);
}

// ---- start / stop -------------------------------------------------------------------------------

Error CaptureSession::Start(const CaptureConfig& config) {
    Stop();

    Error err;
    {
        std::lock_guard control(controlMu_);
        {
            std::lock_guard lock(mu_);
            config_ = config;
            smoothWanted_.store(config.smoothMotion);
            stopRequested_ = false;
            policy_.Reset();
            lastError_ = {};
            SetState(CaptureState::Starting, {});
        }
        err = OpenLocked();
        if (FAILED(err.hr)) {
            std::lock_guard lock(mu_);
            stopRequested_ = true;
            SetState(CaptureState::Failed, err);
        }
    }

    if (FAILED(err.hr)) {
        Notify(CaptureState::Failed, err);
        return err;
    }
    RegisterDeviceNotifications();
    Notify(CaptureState::Streaming, {});
    return {};
}

void CaptureSession::Stop() {
    {
        std::lock_guard lock(mu_);
        if (stopRequested_ && state_ == CaptureState::Stopped && !reader_) return;
        stopRequested_ = true;  // blocks timers, notifications and work items from acting
    }
    UnregisterDeviceNotifications();
    // Wait for in-flight callbacks without holding controlMu_ (they acquire it).
    if (retryTimer_) {
        SetThreadpoolTimer(retryTimer_, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(retryTimer_, TRUE);
    }
    if (lossWork_) WaitForThreadpoolWorkCallbacks(lossWork_, TRUE);

    bool changed = false;
    {
        std::lock_guard control(controlMu_);
        TeardownLocked();
        std::lock_guard lock(mu_);
        changed = state_ != CaptureState::Stopped;
        SetState(CaptureState::Stopped, {});
    }
    frames_.Clear();
    if (changed) Notify(CaptureState::Stopped, {});
}

Error CaptureSession::OpenLocked() {
    CaptureConfig cfg;
    {
        std::lock_guard lock(mu_);
        cfg = config_;
    }
    const char* name = cfg.cameraName.empty() ? "the camera" : cfg.cameraName.c_str();
    const std::string openFail = std::string("IXC Camera could not open ") + name + ".";

    ComPtr<IMFMediaSource> source;
    HRESULT hr = CreateCameraSource(cfg.symbolicLink, source);
    if (FAILED(hr)) return MakeError(hr, "CreateCameraSource", openFail);

    auto fail = [&](HRESULT h, const char* stage, const std::string& summary) {
        source->Shutdown();
        return MakeError(h, stage, summary);
    };

    ComPtr<IMFAttributes> attr;
    hr = MFCreateAttributes(&attr, 4);
    if (SUCCEEDED(hr)) hr = attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this));
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (SUCCEEDED(hr) && cfg.output != OutputFormat::Native) {
        // Lets the reader insert the MJPEG decoder and colour converter when needed.
        hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    }
    if (FAILED(hr)) return fail(hr, "CreateReaderAttributes", openFail);

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), attr.Get(), &reader);
    if (FAILED(hr)) return fail(hr, "CreateSourceReader", openFail);

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader->SetStreamSelection(kVideoStream, TRUE);
    if (FAILED(hr)) return fail(hr, "SelectVideoStream", openFail);

    // Find the requested native mode. The camera may have changed since enumeration
    // (e.g. a different model plugged into the same port), so fall back to the closest mode
    // it really offers instead of failing.
    std::vector<CaptureFormat> natives;
    std::vector<ComPtr<IMFMediaType>> nativeTypes;
    for (DWORD i = 0; i < 1024; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetNativeMediaType(kVideoStream, i, &t))) break;
        CaptureFormat f;
        if (!FormatFromMediaType(t.Get(), f)) continue;
        f.nativeIndex = i;
        natives.push_back(f);
        nativeTypes.push_back(t);
    }
    if (natives.empty()) return fail(MF_E_INVALIDMEDIATYPE, "EnumerateNativeFormats", std::string(name) + " reports no video formats.");

    std::optional<size_t> pick;
    for (size_t i = 0; i < natives.size(); ++i) {
        if (natives[i].SameMode(cfg.format)) { pick = i; break; }
    }
    if (!pick) {
        pick = SelectFormat(natives, FormatRequest{cfg.format.width, cfg.format.height, cfg.format.Fps()});
        log::Warn("capture", "requested mode " + Describe(cfg.format) + " not offered; using " + Describe(natives[*pick]));
    }
    const CaptureFormat chosen = natives[*pick];

    hr = reader->SetCurrentMediaType(kVideoStream, nullptr, nativeTypes[*pick].Get());
    if (FAILED(hr)) return fail(hr, "SetNativeFormat", std::string("IXC Camera could not set ") + name + " to " + Describe(chosen) + ".");

    if (cfg.output != OutputFormat::Native) {
        ComPtr<IMFMediaType> outType;
        hr = MFCreateMediaType(&outType);
        if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_SUBTYPE, cfg.output == OutputFormat::Rgb32 ? MFVideoFormat_RGB32 : MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr = MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, chosen.width, chosen.height);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, chosen.fpsNumerator, chosen.fpsDenominator);
        if (SUCCEEDED(hr)) hr = outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(kVideoStream, nullptr, outType.Get());
        if (FAILED(hr)) return fail(hr, "SetOutputFormat", std::string("IXC Camera could not convert ") + Describe(chosen) + " for processing.");
    }

    FrameLayout layout;
    ComPtr<IMFMediaType> current;
    hr = reader->GetCurrentMediaType(kVideoStream, &current);
    if (FAILED(hr)) return fail(hr, "GetOutputFormat", openFail);
    current->GetGUID(MF_MT_SUBTYPE, &layout.subtype);
    MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &layout.width, &layout.height);
    layout.yuvMatrix = MFGetAttributeUINT32(current.Get(), MF_MT_YUV_MATRIX, 0);
    layout.nominalRange = MFGetAttributeUINT32(current.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0);
    UINT32 strideAttr = 0;
    if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideAttr))) {
        layout.stride = static_cast<std::int32_t>(strideAttr);
    } else {
        LONG s = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(layout.subtype.Data1, layout.width, &s))) layout.stride = s;
    }

    {
        std::lock_guard lock(statsMu_);
        const std::int64_t interval =
            chosen.fpsNumerator ? static_cast<std::int64_t>(1'000'000.0 * chosen.fpsDenominator / chosen.fpsNumerator) : 33333;
        stats_.Reset(interval);
    }
    frames_.Clear();
    frames_.ResetDropped();

    {
        std::lock_guard lock(mu_);
        if (stopRequested_) {
            // Stop() arrived while we were opening; don't resurrect the stream.
            source->Shutdown();
            return MakeError(E_ABORT, "Open", "Camera start was cancelled.");
        }
        reader_ = reader;
        source_ = source;
        activeFormat_ = chosen;
        awaitingFirstFrame_ = true;
        layout_ = layout;
        SetState(CaptureState::Streaming, {});
    }

    {
        std::lock_guard lock(smoothMu_);
        smoothBlocked_ = false;  // a fresh stream: Smooth motion starts on its first frame
    }
    hr = reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        TeardownLocked();
        return MakeError(hr, "StartReading", openFail);
    }
    log::Info("capture", "streaming " + std::string(name) + " " + Describe(chosen) + " -> " + SubtypeName(layout.subtype));
    return {};
}

void CaptureSession::TeardownLocked() {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaSource> source;
    {
        std::lock_guard lock(mu_);
        reader = std::move(reader_);
        source = std::move(source_);
    }
    {
        // Before the flush: the camera's controls reject changes once its reader is flushed
        // (MF 0xC00D36B6 on the Lenovo FHD Webcam). Callbacks can't restart it (smoothBlocked_).
        std::lock_guard lock(smoothMu_);
        smoothBlocked_ = true;
        if (smoothRunning_ && FAILED(smooth_.End())) log::Warn("capture", "could not give the camera its automatic exposure back");
        smoothRunning_ = false;
        smoothEv_.store(0);
    }
    if (reader) {
        // Flush cancels the pending ReadSample; wait for OnFlush so no callback for this reader
        // can arrive after it has been released.
        ResetEvent(flushEvent_);
        if (SUCCEEDED(reader->Flush(kVideoStream))) {
            if (WaitForSingleObject(flushEvent_, kFlushTimeoutMs) != WAIT_OBJECT_0) {
                log::Warn("capture", "timed out waiting for reader flush");
            }
        }
        reader.Reset();
    }
    if (source) source->Shutdown();  // releases the physical camera
    frames_.Clear();
}

// ---- streaming callbacks ------------------------------------------------------------------------

STDMETHODIMP CaptureSession::OnReadSample(HRESULT hrStatus, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) {
    ComPtr<IMFSourceReader> reader;
    {
        std::lock_guard lock(mu_);
        if (state_ != CaptureState::Streaming || stopRequested_ || !reader_) return S_OK;
        reader = reader_;
    }

    if (FAILED(hrStatus) || (flags & MF_SOURCE_READERF_ERROR)) {
        OnStreamFailure(FAILED(hrStatus) ? hrStatus : E_FAIL, "ReadSample");
        return S_OK;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        OnStreamFailure(MF_E_END_OF_STREAM, "EndOfStream");
        return S_OK;
    }
    if (flags & MF_SOURCE_READERF_STREAMTICK) {
        std::lock_guard lock(statsMu_);
        stats_.OnStreamTick();
    }

    if (sample) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const std::int64_t arrivalUs = static_cast<std::int64_t>(now.QuadPart * 1'000'000 / qpcFreq_.QuadPart);

        // MFSampleExtension_DeviceTimestamp is the capture time on the QPC-based MF clock.
        std::int64_t latencyUs = -1;
        UINT64 deviceTs = 0;
        if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &deviceTs))) {
            const std::int64_t d = static_cast<std::int64_t>(MFGetSystemTime()) - static_cast<std::int64_t>(deviceTs);
            if (d >= 0 && d < 20'000'000) latencyUs = d / 10;  // plausible: 0..2 s
        }
        {
            std::lock_guard lock(statsMu_);
            stats_.OnFrame(arrivalUs, timestamp / 10, latencyUs);
        }
        FeedSmoothMotion(sample);  // before handing the sample to the UI thread
        frames_.Put(ComPtr<IMFSample>(sample));
        {
            std::lock_guard lock(mu_);
            if (awaitingFirstFrame_) {
                awaitingFirstFrame_ = false;
                policy_.Reset();  // the stream is genuinely healthy again
            }
        }
        std::lock_guard lock(listenerMu_);
        if (listener_) listener_->OnFrameAvailable();
    }

    const HRESULT hr = reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) OnStreamFailure(hr, "ReadSample");
    return S_OK;
}

void CaptureSession::FeedSmoothMotion(IMFSample* sample) {
    std::lock_guard lock(smoothMu_);
    if (smoothBlocked_) return;
    const bool want = smoothWanted_.load();
    if (want != smoothRunning_) {
        if (want) {
            ComPtr<IMFMediaSource> source;
            FrameLayout layout;
            double fps = 0;
            {
                std::lock_guard l(mu_);
                source = source_;
                layout = layout_;
                if (activeFormat_.fpsDenominator) fps = static_cast<double>(activeFormat_.fpsNumerator) / activeFormat_.fpsDenominator;
            }
            ComPtr<IKsControl> ks;
            if (source) source.As(&ks);  // the camera's UVC controls; absent on some cameras
            smooth_.Begin(ks.Get(), IsEqualGUID(layout.subtype, MFVideoFormat_NV12), fps);
        } else {
            smooth_.End();
        }
        smoothRunning_ = want;
    }
    if (!smoothRunning_ || !smooth_.Active()) {
        smoothEv_.store(0);
        return;
    }

    if (!smooth_.NeedsLuma()) {
        smooth_.OnFrame(nullptr, 0, 0, 0);  // frame timing only (watchdog)
    } else {
        FrameLayout layout;
        {
            std::lock_guard l(mu_);
            layout = layout_;
        }
        ComPtr<IMFMediaBuffer> buffer;
        if (SUCCEEDED(sample->GetBufferByIndex(0, &buffer))) {
            ComPtr<IMF2DBuffer> buf2d;
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(buffer.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                if (pitch > 0) smooth_.OnFrame(scan0, pitch, static_cast<int>(layout.width), static_cast<int>(layout.height));
                buf2d->Unlock2D();
            } else {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                    const LONG stride = layout.stride > 0 ? layout.stride : static_cast<LONG>(layout.width);
                    if (static_cast<size_t>(stride) * layout.height <= length) {
                        smooth_.OnFrame(data, stride, static_cast<int>(layout.width), static_cast<int>(layout.height));
                    }
                    buffer->Unlock();
                }
            }
        }
    }
    smoothEv_.store(smooth_.CompensationEv());
}

STDMETHODIMP CaptureSession::OnFlush(DWORD) {
    SetEvent(flushEvent_);
    return S_OK;
}

STDMETHODIMP CaptureSession::OnEvent(DWORD, IMFMediaEvent*) { return S_OK; }

// ---- failure and recovery -----------------------------------------------------------------------

void CaptureSession::OnStreamFailure(HRESULT hr, const char* stage) {
    Error err = MakeError(hr, stage, "The camera stopped delivering video.");
    {
        std::lock_guard lock(mu_);
        if (stopRequested_ || state_ != CaptureState::Streaming) return;
        SetState(CaptureState::Reconnecting, err);
    }
    Notify(CaptureState::Reconnecting, err);
    SubmitThreadpoolWork(lossWork_);
}

void CALLBACK CaptureSession::LossWorkCallback(PTP_CALLBACK_INSTANCE, void* ctx, PTP_WORK) {
    static_cast<CaptureSession*>(ctx)->HandleLoss();
}

void CALLBACK CaptureSession::RetryTimerCallback(PTP_CALLBACK_INSTANCE, void* ctx, PTP_TIMER) {
    static_cast<CaptureSession*>(ctx)->AttemptReconnect();
}

void CaptureSession::HandleLoss() {
    std::lock_guard control(controlMu_);
    {
        std::lock_guard lock(mu_);
        if (stopRequested_ || state_ != CaptureState::Reconnecting) return;
    }
    TeardownLocked();  // release the dead source right away
    ScheduleRetryLocked();
}

void CaptureSession::ScheduleRetryLocked() {
    CaptureState notifyState;
    Error err;
    {
        std::lock_guard lock(mu_);
        if (stopRequested_) return;
        if (auto delay = policy_.NextDelay()) {
            log::Info("capture", "reconnect attempt " + std::to_string(policy_.Attempts()) + "/" +
                                     std::to_string(policy_.MaxAttempts()) + " in " + std::to_string(delay->count()) + " ms");
            ArmTimer(retryTimer_, *delay);
            return;
        }
        err = lastError_;
        SetState(CaptureState::WaitingForDevice, {});
        notifyState = CaptureState::WaitingForDevice;
    }
    Notify(notifyState, err);  // idle until the device re-appears
}

void CaptureSession::AttemptReconnect() {
    std::lock_guard control(controlMu_);
    {
        std::lock_guard lock(mu_);
        if (stopRequested_ || (state_ != CaptureState::Reconnecting && state_ != CaptureState::WaitingForDevice)) return;
        state_ = CaptureState::Reconnecting;
    }
    const Error err = OpenLocked();
    if (SUCCEEDED(err.hr)) {
        // The backoff is reset only when a frame actually arrives (OnReadSample). A camera that
        // opens but then fails every read (e.g. held by another app) must still run out of retries.
        Notify(CaptureState::Streaming, {});
        return;
    }
    if (err.hr == E_ABORT) return;

    if (ClassifyHResult(err.hr) == ErrorClass::AccessDenied) {
        {
            std::lock_guard lock(mu_);
            SetState(CaptureState::Failed, err);
        }
        Notify(CaptureState::Failed, err);  // never retry a privacy denial
        return;
    }
    {
        std::lock_guard lock(mu_);
        if (stopRequested_) return;
        SetState(CaptureState::Reconnecting, err);  // OpenLocked may have reached Streaming before failing
    }
    log::Debug("capture", "reconnect failed: " + err.Describe());
    ScheduleRetryLocked();
}

// ---- device notifications -----------------------------------------------------------------------

void CaptureSession::RegisterDeviceNotifications() {
    if (deviceNotify_) return;
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = KSCATEGORY_VIDEO_CAMERA;
    const CONFIGRET cr = CM_Register_Notification(&filter, this, &CaptureSession::DeviceNotifyCallback, &deviceNotify_);
    if (cr != CR_SUCCESS) {
        deviceNotify_ = nullptr;
        log::Warn("capture", "device notifications unavailable (CONFIGRET " + std::to_string(cr) +
                                 "); reconnect will rely on backoff retries only");
    }
}

void CaptureSession::UnregisterDeviceNotifications() {
    if (deviceNotify_) {
        CM_Unregister_Notification(deviceNotify_);  // waits for in-flight callbacks
        deviceNotify_ = nullptr;
    }
}

DWORD CALLBACK CaptureSession::DeviceNotifyCallback(HCMNOTIFICATION, void* ctx, CM_NOTIFY_ACTION action,
                                                    PCM_NOTIFY_EVENT_DATA data, DWORD) {
    auto* self = static_cast<CaptureSession*>(ctx);
    if (!data || data->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE) return ERROR_SUCCESS;
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
        self->OnDeviceInterfaceChange(true, data->u.DeviceInterface.SymbolicLink);
    } else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        self->OnDeviceInterfaceChange(false, data->u.DeviceInterface.SymbolicLink);
    }
    return ERROR_SUCCESS;
}

void CaptureSession::OnDeviceInterfaceChange(bool arrival, const wchar_t* link) {
    std::wstring target;
    CaptureState state;
    {
        std::lock_guard lock(mu_);
        if (stopRequested_) return;
        target = config_.symbolicLink;
        state = state_;
    }
    if (!link || !SameDevice(target, link)) return;

    if (!arrival) {
        log::Info("capture", "camera removed");
        if (state == CaptureState::Streaming) OnStreamFailure(HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED), "DeviceRemoved");
        return;
    }
    log::Info("capture", "camera arrived");
    std::lock_guard lock(mu_);
    if (stopRequested_) return;
    if (state_ == CaptureState::Reconnecting || state_ == CaptureState::WaitingForDevice) {
        policy_.Reset();
        ArmTimer(retryTimer_, std::chrono::milliseconds(kArrivalSettleMs));
    }
}

}  // namespace ixc::camera
