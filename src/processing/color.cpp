#include "processing/color.h"

#include <algorithm>

namespace ixc::processing {

namespace {

// Coefficients in 16.16 fixed point, with range expansion folded in.
struct Coeffs {
    int yMul, yOff;         // Y' = (Y - yOff) * yMul
    int crR, cbG, crG, cbB; // chroma contributions (already scaled for range)
};

Coeffs MakeCoeffs(const YuvFormat& f) {
    // Kr/Kb per ITU-R BT.601 / BT.709.
    const double kr = f.matrix == YuvMatrix::Bt709 ? 0.2126 : 0.299;
    const double kb = f.matrix == YuvMatrix::Bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const double ys = f.fullRange ? 1.0 : 255.0 / 219.0;
    const double cs = f.fullRange ? 1.0 : 255.0 / 224.0;
    const double s = 65536.0;
    Coeffs c;
    c.yMul = static_cast<int>(ys * s + 0.5);
    c.yOff = f.fullRange ? 0 : 16;
    c.crR = static_cast<int>(2.0 * (1.0 - kr) * cs * s + 0.5);
    c.cbB = static_cast<int>(2.0 * (1.0 - kb) * cs * s + 0.5);
    c.cbG = static_cast<int>(2.0 * (1.0 - kb) * kb / kg * cs * s + 0.5);
    c.crG = static_cast<int>(2.0 * (1.0 - kr) * kr / kg * cs * s + 0.5);
    return c;
}

inline std::uint32_t Pack(int yv, int cb, int cr, const Coeffs& c) {
    const int y = (yv - c.yOff) * c.yMul + 32768;  // + rounding
    const int u = cb - 128, v = cr - 128;
    const int r = (y + c.crR * v) >> 16;
    const int g = (y - c.cbG * u - c.crG * v) >> 16;
    const int b = (y + c.cbB * u) >> 16;
    auto clamp = [](int x) { return static_cast<std::uint32_t>(x < 0 ? 0 : (x > 255 ? 255 : x)); };
    return 0xFF000000u | clamp(r) << 16 | clamp(g) << 8 | clamp(b);
}

}  // namespace

YuvFormat DefaultYuvFormat(std::uint32_t height) {
    return {height >= 720 ? YuvMatrix::Bt709 : YuvMatrix::Bt601, false};
}

std::uint32_t YuvToBgra(int y, int cb, int cr, const YuvFormat& fmt) { return Pack(y, cb, cr, MakeCoeffs(fmt)); }

bool Nv12ToBgraScaler::Convert(const Nv12Planes& src, const YuvFormat& fmt, std::uint32_t* dst, int dstW, int dstH,
                               int dstStride, bool mirror) {
    if (!src.y || !src.uv || !dst || src.width < 2 || src.height < 2 || dstW <= 0 || dstH <= 0 || dstStride < dstW ||
        src.yStride < src.width || src.uvStride < src.width) {
        return false;
    }
    if (mapSrcW_ != src.width || mapDstW_ != dstW || mapMirror_ != mirror) {
        xmap_.resize(static_cast<size_t>(dstW));
        for (int x = 0; x < dstW; ++x) {
            int sx = static_cast<int>((static_cast<long long>(x) * 2 + 1) * src.width / (2LL * dstW));  // pixel centres
            sx = std::clamp(sx, 0, src.width - 1);
            xmap_[static_cast<size_t>(mirror ? dstW - 1 - x : x)] = sx;
        }
        mapSrcW_ = src.width;
        mapDstW_ = dstW;
        mapMirror_ = mirror;
    }

    const Coeffs c = MakeCoeffs(fmt);
    const int* xm = xmap_.data();
    for (int y = 0; y < dstH; ++y) {
        int sy = static_cast<int>((static_cast<long long>(y) * 2 + 1) * src.height / (2LL * dstH));
        sy = std::clamp(sy, 0, src.height - 1);
        const std::uint8_t* yrow = src.y + static_cast<std::ptrdiff_t>(sy) * src.yStride;
        const std::uint8_t* uvrow = src.uv + static_cast<std::ptrdiff_t>(sy / 2) * src.uvStride;
        std::uint32_t* out = dst + static_cast<std::ptrdiff_t>(y) * dstStride;
        for (int x = 0; x < dstW; ++x) {
            const int sx = xm[x];
            const int ci = sx & ~1;
            out[x] = Pack(yrow[sx], uvrow[ci], uvrow[ci + 1], c);
        }
    }
    return true;
}

}  // namespace ixc::processing
