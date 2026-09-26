#pragma once

// Applies ExposureGovernor decisions to a real camera through its UVC exposure control
// (IKsControl, KSPROPERTY_CAMERACONTROL_EXPOSURE). Cheap per frame: one timestamp and a sparse
// brightness sample (1 in 64 luma pixels). The camera's automatic exposure is always restored
// in End(), so other apps see the camera unchanged.

#include "camera/exposure_governor.h"

#include <windows.h>
#include <mfidl.h>  // COM base declarations ksproxy.h relies on
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace ixc::camera {

class SmoothMotion {
public:
    // ks may be null (control unavailable): then nothing happens. A camera already on manual
    // exposure is left alone.
    void Begin(IKsControl* ks, bool enabled, double nominalFps);
    // Luma plane of the frame (NV12 Y plane). Returns true when CompensationEv() changed.
    bool OnFrame(const std::uint8_t* y, int stride, int width, int height);
    // Restores automatic exposure if it was changed: S_FALSE = nothing to restore.
    HRESULT End();

    // Needs per-frame input; false once disabled (then OnFrame is unneeded).
    bool Active() const {
        const auto s = governor_.state();
        return s != ExposureGovernor::State::Disabled;  // locked: still watches frame times
    }
    double CompensationEv() const { return governor_.CompensationEv(); }
    ExposureGovernor::State state() const { return governor_.state(); }
    int Reasserts() const { return governor_.Reasserts(); }
    bool NeedsLuma() const { return governor_.NeedsLuma(); }
    int AppliedExposure() const { return governor_.ExposureChanged() ? governor_.RequestedExposure() : 0; }

private:
    HRESULT GetExposure(LONG& value, LONG& flags);
    HRESULT SetExposure(LONG value, bool manual);
    HRESULT RestoreAuto();

    Microsoft::WRL::ComPtr<IKsControl> ks_;
    ExposureGovernor governor_;
    long long lastQpc_ = 0;
    double qpcToMs_ = 0;
    double lastCompensation_ = 0;
    LONG originalValue_ = 0;  // exposure value before we changed anything
    bool governed_ = false;   // camera was on auto exposure and Smooth motion is on
};

}  // namespace ixc::camera
