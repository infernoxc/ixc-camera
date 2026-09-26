#pragma once

// One output stream of the IXC Camera source. It wraps the matching stream of the physical
// camera: sample requests are forwarded to it, and every delivered frame passes through
// ProcessSample(), the single hook where IXC's image pipeline runs (identity in Phase 4).
//
// The stream exposes only uncompressed NV12 media types, so every mode an app can pick is one
// IXC can process. Frame rates and sizes are exactly the physical camera's.

#include "virtual_camera/source/frame_processor.h"

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/implements.h>

#include <mutex>
#include <vector>

namespace ixc::vcam {

class MediaSource;

class MediaStream final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>> {
public:
    // devDescriptor: stream descriptor of the physical camera stream being wrapped.
    HRESULT RuntimeClassInitialize(MediaSource* parent, IMFStreamDescriptor* devDescriptor, DWORD workQueue);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID extendedType, HRESULT status, const PROPVARIANT* v) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // Internal (called by MediaSource under its own lock)
    HRESULT AttachDeviceStream(IMFMediaStream* devStream);
    HRESULT Shutdown();
    DWORD StreamId() const { return streamId_; }
    IMFStreamDescriptor* Descriptor() const { return descriptor_.Get(); }

private:
    class EventCallback;
    void OnDeviceStreamEvent(IMFAsyncResult* result);
    HRESULT ProcessSample(IMFSample* sample);
    void BeginProcessingSession();
    HRESULT CheckShutdownLocked() const;

    FrameProcessor processor_;  // frame path runs on the serial work queue only

    std::mutex mu_;
    bool shutdown_ = false;
    bool listening_ = false;  // a BeginGetEvent on devStream_ is pending
    DWORD streamId_ = 0;
    DWORD workQueue_ = 0;
    Microsoft::WRL::ComPtr<IMFMediaSource> parent_;
    Microsoft::WRL::ComPtr<IMFMediaStream> devStream_;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
    Microsoft::WRL::ComPtr<IMFAsyncCallback> devCallback_;
    unsigned long long framesDelivered_ = 0;
};

// Keeps only NV12 media types from a physical stream descriptor (falling back to YUY2 when a
// camera offers no NV12 at all). Returns MF_E_INVALIDMEDIATYPE when nothing usable remains.
HRESULT FilterProcessableTypes(IMFStreamDescriptor* devDescriptor, std::vector<Microsoft::WRL::ComPtr<IMFMediaType>>& out);

}  // namespace ixc::vcam
