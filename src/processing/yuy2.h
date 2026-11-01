#pragma once

// YUY2 (packed 4:2:2, bytes Y0 U Y1 V) <-> NV12 (planar Y + interleaved UV, 4:2:0).
//
// Many webcams deliver YUY2 instead of NV12. IXC's effects and pipeline work on NV12, so for those
// cameras the virtual camera converts each processed frame to NV12 and back. Both directions are
// plain CPU loops over reused buffers (no allocation). Width and height must be even.
// YUY2 -> NV12 averages the two chroma rows of each pair; NV12 -> YUY2 repeats a chroma row for
// both luma rows, so luma is exact and chroma loses only its vertical detail.

#include "processing/color.h"
#include "processing/image_pipeline.h"

#include <cstdint>

namespace ixc::processing {

void Yuy2ToNv12(const std::uint8_t* src, int srcPitch, const Nv12Frame& dst);
void Nv12ToYuy2(const Nv12Planes& src, std::uint8_t* dst, int dstPitch);

}  // namespace ixc::processing
