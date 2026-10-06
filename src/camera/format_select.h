#pragma once

// Pure format-negotiation logic (no Media Foundation calls), unit-tested.
//
// Policy: only modes the camera reports are ever chosen. Resolution matters most, then
// frame rate, then pixel format. Uncompressed formats win ties because they avoid a decode,
// but a compressed mode that reaches the requested frame rate beats an uncompressed one
// that can't (e.g. many USB 2.0 webcams deliver 1080p YUY2 at only 5 FPS, but MJPG at 30).

#include "camera/camera_types.h"
#include "profiles/profile.h"

#include <optional>
#include <vector>

namespace ixc::camera {

struct FormatRequest {
    std::uint32_t width = 0;   // 0 = choose automatically
    std::uint32_t height = 0;
    double fps = 0;            // 0 = highest available up to 30 FPS
    // Best for this camera: the largest resolution it delivers smoothly (>= 24 FPS), at the
    // highest frame rate offered for that resolution (e.g. 1080p60 rather than 1080p30). width,
    // height and fps are ignored.
    bool best = false;
};

// Default capture request for a performance tier: Ultra Low / Low → 720p30 (weak PCs), every
// other tier → the best mode the camera has (FormatRequest::best).
FormatRequest RequestForTier(PerformanceTier tier);

// Removes exact duplicates (same subtype, size, rate) keeping the first, and sorts
// by resolution (desc), then frame rate (desc), then subtype preference.
std::vector<CaptureFormat> NormalizeFormats(std::vector<CaptureFormat> formats);

// Index into `formats` of the best match, or nullopt when the list is empty.
std::optional<size_t> SelectFormat(const std::vector<CaptureFormat>& formats, const FormatRequest& request);

// Lower is better: NV12 0, YUY2 1, MJPG 2, other 3.
int SubtypeRank(const GUID& subtype);

}  // namespace ixc::camera
