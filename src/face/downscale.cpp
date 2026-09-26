#include "face/downscale.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace ixc::face {

namespace {
// Mean luma from a sparse grid (every 16th pixel of every 16th row): enough for a gain decision.
double SparseMeanLuma(const processing::Nv12Planes& src) {
    std::uint64_t sum = 0, n = 0;
    for (int y = 0; y < src.height; y += 16) {
        const std::uint8_t* row = src.y + static_cast<std::ptrdiff_t>(y) * src.yStride;
        for (int x = 0; x < src.width; x += 16) {
            sum += row[x];
            ++n;
        }
    }
    return n ? static_cast<double>(sum) / static_cast<double>(n) : 0;
}
}  // namespace

bool DownscaleNv12ToBgr(const processing::Nv12Planes& src, const processing::YuvFormat& fmt, std::uint8_t* dst, int dstW,
                        int dstH, bool normalize, float* appliedGain) {
    if (!src.y || !src.uv || !dst || src.width < 2 || src.height < 2 || dstW <= 0 || dstH <= 0 || dstW > src.width ||
        dstH > src.height || src.yStride < src.width || src.uvStride < src.width) {
        return false;
    }
    // Brightness normalization for the detector only: dark frames (dim rooms, Smooth motion's
    // shorter exposure) are lifted to a mid-grey mean, at most 4x.
    const int black = fmt.fullRange ? 0 : 16;
    const int white = fmt.fullRange ? 255 : 235;
    float gain = 1;
    if (normalize) {
        const double mean = SparseMeanLuma(src) - black;
        if (mean > 1) gain = static_cast<float>(std::clamp((kTargetLuma - black) / mean, 1.0, 4.0));
    }
    if (appliedGain) *appliedGain = gain;
    std::array<std::uint8_t, 256> lut;
    for (int v = 0; v < 256; ++v) {
        lut[static_cast<size_t>(v)] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(black + (v - black) * gain + 0.5f), 0, white));
    }

    for (int oy = 0; oy < dstH; ++oy) {
        // Source block rows [y0, y1); samples at 1/4 and 3/4 of the block.
        const int y0 = static_cast<int>(static_cast<long long>(oy) * src.height / dstH);
        const int y1 = static_cast<int>(static_cast<long long>(oy + 1) * src.height / dstH);
        const int bh = std::max(1, y1 - y0);
        const int ya = std::min(src.height - 1, y0 + bh / 4), yb = std::min(src.height - 1, y0 + (3 * bh) / 4);
        const int yc = std::min(src.height / 2 - 1, (y0 + bh / 2) / 2);
        const std::uint8_t* ra = src.y + static_cast<std::ptrdiff_t>(ya) * src.yStride;
        const std::uint8_t* rb = src.y + static_cast<std::ptrdiff_t>(yb) * src.yStride;
        const std::uint8_t* rc = src.uv + static_cast<std::ptrdiff_t>(yc) * src.uvStride;
        std::uint8_t* out = dst + static_cast<std::ptrdiff_t>(oy) * dstW * 3;
        for (int ox = 0; ox < dstW; ++ox) {
            const int x0 = static_cast<int>(static_cast<long long>(ox) * src.width / dstW);
            const int x1 = static_cast<int>(static_cast<long long>(ox + 1) * src.width / dstW);
            const int bw = std::max(1, x1 - x0);
            const int xa = std::min(src.width - 1, x0 + bw / 4), xb = std::min(src.width - 1, x0 + (3 * bw) / 4);
            const int luma = lut[static_cast<size_t>((ra[xa] + ra[xb] + rb[xa] + rb[xb] + 2) / 4)];
            const int xc = std::min(src.width / 2 - 1, (x0 + bw / 2) / 2) * 2;
            const std::uint32_t bgra = processing::YuvToBgra(luma, rc[xc], rc[xc + 1], fmt);
            out[0] = static_cast<std::uint8_t>(bgra);
            out[1] = static_cast<std::uint8_t>(bgra >> 8);
            out[2] = static_cast<std::uint8_t>(bgra >> 16);
            out += 3;
        }
    }
    return true;
}

}  // namespace ixc::face
