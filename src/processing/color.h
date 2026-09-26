#pragma once

// CPU colour conversion used by the preview and as the CPU fallback path.
//
// The main conversion samples NV12 (YUV 4:2:0) directly into a BGRA buffer at the *display*
// size, so the cost scales with preview pixels rather than camera pixels, and there is no
// full-resolution RGB intermediate. Integer fixed-point maths; the per-column index table is
// reused across frames (no allocation per frame).

#include <cstdint>
#include <vector>

namespace ixc::processing {

enum class YuvMatrix { Bt601, Bt709 };

struct YuvFormat {
    YuvMatrix matrix = YuvMatrix::Bt709;
    bool fullRange = false;  // false = video range (Y 16-235, C 16-240)
};

// Typical default when a camera doesn't declare its matrix: SD → BT.601, HD → BT.709.
YuvFormat DefaultYuvFormat(std::uint32_t height);

struct Nv12Planes {
    const std::uint8_t* y = nullptr;
    const std::uint8_t* uv = nullptr;  // interleaved Cb,Cr at half resolution
    int yStride = 0;
    int uvStride = 0;
    int width = 0;   // luma width (even)
    int height = 0;  // luma height (even)
};

// Converts one YUV triplet to 0xAARRGGBB-in-memory B,G,R,A bytes (packed as uint32 little-endian).
std::uint32_t YuvToBgra(int y, int cb, int cr, const YuvFormat& fmt);

class Nv12ToBgraScaler {
public:
    // dst receives dstW*dstH pixels, row pitch dstStridePixels. Nearest-neighbour sampling.
    // Returns false when inputs are inconsistent (nothing written).
    bool Convert(const Nv12Planes& src, const YuvFormat& fmt, std::uint32_t* dst, int dstW, int dstH, int dstStridePixels,
                 bool mirror = false);

private:
    std::vector<int> xmap_;
    int mapSrcW_ = 0, mapDstW_ = 0;
    bool mapMirror_ = false;
};

}  // namespace ixc::processing
