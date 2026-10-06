#pragma once

// CPU NV12 image pipeline: colour/tone adjustments, sharpening, crop, digital zoom, mirror.
//
// Design for weak machines:
//   * Settings are compiled once (CompileParams) into 256-entry lookup tables and geometry;
//     per pixel the colour work is a single table lookup.
//   * Every stage is skipped when its settings are neutral; fully neutral settings mean the
//     caller should pass the frame through untouched (PipelineParams::identity).
//   * The processor owns fixed scratch memory, reallocated only when the frame size changes.
//     Nothing is allocated per frame.
//   * Output size always equals input size: crop and zoom select a source rectangle (with the
//     output's aspect ratio) and rescale it. Zoom is labelled digital; no detail is invented.
//
// This is also the reference implementation the Phase 6 GPU path must match.

#include "processing/color.h"
#include "profiles/profile.h"

#include <array>
#include <cstdint>
#include <vector>

namespace ixc::processing {

struct PipelineParams {
    bool identity = true;         // nothing to do: pass the frame through
    bool lutIdentity = true;      // Y/U/V tables are all identity
    std::array<std::uint8_t, 256> yLut{};
    std::array<std::uint8_t, 256> uLut{};
    std::array<std::uint8_t, 256> vLut{};
    int sharpenAmount = 0;        // 0..384, fixed point /256 (0 = off)
    int sharpenThreshold = 2;     // ignore detail below this (compression noise)
    int sharpenHaloLimit = 6;     // max overshoot beyond the local min/max (prevents halos)
    bool mirror = false;
    // Source rectangle in normalized coordinates (after crop, zoom and aspect fitting).
    double srcX = 0, srcY = 0, srcW = 1, srcH = 1;
    bool geometryIdentity = true;
    bool gpuAllowed = true;       // profile permits GPU use where it's measured to help
};

// The profile as actually applied while Smooth motion brightens the picture by `ev` stops in
// software: the gain adds to the exposure, and temporal denoise is raised to match (the gain
// amplifies noise too). Shared by the app preview and the IXC Camera source.
Profile WithSmoothMotionGain(const Profile& profile, double ev);

// Compiles profile settings for frames of the given size and YUV range.
PipelineParams CompileParams(const Profile& profile, std::uint32_t width, std::uint32_t height, bool fullRange);

struct Nv12Frame {
    std::uint8_t* y = nullptr;
    std::uint8_t* uv = nullptr;
    int yStride = 0;
    int uvStride = 0;
    int width = 0;   // even
    int height = 0;  // even
};

// Bilinear sampling tables (16.16 fixed point, output pixel → source coordinate) for the
// scale pass. Shared by the CPU and GPU paths so both sample exactly the same positions.
struct GeometryTables {
    std::vector<std::int32_t> yX, yY, uvX, uvY;
};
void BuildGeometryTables(int width, int height, const PipelineParams& p, GeometryTables& t);

namespace detail {
// One luma row of the unsharp mask. a/c/b are the ORIGINAL rows above/at/below; d receives
// the result for x in [1, w-2] (borders untouched). Exposed so tests can prove the SSE2 path
// and the scalar reference produce identical output.
void SharpenRowScalar(const std::uint8_t* a, const std::uint8_t* c, const std::uint8_t* b, std::uint8_t* d, int w, int amount,
                      int threshold, int halo);
void SharpenRowSse2(const std::uint8_t* a, const std::uint8_t* c, const std::uint8_t* b, std::uint8_t* d, int w, int amount,
                    int threshold, int halo);
}  // namespace detail

class Nv12Processor {
public:
    // Processes src into dst (same width/height; distinct buffers). Returns false when the
    // inputs are inconsistent (dst untouched).
    bool Process(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& params);

    // Bytes of scratch memory currently held (for diagnostics / tests).
    size_t ScratchBytes() const;

private:
    void PrepareGeometry(int width, int height, const PipelineParams& p);
    void ColorPass(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& p);
    void ScalePass(const Nv12Planes& src, const Nv12Frame& dst, const PipelineParams& p);
    void SharpenPass(const Nv12Frame& dst, const PipelineParams& p);

    // Geometry tables, rebuilt only when size or rectangle changes.
    GeometryTables geo_;
    int geoW_ = 0, geoH_ = 0;
    double geoKey_[5] = {-1, -1, -1, -1, -1};
    std::vector<std::uint8_t> rows_;     // 3 luma rows for the in-place sharpen
    std::vector<std::uint16_t> blend_;   // one vertically blended luma row for scaling
};

}  // namespace ixc::processing
