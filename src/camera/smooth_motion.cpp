#include "camera/smooth_motion.h"

#include <ksmedia.h>

#include <cmath>
#include <mutex>

namespace ixc::camera {

namespace {
// Last decision that worked, for this process (the Frame Server keeps the source loaded between
// sessions). Memory only: lighting changes are caught because a remembered decision is re-verified.
std::mutex g_hintMu;
ExposureGovernor::Hint g_hint;
}  // namespace

void SmoothMotion::Begin(IKsControl* ks, bool enabled, double nominalFps) {
    End();
    ks_ = ks;
    // Only a camera on automatic exposure is governed: a manual exposure is the user's choice.
    // The original value is kept because some drivers reject an out-of-range value even together
    // with the AUTO flag (the Lenovo FHD Webcam rejects 0), so it's what we restore with.
    bool autoExposure = false, sharedFixed = false;
    if (enabled && ks_) {
        LONG flags = 0;
        if (SUCCEEDED(GetExposure(originalValue_, flags))) {
            autoExposure = (flags & KSPROPERTY_CAMERACONTROL_FLAGS_AUTO) != 0;
            // Exactly the value Smooth motion uses: almost certainly set by IXC in another process
            // sharing this camera (app preview + IXC Camera). Keep it, and watch it like our own.
            sharedFixed = !autoExposure && originalValue_ == ExposureGovernor::ExposureForFps(nominalFps);
        }
    }
    ExposureGovernor::Hint hint;
    {
        std::lock_guard lock(g_hintMu);
        hint = g_hint;
    }
    if (sharedFixed) {
        const bool known = hint.valid && hint.exposure == originalValue_ && std::abs(hint.fps - nominalFps) < 0.5;
        hint.valid = true;
        hint.fps = nominalFps;
        hint.exposure = originalValue_;
        if (!known) hint.compensationEv = 0;  // the other process measured it; unknown here
    }
    governed_ = autoExposure || sharedFixed;
    governor_.Reset(governed_, nominalFps, hint);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcToMs_ = 1000.0 / static_cast<double>(f.QuadPart);
    lastQpc_ = 0;
    lastCompensation_ = 0;
}

HRESULT SmoothMotion::GetExposure(LONG& value, LONG& flags) {
    if (!ks_) return E_POINTER;
    KSPROPERTY_CAMERACONTROL_S s{};
    s.Property.Set = PROPSETID_VIDCAP_CAMERACONTROL;
    s.Property.Id = KSPROPERTY_CAMERACONTROL_EXPOSURE;
    s.Property.Flags = KSPROPERTY_TYPE_GET;
    ULONG returned = 0;
    const HRESULT hr = ks_->KsProperty(&s.Property, sizeof(s), &s, sizeof(s), &returned);
    if (SUCCEEDED(hr)) {
        value = s.Value;
        flags = s.Flags;
    }
    return hr;
}

HRESULT SmoothMotion::SetExposure(LONG value, bool manual) {
    if (!ks_) return E_POINTER;
    KSPROPERTY_CAMERACONTROL_S s{};
    s.Property.Set = PROPSETID_VIDCAP_CAMERACONTROL;
    s.Property.Id = KSPROPERTY_CAMERACONTROL_EXPOSURE;
    s.Property.Flags = KSPROPERTY_TYPE_SET;
    s.Value = value;
    s.Flags = manual ? KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL : KSPROPERTY_CAMERACONTROL_FLAGS_AUTO;
    ULONG returned = 0;
    return ks_->KsProperty(&s.Property, sizeof(s), &s, sizeof(s), &returned);
}

HRESULT SmoothMotion::RestoreAuto() {
    HRESULT hr = SetExposure(originalValue_, false);
    if (FAILED(hr)) {
        LONG value = 0, flags = 0;  // fall back to whatever the camera holds now
        if (SUCCEEDED(GetExposure(value, flags))) hr = SetExposure(value, false);
    }
    return hr;
}

bool SmoothMotion::OnFrame(const std::uint8_t* y, int stride, int width, int height) {
    if (!Active()) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double interval = lastQpc_ ? static_cast<double>(now.QuadPart - lastQpc_) * qpcToMs_ : 0;
    lastQpc_ = now.QuadPart;

    // Sparse brightness sample: every 8th pixel of every 8th row.
    double luma = 0;
    if (governor_.NeedsLuma() && y && width > 0 && height > 0) {
        std::uint64_t sum = 0, n = 0;
        for (int r = 0; r < height; r += 8) {
            const std::uint8_t* row = y + static_cast<std::ptrdiff_t>(r) * stride;
            for (int c = 0; c < width; c += 8) { sum += row[c]; ++n; }
        }
        luma = n ? static_cast<double>(sum) / static_cast<double>(n) : 0;
    }

    switch (governor_.OnFrame(interval, luma)) {
        case ExposureGovernor::Action::SetManualExposure:
            governor_.OnExposureApplied(SUCCEEDED(SetExposure(governor_.RequestedExposure(), true)));
            lastQpc_ = 0;  // the switch itself may cost a frame: don't count that interval
            break;
        case ExposureGovernor::Action::RestoreAutoExposure:
            RestoreAuto();
            break;
        case ExposureGovernor::Action::None:
            break;
    }
    const bool changed = governor_.CompensationEv() != lastCompensation_;
    lastCompensation_ = governor_.CompensationEv();
    return changed;
}

HRESULT SmoothMotion::End() {
    HRESULT hr = S_FALSE;  // nothing to restore
    if (ks_) {
        const auto hint = governor_.CurrentHint();
        const auto st = governor_.state();
        {
            std::lock_guard lock(g_hintMu);
            if (hint.valid) g_hint = hint;
            else if (st == ExposureGovernor::State::Disabled && governed_) g_hint = {};  // fixed exposure didn't help
        }
        if (governor_.ExposureChanged()) hr = RestoreAuto();  // give the camera its auto exposure back
    }
    governor_.Reset(false, 0);
    governed_ = false;
    ks_.Reset();
    return hr;
}

}  // namespace ixc::camera
