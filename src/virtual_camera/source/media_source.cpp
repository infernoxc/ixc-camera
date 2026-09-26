#include "virtual_camera/source/media_source.h"

#include "virtual_camera/source/trace.h"

#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfvirtualcamera.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::MakeAndInitialize;

namespace ixc::vcam {

class MediaSource::EventCallback final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
public:
    EventCallback(MediaSource* owner, DWORD queue) : owner_(owner), queue_(queue) {}
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
        *flags = 0;
        *queue = queue_;
        return S_OK;
    }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        if (owner_) owner_->OnDeviceSourceEvent(result);
        return S_OK;
    }

private:
    ComPtr<MediaSource> owner_;
    DWORD queue_;
};

namespace {

// Finds the colour capture stream to expose: VIDEO_CAPTURE preferred, VIDEO_PREVIEW otherwise.
HRESULT FindColorStream(IMFPresentationDescriptor* pd, DWORD& index) {
    DWORD count = 0;
    HRESULT hr = pd->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) return hr;
    bool havePreview = false;
    DWORD preview = 0;
    for (DWORD i = 0; i < count; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd))) continue;
        GUID category = GUID_NULL;
        UINT32 sourceType = MFFrameSourceTypes_Color;  // absent on some cameras: treat as colour
        sd->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
        sd->GetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, &sourceType);
        if (!(sourceType & MFFrameSourceTypes_Color)) continue;
        if (IsEqualGUID(category, PINNAME_VIDEO_CAPTURE) || IsEqualGUID(category, GUID_NULL)) {
            index = i;
            return S_OK;
        }
        if (IsEqualGUID(category, PINNAME_VIDEO_PREVIEW) && !havePreview) {
            havePreview = true;
            preview = i;
        }
    }
    if (havePreview) {
        index = preview;
        return S_OK;
    }
    return MF_E_CAPTURE_SOURCE_NO_VIDEO_STREAM_PRESENT;
}

}  // namespace

HRESULT MediaSource::RuntimeClassInitialize(IMFAttributes* activateAttributes, IMFMediaSource* deviceSource) try {
    if (!deviceSource) return E_INVALIDARG;
    dev_ = deviceSource;

    HRESULT hr = MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &workQueue_);
    if (SUCCEEDED(hr)) hr = MFCreateEventQueue(&events_);
    if (SUCCEEDED(hr)) hr = dev_->CreatePresentationDescriptor(&devPd_);
    if (SUCCEEDED(hr)) hr = BuildSourceAttributes(activateAttributes);
    if (SUCCEEDED(hr)) hr = BuildStreams();
    if (FAILED(hr)) {
        IXC_TRACE_HR("SourceInitFailed", hr);
        return hr;
    }

    devCallback_ = Make<EventCallback>(this, workQueue_);
    if (!devCallback_) return E_OUTOFMEMORY;
    hr = dev_->BeginGetEvent(devCallback_.Get(), dev_.Get());
    IXC_TRACE("SourceInit", TraceLoggingHResult(hr, "hr"));
    return hr;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

MediaSource::~MediaSource() {
    if (workQueue_) MFUnlockWorkQueue(workQueue_);
}

