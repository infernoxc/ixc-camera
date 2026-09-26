#include "virtual_camera/source/activate.h"

#include "virtual_camera/source/media_source.h"
#include "virtual_camera/source/trace.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfvirtualcamera.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::MakeAndInitialize;

namespace ixc::vcam {

HRESULT Activate::RuntimeClassInitialize() {
    HRESULT hr = MFCreateAttributes(&attrs_, 4);
    // Ask Frame Server to hand us the physical camera it already manages (Windows 11 22621+).
    // That keeps the physical device shared through Frame Server instead of opened a second time.
    if (SUCCEEDED(hr)) hr = attrs_->SetUINT32(MF_VIRTUALCAMERA_PROVIDE_ASSOCIATED_CAMERA_SOURCES, 1);
    return hr;
}

HRESULT Activate::GetPhysicalSource(ComPtr<IMFMediaSource>& source) {
    ComPtr<IMFCollection> associated;
    if (SUCCEEDED(attrs_->GetUnknown(MF_VIRTUALCAMERA_ASSOCIATED_CAMERA_SOURCES, IID_PPV_ARGS(&associated)))) {
        DWORD count = 0;
        HRESULT hr = associated->GetElementCount(&count);
        if (FAILED(hr)) return hr;
        if (count != 1) return MF_E_UNEXPECTED;  // IXC Camera wraps exactly one physical camera
        ComPtr<IUnknown> element;
        ComPtr<IMFActivate> activate;
        hr = associated->GetElement(0, &element);
        if (SUCCEEDED(hr)) hr = element.As(&activate);
        if (SUCCEEDED(hr)) hr = activate->ActivateObject(IID_PPV_ARGS(&source));
        IXC_TRACE("PhysicalSourceFromFrameServer", TraceLoggingHResult(hr, "hr"));
        return hr;
    }

    // Fallback for builds without associated-source hand-over: open by symbolic link.
    WCHAR* link = nullptr;
    UINT32 len = 0;
    HRESULT hr = attrs_->GetAllocatedString(kAttrPhysicalLink, &link, &len);
    if (FAILED(hr) || !link) return MF_E_NOT_FOUND;
    ComPtr<IMFAttributes> devAttrs;
    hr = MFCreateAttributes(&devAttrs, 2);
    if (SUCCEEDED(hr)) hr = devAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = devAttrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link);
    CoTaskMemFree(link);
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(devAttrs.Get(), &source);
    IXC_TRACE("PhysicalSourceFromLink", TraceLoggingHResult(hr, "hr"));
    return hr;
}

STDMETHODIMP Activate::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (source_) return source_->QueryInterface(riid, ppv);

    ComPtr<IMFMediaSource> physical;
    HRESULT hr = GetPhysicalSource(physical);
    if (FAILED(hr)) {
        IXC_TRACE_HR("ActivateNoPhysicalCamera", hr);
        return hr;
    }

    ComPtr<MediaSource> source;
    hr = MakeAndInitialize<MediaSource>(&source, attrs_.Get(), physical.Get());
    if (FAILED(hr)) {
        physical->Shutdown();
        IXC_TRACE_HR("ActivateFailed", hr);
        return hr;
    }
    source_ = source;
    return source_->QueryInterface(riid, ppv);
}

STDMETHODIMP Activate::ShutdownObject() {
    if (source_) {
        source_->Shutdown();
        source_.Reset();
    }
    return S_OK;
}

STDMETHODIMP Activate::DetachObject() {
    source_.Reset();
    return S_OK;
}

}  // namespace ixc::vcam
