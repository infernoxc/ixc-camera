#include "processing/yuy2.h"

namespace ixc::processing {

void Yuy2ToNv12(const std::uint8_t* src, int srcPitch, const Nv12Frame& dst) {
    const int w = dst.width & ~1, h = dst.height & ~1;
    for (int y = 0; y < h; y += 2) {
        const std::uint8_t* r0 = src + static_cast<std::ptrdiff_t>(y) * srcPitch;
        const std::uint8_t* r1 = r0 + srcPitch;
        std::uint8_t* y0 = dst.y + static_cast<std::ptrdiff_t>(y) * dst.yStride;
        std::uint8_t* y1 = y0 + dst.yStride;
        std::uint8_t* uv = dst.uv + static_cast<std::ptrdiff_t>(y / 2) * dst.uvStride;
        for (int x = 0; x < w; x += 2) {
            const int i = x * 2;  // Y0 U Y1 V
            y0[x] = r0[i];
            y0[x + 1] = r0[i + 2];
            y1[x] = r1[i];
            y1[x + 1] = r1[i + 2];
            uv[x] = static_cast<std::uint8_t>((r0[i + 1] + r1[i + 1] + 1) >> 1);
            uv[x + 1] = static_cast<std::uint8_t>((r0[i + 3] + r1[i + 3] + 1) >> 1);
        }
    }
}

void Nv12ToYuy2(const Nv12Planes& src, std::uint8_t* dst, int dstPitch) {
    const int w = src.width & ~1, h = src.height & ~1;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* ly = src.y + static_cast<std::ptrdiff_t>(y) * src.yStride;
        const std::uint8_t* uv = src.uv + static_cast<std::ptrdiff_t>(y / 2) * src.uvStride;
        std::uint8_t* d = dst + static_cast<std::ptrdiff_t>(y) * dstPitch;
        for (int x = 0; x < w; x += 2) {
            const int i = x * 2;
            d[i] = ly[x];
            d[i + 1] = uv[x];
            d[i + 2] = ly[x + 1];
            d[i + 3] = uv[x + 1];
        }
    }
}

}  // namespace ixc::processing