HRESULT MediaSource::BuildSourceAttributes(IMFAttributes* activateAttributes) {
    HRESULT hr = MFCreateAttributes(&attributes_, 8);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaSourceEx> devEx;
    ComPtr<IMFAttributes> devAttributes;
    if (SUCCEEDED(dev_.As(&devEx)) && SUCCEEDED(devEx->GetSourceAttributes(&devAttributes))) {
        devAttributes->CopyAllItems(attributes_.Get());
    }

    // Overlay the virtual camera's own configuration, but never re-publish the handle to the
    // physical camera that Frame Server gave us.
    if (activateAttributes) {
        UINT32 count = 0;
        activateAttributes->GetCount(&count);
        for (UINT32 i = 0; i < count; ++i) {
            GUID key = GUID_NULL;
            PROPVARIANT item;
            PropVariantInit(&item);
            if (SUCCEEDED(activateAttributes->GetItemByIndex(i, &key, &item)) &&
                !IsEqualGUID(key, MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES) &&
                !IsEqualGUID(key, MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES)) {
                attributes_->SetItem(key, item);
            }
            PropVariantClear(&item);
        }
    }

    // Frame Server requires a sensor profile collection containing the Legacy profile.
    ComPtr<IUnknown> existing;
    if (FAILED(attributes_->GetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, IID_PPV_ARGS(&existing)))) {
        ComPtr<IMFSensorProfileCollection> profiles;
        ComPtr<IMFSensorProfile> legacy;
        hr = MFCreateSensorProfileCollection(&profiles);
        if (SUCCEEDED(hr)) hr = MFCreateSensorProfile(KSCAMERAPROFILE_Legacy, 0, nullptr, &legacy);
        // Legacy profile: every exposed resolution, rate and subtype.
        if (SUCCEEDED(hr)) {
            DWORD index = 0;
            DWORD streamId = 0;
            ComPtr<IMFStreamDescriptor> sd;
            BOOL selected = FALSE;
            if (SUCCEEDED(FindColorStream(devPd_.Get(), index)) &&
                SUCCEEDED(devPd_->GetStreamDescriptorByIndex(index, &selected, &sd))) {
                sd->GetStreamIdentifier(&streamId);
            }
            hr = legacy->AddProfileFilter(streamId, L"((RES==;FRT==;SUT==))");
        }
        if (SUCCEEDED(hr)) hr = profiles->AddProfile(legacy.Get());
        if (SUCCEEDED(hr)) hr = attributes_->SetUnknown(MF_DEVICEMFT_SENSORPROFILE_COLLECTION, profiles.Get());
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT MediaSource::BuildStreams() {
    DWORD index = 0;
    HRESULT hr = FindColorStream(devPd_.Get(), index);
    if (FAILED(hr)) return hr;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> devSd;
    hr = devPd_->GetStreamDescriptorByIndex(index, &selected, &devSd);
    if (FAILED(hr)) return hr;

    ComPtr<MediaStream> stream;
    hr = MakeAndInitialize<MediaStream>(&stream, this, devSd.Get(), workQueue_);
    if (FAILED(hr)) return hr;
    streams_.push_back(stream);

    IMFStreamDescriptor* sds[] = {stream->Descriptor()};
    hr = MFCreatePresentationDescriptor(1, sds, &pd_);
    // Like a hardware camera, publish the video stream as selected by default. Apps that rely on
    // the default selection (e.g. the Source Reader) would otherwise start with no stream.
    if (SUCCEEDED(hr)) hr = pd_->SelectStream(0);
    return hr;
}

MediaStream* MediaSource::StreamById(DWORD id) {
    for (auto& s : streams_) {
        if (s->StreamId() == id) return s.Get();
    }
    return nullptr;
}

// ---- event generator ------------------------------------------------------------------------------

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return events_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return events_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID extendedType, HRESULT status, const PROPVARIANT* v) {
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return events_->QueueEventParamVar(met, extendedType, status, v);
}

