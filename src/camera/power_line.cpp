#include "camera/power_line.h"

#include <ksmedia.h>

#include <iterator>
#include <string>

namespace ixc::camera {

std::string UserRegion() {
    wchar_t geo[16] = {};
    const int n = GetUserDefaultGeoName(geo, static_cast<int>(std::size(geo)));
    std::string out;
    if (n == 3) {  // two letters + terminator (numeric UN M.49 codes are longer)
        for (int i = 0; i < 2; ++i) out.push_back(geo[i] < 128 ? static_cast<char>(geo[i]) : '?');
    }
    return out;
}

HRESULT PowerLineControl::Get(LONG& value) {
    KSPROPERTY_VIDEOPROCAMP_S s{};
    s.Property.Set = PROPSETID_VIDCAP_VIDEOPROCAMP;
    s.Property.Id = KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY;
    s.Property.Flags = KSPROPERTY_TYPE_GET;
    ULONG returned = 0;
    const HRESULT hr = ks_->KsProperty(&s.Property, sizeof(s), &s, sizeof(s), &returned);
    if (SUCCEEDED(hr)) value = s.Value;
    return hr;
}

HRESULT PowerLineControl::Set(LONG value) {
    KSPROPERTY_VIDEOPROCAMP_S s{};
    s.Property.Set = PROPSETID_VIDCAP_VIDEOPROCAMP;
    s.Property.Id = KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY;
    s.Property.Flags = KSPROPERTY_TYPE_SET;
    s.Value = value;
    s.Flags = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
    ULONG returned = 0;
    return ks_->KsProperty(&s.Property, sizeof(s), &s, sizeof(s), &returned);
}

HRESULT PowerLineControl::Apply(IKsControl* ks, AntiFlicker mode) {
    if (ks != ks_.Get()) {
        Restore();
        ks_ = ks;
    }
    applied_ = mode;
    if (!ks_) return S_FALSE;
    if (mode == AntiFlicker::Auto) return Restore();  // leave (or give back) the camera's own setting
    // UVC values: 0 = disabled, 1 = 50 Hz, 2 = 60 Hz.
    const LONG want = mode == AntiFlicker::Hz50 ? 1 : mode == AntiFlicker::Hz60 ? 2 : 0;
    LONG current = 0;
    HRESULT hr = Get(current);
    if (FAILED(hr)) return hr;  // the camera has no such control
    if (current == want) return S_OK;
    hr = Set(want);
    if (SUCCEEDED(hr) && !changed_) {
        original_ = current;
        changed_ = true;
    }
    return hr;
}

HRESULT PowerLineControl::Restore() {
    HRESULT hr = S_FALSE;
    if (changed_ && ks_) hr = Set(original_);
    changed_ = false;
    return hr;
}

}  // namespace ixc::camera
