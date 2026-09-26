#pragma once

// The IXC Camera media source, hosted by the Windows Frame Server service.
//
// It wraps one physical camera source (normally handed over by Frame Server through
// MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES, so Frame Server keeps managing and sharing the
// physical device). It exposes the physical camera's colour capture stream, restricted to
// NV12 types, and forwards camera controls (IKsControl) so Windows camera settings still
// reach the real hardware.
//
// The physical camera streams only between Start() and Stop(), i.e. while an app is using
// IXC Camera.

#include "virtual_camera/source/media_stream.h"

#include <ks.h>
#include <ksproxy.h>
#include <mfidl.h>
#include <wrl/implements.h>

#include <mutex>
#include <vector>

namespace ixc::vcam {

class MediaSource final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService, IKsControl,
          IMFSampleAllocatorControl> {
public:
    HRESULT RuntimeClassInitialize(IMFAttributes* activateAttributes, IMFMediaSource* deviceSource);

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID extendedType, HRESULT status, const PROPVARIANT* v) override;

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, void** object) override;

    // IKsControl (camera controls go straight to the physical camera)
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, void* data, ULONG dataLength, ULONG* returned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, void* data, ULONG dataLength, ULONG* returned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, void* data, ULONG dataLength, ULONG* returned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD streamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) override;

    ~MediaSource() override;

private:
    enum class State { Stopped, Started, Shutdown };
    class EventCallback;

    HRESULT BuildSourceAttributes(IMFAttributes* activateAttributes);
    HRESULT BuildStreams();
    void OnDeviceSourceEvent(IMFAsyncResult* result);
    MediaStream* StreamById(DWORD id);  // requires mu_

    std::mutex mu_;
    State state_ = State::Stopped;
    Microsoft::WRL::ComPtr<IMFMediaSource> dev_;
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> devPd_;
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> pd_;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
    Microsoft::WRL::ComPtr<IMFAsyncCallback> devCallback_;
    std::vector<Microsoft::WRL::ComPtr<MediaStream>> streams_;
    DWORD workQueue_ = 0;
};

}  // namespace ixc::vcam