// ---- IMFMediaSource -------------------------------------------------------------------------------

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
    if (!pd) return E_POINTER;
    *pd = nullptr;
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return pd_->Clone(pd);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return dev_->GetCharacteristics(characteristics);
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPosition) {
    IXC_TRACE("SourceStartCall", TraceLoggingBoolean(pd != nullptr, "hasPd"), TraceLoggingBoolean(startPosition != nullptr, "hasPos"),
              TraceLoggingUInt16(startPosition ? startPosition->vt : 0, "posVt"));
    if (!pd || !startPosition) return E_INVALIDARG;
    if (timeFormat && !IsEqualGUID(*timeFormat, GUID_NULL)) return MF_E_UNSUPPORTED_TIME_FORMAT;

    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = pd->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) return hr;
    IXC_TRACE("SourceStartStreams", TraceLoggingUInt32(count, "count"));

    // Physical streams we don't expose stay deselected.
    DWORD devCount = 0;
    devPd_->GetStreamDescriptorCount(&devCount);
    for (DWORD i = 0; i < devCount; ++i) devPd_->DeselectStream(i);

    bool any = false;
    for (DWORD i = 0; i < count; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(i, &selected, &sd);
        if (FAILED(hr)) return hr;
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);
        IXC_TRACE("SourceStartStream", TraceLoggingUInt32(id, "streamId"), TraceLoggingBoolean(selected != FALSE, "selected"),
                  TraceLoggingBoolean(StreamById(id) != nullptr, "known"));
        if (!StreamById(id)) return E_INVALIDARG;  // not one of ours
        if (!selected) continue;
        any = true;

        // Apply the app's chosen type to the matching physical stream.
        ComPtr<IMFMediaTypeHandler> ours;
        ComPtr<IMFMediaType> type;
        hr = sd->GetMediaTypeHandler(&ours);
        if (SUCCEEDED(hr)) hr = ours->GetCurrentMediaType(&type);
        if (FAILED(hr)) return hr;

        for (DWORD d = 0; d < devCount; ++d) {
            BOOL devSelected = FALSE;
            ComPtr<IMFStreamDescriptor> devSd;
            DWORD devId = 0;
            if (FAILED(devPd_->GetStreamDescriptorByIndex(d, &devSelected, &devSd)) || FAILED(devSd->GetStreamIdentifier(&devId)) ||
                devId != id) {
                continue;
            }
            ComPtr<IMFMediaTypeHandler> devHandler;
            hr = devSd->GetMediaTypeHandler(&devHandler);
            if (SUCCEEDED(hr)) hr = devHandler->SetCurrentMediaType(type.Get());
            if (SUCCEEDED(hr)) hr = devPd_->SelectStream(d);
            if (FAILED(hr)) return hr;

            UINT32 w = 0, h = 0, n = 0, dn = 1;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &dn);
            IXC_TRACE("SourceStart", TraceLoggingUInt32(id, "streamId"), TraceLoggingUInt32(w, "width"), TraceLoggingUInt32(h, "height"),
                      TraceLoggingUInt32(n, "fpsNum"), TraceLoggingUInt32(dn, "fpsDen"));
        }
    }
    if (!any) return E_INVALIDARG;

    hr = dev_->Start(devPd_.Get(), timeFormat, startPosition);
    if (FAILED(hr)) IXC_TRACE_HR("DeviceStartFailed", hr);
    return hr;  // MESourceStarted / MENewStream arrive from the physical source and are relayed
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    IXC_TRACE("SourceStop");
    return dev_->Stop();  // MESourceStopped is relayed from the physical source
}

STDMETHODIMP MediaSource::Shutdown() {
    std::vector<ComPtr<MediaStream>> streams;
    ComPtr<IMFMediaSource> dev;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return S_OK;
        state_ = State::Shutdown;
        streams.swap(streams_);
        dev = std::move(dev_);
        if (events_) events_->Shutdown();
        events_.Reset();
        attributes_.Reset();
        pd_.Reset();
        devPd_.Reset();
        devCallback_.Reset();  // breaks the callback ↔ source reference cycle
    }
    for (auto& s : streams) s->Shutdown();
    if (dev) dev->Shutdown();  // releases the physical camera
    IXC_TRACE("SourceShutdown");
    return S_OK;
}

// ---- IMFMediaSourceEx -----------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    MediaStream* s = StreamById(streamId);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    return s->Descriptor()->QueryInterface(IID_PPV_ARGS(attributes));
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown* manager) {
    // Best effort: lets the physical source hand out GPU surfaces when it can.
    ComPtr<IMFMediaSourceEx> devEx;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&devEx))) return S_OK;
    }
    return devEx->SetD3DManager(manager);
}

// ---- forwarded interfaces ---------------------------------------------------------------------------

