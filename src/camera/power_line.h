#pragma once

// Anti-flicker: the camera's power-line frequency control (UVC "Power Line Frequency",
// KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY). Set to the mains frequency of the room's
// lighting, the camera's automatic exposure picks exposure times that are whole flicker periods,
// so lamps don't paint dark bands across the picture. A camera shipped for another region (a
// 60 Hz camera in a 50 Hz country) bands until this is set.
//
// Auto means "the user's region": resolved by the app (the user's Windows region setting) before
// the profile is published. The IXC Camera source runs inside the Windows camera service under a
// service account whose region isn't the user's, so it never guesses: Auto there means "leave the
// camera's setting as it is". The original value is restored at session end.

#include "profiles/profile.h"

#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <mfidl.h>  // COM base declarations ksproxy.h relies on
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>
#endif

namespace ixc::camera {

// Mains frequency of a country (ISO 3166 alpha-2, any case): 50, 60, or 0 when unknown or mixed
// (Japan runs both).
int MainsHzForRegion(std::string_view iso2);
// Auto → Hz50/Hz60 for the given region; Auto stays Auto when the region doesn't say.
AntiFlicker ResolveAntiFlicker(AntiFlicker a, std::string_view iso2);

#ifdef _WIN32
// The current user's region (Windows Settings › Time & language › Region), "" if unavailable.
std::string UserRegion();

class PowerLineControl {
public:
    ~PowerLineControl() { Restore(); }
    // Applies a resolved setting (Auto = leave the camera as it is). ks may be null (no control).
    HRESULT Apply(IKsControl* ks, AntiFlicker mode);
    // Restores the camera's original value if it was changed: S_FALSE = nothing to restore.
    HRESULT Restore();
    AntiFlicker applied() const { return applied_; }

private:
    HRESULT Get(LONG& value);
    HRESULT Set(LONG value);

    Microsoft::WRL::ComPtr<IKsControl> ks_;
    LONG original_ = 0;
    bool changed_ = false;
    AntiFlicker applied_ = AntiFlicker::Auto;
};
#endif

}  // namespace ixc::camera
