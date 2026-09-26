#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ixc::camera {

struct CameraInfo {
    std::string name;          // friendly name, UTF-8
    std::wstring symbolicLink; // stable device identity used for reopen/reconnect
    bool isSoftwareDevice = false;  // root/software-enumerated (virtual) camera, not a physical bus device
};

// One mode the physical camera can actually deliver (never synthesized).
struct CaptureFormat {
    GUID subtype{};            // MFVideoFormat_* (NV12, YUY2, MJPG, ...)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fpsNumerator = 0;
    std::uint32_t fpsDenominator = 1;
    std::uint32_t nativeIndex = 0;  // index into the source's native media types (stream 0)

    double Fps() const { return fpsDenominator ? static_cast<double>(fpsNumerator) / fpsDenominator : 0.0; }
    bool SameMode(const CaptureFormat& o) const {
        return IsEqualGUID(subtype, o.subtype) && width == o.width && height == o.height &&
               static_cast<std::uint64_t>(fpsNumerator) * o.fpsDenominator ==
                   static_cast<std::uint64_t>(o.fpsNumerator) * fpsDenominator;
    }
};

// "MJPG", "YUY2", "NV12", "RGB32", or the GUID when unknown.
std::string SubtypeName(const GUID& subtype);

// "1920x1080 @ 30 FPS (MJPG)"; fractional rates print as e.g. "29.97".
std::string Describe(const CaptureFormat& f);

// True when the symbolic link denotes a root-enumerated or software device (virtual camera).
bool IsSoftwareDeviceLink(const std::wstring& symbolicLink);

}  // namespace ixc::camera
