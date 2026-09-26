#pragma once

// NV12 frame → small BGR image for the face detector.
//
// Runs on the frame thread, so it must be much cheaper than a full-frame pass: each output pixel
// averages a 2x2 grid of luma samples inside its source block (not the whole block) and takes one
// chroma sample. At 1080p → 240x135 that reads ~130 K luma bytes instead of 2 M (measured 0.5-0.8 ms,
// only on the few frames per second that feed a detection).
// `ixc_probe --bench-face` compares detections on this input with a full area average on live frames.

#include "processing/color.h"

#include <cstdint>

namespace ixc::face {

// Mean luma the detector input is normalized towards (when normalize is on).
inline constexpr double kTargetLuma = 110;

// dst: dstW*dstH*3 bytes, B,G,R, row pitch dstW*3. normalize: brighten dark frames (gain 1..4,
// detector input only). Returns false on inconsistent inputs.
bool DownscaleNv12ToBgr(const processing::Nv12Planes& src, const processing::YuvFormat& fmt, std::uint8_t* dst, int dstW,
                        int dstH, bool normalize = true, float* appliedGain = nullptr);

}  // namespace ixc::face
