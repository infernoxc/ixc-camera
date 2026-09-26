#include "virtual_camera/source/media_stream.h"

#include "virtual_camera/source/media_source.h"
#include "virtual_camera/source/trace.h"

#include <ks.h>
#include <ksmedia.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace ixc::vcam {

// IMFAsyncCallback that routes physical-stream events back to the stream on the source's
// serial work queue. It holds a strong reference; MediaStream::Shutdown breaks the cycle.
class MediaStream::EventCallback final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
public:
    EventCallback(MediaStream* owner, DWORD queue) : owner_(owner), queue_(queue) {}
    STDMETHODIMP GetParameters(DWORD* flags, DWORD* queue) override {
        *flags = 0;
        *queue = queue_;
        return S_OK;
    }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        if (owner_) owner_->OnDeviceStreamEvent(result);
        return S_OK;
    }

private:
    ComPtr<MediaStream> owner_;
    DWORD queue_;
};

HRESULT FilterProcessableTypes(IMFStreamDescriptor* devDescriptor, std::vector<ComPtr<IMFMediaType>>& out) try {
    out.clear();
    ComPtr<IMFMediaTypeHandler> handler;
    HRESULT hr = devDescriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    DWORD count = 0;
    hr = handler->GetMediaTypeCount(&count);
    if (FAILED(hr)) return hr;

    std::vector<ComPtr<IMFMediaType>> yuy2;
    for (DWORD i = 0; i < count && i < 1024; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(handler->GetMediaTypeByIndex(i, &t))) continue;
        GUID major{}, sub{};
        if (FAILED(t->GetGUID(MF_MT_MAJOR_TYPE, &major)) || !IsEqualGUID(major, MFMediaType_Video)) continue;
        if (FAILED(t->GetGUID(MF_MT_SUBTYPE, &sub))) continue;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) continue;

        // Copy so later changes by the pipeline never touch the physical camera's type objects.
        ComPtr<IMFMediaType> copy;
        if (FAILED(MFCreateMediaType(&copy)) || FAILED(t->CopyAllItems(copy.Get()))) continue;
        if (IsEqualGUID(sub, MFVideoFormat_NV12)) out.push_back(copy);
        else if (IsEqualGUID(sub, MFVideoFormat_YUY2)) yuy2.push_back(copy);
    }
    if (out.empty()) out = std::move(yuy2);
    return out.empty() ? MF_E_INVALIDMEDIATYPE : S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* parent, IMFStreamDescriptor* devDescriptor, DWORD workQueue) try {
    if (!parent || !devDescriptor) return E_INVALIDARG;
    parent_ = parent;
    workQueue_ = workQueue;

    HRESULT hr = devDescriptor->GetStreamIdentifier(&streamId_);
    if (FAILED(hr)) return hr;

    std::vector<ComPtr<IMFMediaType>> types;
    hr = FilterProcessableTypes(devDescriptor, types);
    if (FAILED(hr)) return hr;

    std::vector<IMFMediaType*> raw;
    raw.reserve(types.size());
    for (auto& t : types) raw.push_back(t.Get());

    // Same stream identifier as the physical stream, so presentation descriptors map 1:1.
    hr = MFCreateStreamDescriptor(streamId_, static_cast<DWORD>(raw.size()), raw.data(), &descriptor_);
    if (FAILED(hr)) return hr;

    // Carry over the physical stream's attributes, then state what Frame Server needs to know.
    devDescriptor->CopyAllItems(descriptor_.Get());
    descriptor_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    descriptor_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId_);
    descriptor_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);  // several apps may open IXC Camera at once
    descriptor_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    ComPtr<IMFMediaTypeHandler> handler;
    hr = descriptor_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(raw.front());
    if (FAILED(hr)) return hr;

    hr = MFCreateEventQueue(&events_);
    if (FAILED(hr)) return hr;
    devCallback_ = Make<EventCallback>(this, workQueue_);
    if (!devCallback_) return E_OUTOFMEMORY;

    IXC_TRACE("StreamInit", TraceLoggingUInt32(streamId_, "streamId"), TraceLoggingUInt32(static_cast<UINT32>(raw.size()), "mediaTypes"));
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

HRESULT MediaStream::CheckShutdownLocked() const { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }

HRESULT MediaStream::AttachDeviceStream(IMFMediaStream* devStream) {
    std::lock_guard lock(mu_);
    if (!devStream) return E_INVALIDARG;
    if (shutdown_) return MF_E_SHUTDOWN;
    // Frame Server keeps the source alive between app sessions and re-announces the same physical
    // stream (MEUpdatedStream) on each Start. Subscribing twice fails with
    // MF_E_MULTIPLE_SUBSCRIBERS, so only subscribe when not already listening to this stream.
    if (devStream_.Get() == devStream && listening_) return S_OK;
    devStream_ = devStream;
    const HRESULT hr = devStream_->BeginGetEvent(devCallback_.Get(), devStream_.Get());
    listening_ = SUCCEEDED(hr);
    return hr;
}