STDMETHODIMP MediaSource::GetService(REFGUID service, REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    ComPtr<IMFGetService> gs;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&gs))) return MF_E_UNSUPPORTED_SERVICE;
    }
    return gs->GetService(service, riid, object);
}

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY property, ULONG propertyLength, void* data, ULONG dataLength, ULONG* returned) {
    ComPtr<IKsControl> ks;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&ks))) return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    return ks->KsProperty(property, propertyLength, data, dataLength, returned);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD method, ULONG methodLength, void* data, ULONG dataLength, ULONG* returned) {
    ComPtr<IKsControl> ks;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&ks))) return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    return ks->KsMethod(method, methodLength, data, dataLength, returned);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT event, ULONG eventLength, void* data, ULONG dataLength, ULONG* returned) {
    ComPtr<IKsControl> ks;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&ks))) return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
    }
    return ks->KsEvent(event, eventLength, data, dataLength, returned);
}

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD streamId, IUnknown* allocator) {
    ComPtr<IMFSampleAllocatorControl> ac;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&ac))) return E_NOTIMPL;
    }
    return ac->SetDefaultAllocator(streamId, allocator);
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD streamId, DWORD* inputStreamId, MFSampleAllocatorUsage* usage) {
    if (!inputStreamId || !usage) return E_POINTER;
    ComPtr<IMFSampleAllocatorControl> ac;
    {
        std::lock_guard lock(mu_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        if (FAILED(dev_.As(&ac))) {
            *inputStreamId = streamId;
            *usage = MFSampleAllocatorUsage_UsesCustomAllocator;
            return S_OK;
        }
    }
    return ac->GetAllocatorUsage(streamId, inputStreamId, usage);
}

// ---- physical source events -------------------------------------------------------------------------

void MediaSource::OnDeviceSourceEvent(IMFAsyncResult* result) {
    ComPtr<IUnknown> stateUnk;
    ComPtr<IMFMediaSource> dev;
    ComPtr<IMFMediaEvent> event;
    MediaEventType met = MEUnknown;
    HRESULT hr = result->GetState(&stateUnk);
    if (SUCCEEDED(hr)) hr = stateUnk.As(&dev);
    if (SUCCEEDED(hr)) hr = dev->EndGetEvent(result, &event);
    if (SUCCEEDED(hr)) hr = event->GetType(&met);

    std::lock_guard lock(mu_);
    if (state_ == State::Shutdown) return;

    if (SUCCEEDED(hr)) {
        switch (met) {
            case MENewStream:
            case MEUpdatedStream: {
                // Hand the app our stream in place of the physical one.
                PROPVARIANT v;
                PropVariantInit(&v);
                ComPtr<IMFMediaStream> devStream;
                ComPtr<IMFStreamDescriptor> devSd;
                DWORD id = 0;
                hr = event->GetValue(&v);
                if (SUCCEEDED(hr)) hr = (v.vt == VT_UNKNOWN && v.punkVal) ? v.punkVal->QueryInterface(IID_PPV_ARGS(&devStream)) : E_INVALIDARG;
                PropVariantClear(&v);
                if (SUCCEEDED(hr)) hr = devStream->GetStreamDescriptor(&devSd);
                if (SUCCEEDED(hr)) hr = devSd->GetStreamIdentifier(&id);
                if (SUCCEEDED(hr)) {
                    if (MediaStream* s = StreamById(id)) {
                        hr = s->AttachDeviceStream(devStream.Get());
                        if (SUCCEEDED(hr)) {
                            hr = events_->QueueEventParamUnk(met, GUID_NULL, S_OK, static_cast<IMFMediaStream2*>(s));
                        }
                    }
                }
                break;
            }
            case MESourceStarted:
                state_ = State::Started;
                hr = events_->QueueEvent(event.Get());
                break;
            case MESourceStopped:
                state_ = State::Stopped;
                hr = events_->QueueEvent(event.Get());
                break;
            default:
                hr = events_->QueueEvent(event.Get());  // errors, device-removed, etc.
                break;
        }
    }
    if (FAILED(hr)) {
        IXC_TRACE_HR("SourceEventError", hr);
        events_->QueueEventParamVar(MEError, GUID_NULL, hr, nullptr);
    }
    if (dev_) dev_->BeginGetEvent(devCallback_.Get(), dev_.Get());
}

}  // namespace ixc::vcam
