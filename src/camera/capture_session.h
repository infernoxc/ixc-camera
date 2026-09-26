#pragma once

// Asynchronous camera capture on the Media Foundation Source Reader.
//
// * Event-driven: frames arrive through IMFSourceReaderCallback. There are no polling threads.
// * Newest-frame policy: frames go into a single-slot mailbox. A frame the consumer didn't take
//   is dropped instead of queued.
// * Zero-copy handoff: the consumer receives the IMFSample itself.
// * Bounded recovery: on device loss the session retries with backoff (ReconnectPolicy), then
//   waits for a device-arrival notification without using CPU. Privacy denials are never retried.
// * The physical camera is held only between Start() and Stop().
//
// Threading: Start/Stop/Close may be called from any single controlling thread. Listener
// methods are called on internal threads. They must be non-blocking (e.g. PostMessage) and
// must not call back into the session.

#include "camera/camera_types.h"
#include "camera/frame_stats.h"
#include "camera/latest_mailbox.h"
#include "camera/reconnect_policy.h"
#include "camera/smooth_motion.h"
#include "diagnostics/error.h"

#include <cfgmgr32.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>

namespace ixc::camera {

enum class CaptureState { Stopped, Starting, Streaming, Reconnecting, WaitingForDevice, Failed };
const char* ToString(CaptureState s);

enum class OutputFormat {
    Native,  // whatever the camera delivers (MJPG stays compressed): cheapest, used for measurement
    Nv12,    // processing pipeline format
    Rgb32,   // simple GDI preview
};

struct CaptureConfig {
    std::wstring symbolicLink;
    std::string cameraName;
    CaptureFormat format;  // requested native mode (from EnumerateFormats)
    OutputFormat output = OutputFormat::Nv12;
    bool smoothMotion = false;  // keep the negotiated frame rate in low light (camera/smooth_motion.h)
};

struct FrameLayout {
    GUID subtype{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t stride = 0;  // bytes per row; negative means bottom-up
    std::uint32_t yuvMatrix = 0;     // MFVideoTransferMatrix, 0 = not declared by the source
    std::uint32_t nominalRange = 0;  // MFNominalRange, 0 = not declared by the source
};

class ICaptureListener {
public:
    virtual ~ICaptureListener() = default;
    virtual void OnFrameAvailable() = 0;
    virtual void OnStateChanged(CaptureState state, const Error& error) = 0;
};

class CaptureSession final : public IMFSourceReaderCallback {
public:
    static HRESULT Create(ICaptureListener* listener, Microsoft::WRL::ComPtr<CaptureSession>& out);

    // Opens the camera and starts streaming. Synchronous; returns the failure with its stage.
    Error Start(const CaptureConfig& config);
    // Stops streaming and releases the physical camera. Idempotent.
    void Stop();
    // Stop + release all internal threadpool objects. Call before the final Release().
    void Close();

    std::optional<Microsoft::WRL::ComPtr<IMFSample>> TakeFrame() { return frames_.Take(); }

    CaptureState State() const;
    Error LastError() const;
    CaptureFormat ActiveFormat() const;
    FrameLayout Layout() const;
    FrameStatsSnapshot Stats() const;
    std::uint64_t DroppedFrames() const { return frames_.Dropped(); }
    int ReconnectAttempts() const;

    // Smooth motion: live on/off, and the software brightness gain (EV) the preview must add so it
    // matches what the camera delivered before its exposure was fixed. 0 when inactive.
    void SetSmoothMotion(bool enabled) { smoothWanted_.store(enabled); }
    double SmoothCompensationEv() const { return smoothEv_.load(); }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD streamIndex, DWORD flags, LONGLONG timestamp, IMFSample* sample) override;
    STDMETHODIMP OnFlush(DWORD streamIndex) override;
    STDMETHODIMP OnEvent(DWORD streamIndex, IMFMediaEvent* event) override;

private:
    explicit CaptureSession(ICaptureListener* listener);
    ~CaptureSession();

    HRESULT Init();
    Error OpenLocked();          // requires controlMu_
    void TeardownLocked();       // requires controlMu_
    void ScheduleRetryLocked();  // requires controlMu_
    void OnStreamFailure(HRESULT hr, const char* stage);
    void SetState(CaptureState s, const Error& e);  // requires mu_
    void Notify(CaptureState s, const Error& e);
    void RegisterDeviceNotifications();
    void UnregisterDeviceNotifications();

    static void CALLBACK LossWorkCallback(PTP_CALLBACK_INSTANCE, void* ctx, PTP_WORK);
    static void CALLBACK RetryTimerCallback(PTP_CALLBACK_INSTANCE, void* ctx, PTP_TIMER);
    static DWORD CALLBACK DeviceNotifyCallback(HCMNOTIFICATION, void* ctx, CM_NOTIFY_ACTION action,
                                               PCM_NOTIFY_EVENT_DATA data, DWORD size);
    void FeedSmoothMotion(IMFSample* sample);  // reader callback only
    void HandleLoss();
    void AttemptReconnect();
    void OnDeviceInterfaceChange(bool arrival, const wchar_t* link);

    std::atomic<ULONG> refs_{1};

    std::mutex controlMu_;  // serializes open/teardown
    mutable std::mutex mu_; // guards the fields below
    CaptureConfig config_;
    CaptureState state_ = CaptureState::Stopped;
    Error lastError_;
    bool stopRequested_ = true;
    bool awaitingFirstFrame_ = false;  // backoff resets only after a real frame
    CaptureFormat activeFormat_;
    FrameLayout layout_;
    ReconnectPolicy policy_;
    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    Microsoft::WRL::ComPtr<IMFMediaSource> source_;

    mutable std::mutex statsMu_;
    FrameStats stats_;

    // Smooth motion runs on the reader callback (serialized); started/stopped with the stream.
    std::mutex smoothMu_;
    SmoothMotion smooth_;
    bool smoothRunning_ = false;  // Begin() called for the current stream
    bool smoothBlocked_ = true;   // between teardown and the next successful open
    double smoothFps_ = 0;
    std::atomic<bool> smoothWanted_{false};
    std::atomic<double> smoothEv_{0};

    std::mutex listenerMu_;
    ICaptureListener* listener_;

    LatestMailbox<Microsoft::WRL::ComPtr<IMFSample>> frames_;

    HANDLE flushEvent_ = nullptr;
    PTP_WORK lossWork_ = nullptr;
    PTP_TIMER retryTimer_ = nullptr;
    HCMNOTIFICATION deviceNotify_ = nullptr;
    LARGE_INTEGER qpcFreq_{};
};

}  // namespace ixc::camera