HRESULT MediaStream::Shutdown() {
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return S_OK;
        shutdown_ = true;
        if (events_) events_->Shutdown();
        events_.Reset();
        devStream_.Reset();
        parent_.Reset();
        devCallback_.Reset();  // breaks the callback ↔ stream reference cycle
        IXC_TRACE("StreamShutdown", TraceLoggingUInt32(streamId_, "streamId"), TraceLoggingUInt64(framesDelivered_, "frames"));
    }
    processor_.EndSession();  // outside the lock: waits for a settings reload in progress
    return S_OK;
}

void MediaStream::BeginProcessingSession() {
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> type;
    {
        std::lock_guard lock(mu_);
        if (shutdown_ || !descriptor_) return;
        if (FAILED(descriptor_->GetMediaTypeHandler(&handler)) || FAILED(handler->GetCurrentMediaType(&type))) return;
        UINT32 n = 0, d = 1;
        timing_ = Timing{};
        if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &d)) && n) timing_.frameIntervalMs = 1000.0 * d / n;
    }
    // Camera controls reach the physical camera through our source's IKsControl forwarding.
    ComPtr<IKsControl> ks;
    ComPtr<IMFMediaSource> parent;
    {
        std::lock_guard lock(mu_);
        parent = parent_;
    }
    if (parent) parent.As(&ks);
    processor_.BeginSession(type.Get(), ks.Get());
}

// ---- event generator ------------------------------------------------------------------------------

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;  // GetEvent may block: don't hold the lock
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID extendedType, HRESULT status, const PROPVARIANT* v) {
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->QueueEventParamVar(met, extendedType, status, v);
}

// ---- IMFMediaStream -------------------------------------------------------------------------------

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    *source = nullptr;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return parent_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    *descriptor = nullptr;
    std::lock_guard lock(mu_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    ComPtr<IMFMediaStream> dev;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (!devStream_) return MF_E_INVALIDREQUEST;  // not started yet
        dev = devStream_;
        ++timing_.requests;
        timing_.maxOutstanding = std::max(timing_.maxOutstanding, ++timing_.outstanding);
    }
    return dev->RequestSample(token);
}

void MediaStream::ReportTimingLocked() {
    const Timing& t = timing_;
    if (t.delivered == 0) return;
    const double meanGap = t.gaps ? t.sumGapMs / t.gaps : 0;
    const double jitter = t.gaps ? std::sqrt(std::max(0.0, t.sumGapSqMs / t.gaps - meanGap * meanGap)) : 0;
    IXC_TRACE("StreamTiming", TraceLoggingUInt64(t.requests, "requests"), TraceLoggingUInt64(t.delivered, "delivered"),
              TraceLoggingInt64(t.maxOutstanding, "maxOutstanding"), TraceLoggingFloat64(meanGap, "meanGapMs"),
              TraceLoggingFloat64(jitter, "jitterMs"), TraceLoggingFloat64(t.maxGapMs, "maxGapMs"), TraceLoggingUInt64(t.longGaps, "longGaps"),
              TraceLoggingFloat64(t.sumProcessMs / t.delivered, "avgProcessMs"), TraceLoggingFloat64(t.maxProcessMs, "maxProcessMs"),
              TraceLoggingFloat64(t.frameIntervalMs, "nominalMs"));
    timing_ = Timing{};
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    ComPtr<IMFMediaStream2> dev;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (!devStream_) return state == MF_STREAM_STATE_STOPPED ? S_OK : MF_E_INVALIDREQUEST;
        if (FAILED(devStream_.As(&dev))) return E_NOTIMPL;
    }
    IXC_TRACE("SetStreamState", TraceLoggingUInt32(streamId_, "streamId"), TraceLoggingInt32(state, "state"));
    const HRESULT hr = dev->SetStreamState(state);
    // Frame Server stops a client's stream this way (no MEStreamStopped arrives), so this is
    // where an idle camera must let go of its settings watcher and frame pool.
    if (SUCCEEDED(hr) && state == MF_STREAM_STATE_STOPPED) {
        std::lock_guard lock(mu_);
        ReportTimingLocked();
    }
    if (SUCCEEDED(hr)) {
        if (state == MF_STREAM_STATE_STOPPED) processor_.EndSession();
        else if (state == MF_STREAM_STATE_RUNNING && !processor_.Active()) BeginProcessingSession();
    }
    return hr;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    ComPtr<IMFMediaStream2> dev;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return MF_E_SHUTDOWN;
        if (!devStream_) {
            *state = MF_STREAM_STATE_STOPPED;
            return S_OK;
        }
        if (FAILED(devStream_.As(&dev))) return E_NOTIMPL;
    }
    return dev->GetStreamState(state);
}

