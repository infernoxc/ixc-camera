#include "camera/device_enum.h"

#include "common/strings.h"

#include <mfapi.h>

#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace ixc::camera {

MediaFoundationScope::MediaFoundationScope() : hr_(MFStartup(MF_VERSION, MFSTARTUP_LITE)) {}

MediaFoundationScope::~MediaFoundationScope() {
    if (SUCCEEDED(hr_)) MFShutdown();
}

HRESULT EnumerateCameras(std::vector<CameraInfo>& out) {
    out.clear();
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (FAILED(hr)) return hr;
    hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return hr;

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attr.Get(), &devices, &count);
    if (FAILED(hr)) return hr;

    for (UINT32 i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        WCHAR* link = nullptr;
        UINT32 len = 0;
        CameraInfo info;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
            info.name = WideToUtf8(name);
        }
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
            info.symbolicLink = link;
        }
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devices[i]->Release();
        if (info.symbolicLink.empty()) continue;
        info.isSoftwareDevice = IsSoftwareDeviceLink(info.symbolicLink);
        if (info.name.empty()) info.name = "Unnamed camera";
        out.push_back(std::move(info));
    }
    CoTaskMemFree(devices);
    return S_OK;
}

HRESULT CreateCameraSource(const std::wstring& symbolicLink, ComPtr<IMFMediaSource>& source) {
    source.Reset();
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolicLink.c_str());
    if (SUCCEEDED(hr)) hr = MFCreateDeviceSource(attr.Get(), &source);
    return hr;
}

bool FormatFromMediaType(IMFMediaType* type, CaptureFormat& f) {
    GUID major{};
    if (FAILED(type->GetMajorType(&major)) || !IsEqualGUID(major, MFMediaType_Video)) return false;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &f.subtype))) return false;
    if (FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &f.width, &f.height))) return false;
    if (FAILED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &f.fpsNumerator, &f.fpsDenominator))) {
        f.fpsNumerator = 0;
        f.fpsDenominator = 1;
    }
    return true;
}

HRESULT EnumerateFormats(IMFMediaSource* source, std::vector<CaptureFormat>& out) {
    out.clear();
    ComPtr<IMFPresentationDescriptor> pd;
    HRESULT hr = source->CreatePresentationDescriptor(&pd);
    if (FAILED(hr)) return hr;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = sd->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    DWORD count = 0;
    hr = handler->GetMediaTypeCount(&count);
    if (FAILED(hr)) return hr;

    for (DWORD i = 0; i < count && i < 1024; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(handler->GetMediaTypeByIndex(i, &type))) continue;
        CaptureFormat f;
        if (!FormatFromMediaType(type.Get(), f)) continue;
        f.nativeIndex = i;
        out.push_back(f);
    }
    return S_OK;
}

HRESULT EnumerateFormats(const std::wstring& symbolicLink, std::vector<CaptureFormat>& out) {
    ComPtr<IMFMediaSource> source;
    HRESULT hr = CreateCameraSource(symbolicLink, source);
    if (FAILED(hr)) return hr;
    hr = EnumerateFormats(source.Get(), out);
    source->Shutdown();
    return hr;
}

namespace {
std::wstring DevicePart(const std::wstring& link) {
    const size_t cut = link.rfind(L"#{");
    std::wstring s = cut == std::wstring::npos ? link : link.substr(0, cut);
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}
}  // namespace

bool SameDevice(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && DevicePart(a) == DevicePart(b);
}

}  // namespace ixc::camera