// ---- physical stream events -------------------------------------------------------------------------

void MediaStream::OnDeviceStreamEvent(IMFAsyncResult* result) {
    ComPtr<IUnknown> stateUnk;
    ComPtr<IMFMediaStream> dev;
    ComPtr<IMFMediaEvent> event;
    MediaEventType met = MEUnknown;
    HRESULT hr = result->GetState(&stateUnk);
    if (SUCCEEDED(hr)) hr = stateUnk.As(&dev);
    if (SUCCEEDED(hr)) hr = dev->EndGetEvent(result, &event);
    if (SUCCEEDED(hr)) hr = event->GetType(&met);

    bool forward = SUCCEEDED(hr);
    if (SUCCEEDED(hr) && met == MEMediaSample) {
        PROPVARIANT v;
        PropVariantInit(&v);
        hr = event->GetValue(&v);
        ComPtr<IMFSample> sample;
        if (SUCCEEDED(hr)) hr = (v.vt == VT_UNKNOWN && v.punkVal) ? v.punkVal->QueryInterface(IID_PPV_ARGS(&sample)) : MF_E_UNEXPECTED;
        PropVariantClear(&v);
        if (SUCCEEDED(hr)) hr = ProcessSample(sample.Get());
        forward = false;  // ProcessSample queued the (processed) sample itself
    } else if (SUCCEEDED(hr) && met == MEStreamStarted) {
        BeginProcessingSession();  // the app's chosen media type is final now
    } else if (SUCCEEDED(hr) && met == MEStreamStopped) {
        processor_.EndSession();   // idle: stop watching settings, free the frame pool
    }

    // An MEError from the physical stream carries its failure in the event status.
    HRESULT failure = hr;
    if (SUCCEEDED(hr) && met == MEError) {
        failure = E_FAIL;
        event->GetStatus(&failure);
    }

    ComPtr<IMFMediaSource> parentToNotify;
    {
        std::lock_guard lock(mu_);
        if (shutdown_) return;
        const bool current = dev && dev.Get() == devStream_.Get();
        if (current) listening_ = false;  // this callback consumed the pending subscription
        if (!current) return;             // event from a stream we no longer wrap: drop it
        if (FAILED(failure)) {
            parentToNotify = parent_;
            IXC_TRACE_HR("StreamEventError", failure);
        } else {
            if (forward) events_->QueueEvent(event.Get());
            // Keep listening; the physical stream ends the chain when it shuts down.
            listening_ = SUCCEEDED(devStream_->BeginGetEvent(devCallback_.Get(), devStream_.Get()));
        }
    }
    // Report to the app through the source (e.g. camera unplugged → device invalidated).
    if (parentToNotify) parentToNotify->QueueEvent(MEError, GUID_NULL, failure, nullptr);
}

HRESULT MediaStream::ProcessSample(IMFSample* sample) {
    // The single insertion point of the IXC image pipeline. Runs on the source's serial work
    // queue; never blocks and never allocates per frame (the processor uses a bounded pool).
    LARGE_INTEGER arrival, done, freq;
    QueryPerformanceCounter(&arrival);
    const ComPtr<IMFSample> out = processor_.Process(sample);
    QueryPerformanceCounter(&done);
    QueryPerformanceFrequency(&freq);
    std::lock_guard lock(mu_);
    if (shutdown_) return S_OK;
    // Timing of frames arriving from the physical camera and of IXC's processing.
    const double processMs = 1000.0 * static_cast<double>(done.QuadPart - arrival.QuadPart) / static_cast<double>(freq.QuadPart);
    timing_.sumProcessMs += processMs;
    timing_.maxProcessMs = std::max(timing_.maxProcessMs, processMs);
    if (timing_.lastArrivalQpc) {
        const double gap = 1000.0 * static_cast<double>(arrival.QuadPart - timing_.lastArrivalQpc) / static_cast<double>(freq.QuadPart);
        timing_.sumGapMs += gap;
        timing_.sumGapSqMs += gap * gap;
        timing_.maxGapMs = std::max(timing_.maxGapMs, gap);
        ++timing_.gaps;
        if (gap > timing_.frameIntervalMs * 1.5) ++timing_.longGaps;
    }
    timing_.lastArrivalQpc = arrival.QuadPart;
    ++timing_.delivered;
    if (timing_.outstanding > 0) --timing_.outstanding;
    ++framesDelivered_;
    if (framesDelivered_ == 1) IXC_TRACE("FirstFrame", TraceLoggingUInt32(streamId_, "streamId"));
    return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, out.Get());
}

}  // namespace ixc::vcam
