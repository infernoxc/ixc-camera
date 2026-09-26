#include "ixc_test.h"
#include "processing/color.h"

#include <cstdlib>
#include <vector>

using namespace ixc::processing;

namespace {

struct Rgb {
    int r, g, b;
};

Rgb Unpack(std::uint32_t p) { return {static_cast<int>((p >> 16) & 0xFF), static_cast<int>((p >> 8) & 0xFF), static_cast<int>(p & 0xFF)}; }

bool Near(Rgb a, Rgb b, int tol = 2) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

// Builds an NV12 image where each 2x2 block has one chroma pair.
struct Nv12Image {
    int w, h;
    std::vector<std::uint8_t> data;
    Nv12Image(int width, int height) : w(width), h(height), data(static_cast<size_t>(width * height * 3 / 2), 128) {}
    std::uint8_t& Y(int x, int y) { return data[static_cast<size_t>(y * w + x)]; }
    void SetUV(int x, int y, std::uint8_t cb, std::uint8_t cr) {
        const size_t o = static_cast<size_t>(w * h + (y / 2) * w + (x & ~1));
        data[o] = cb;
        data[o + 1] = cr;
    }
    Nv12Planes Planes() const { return {data.data(), data.data() + w * h, w, w, w, h}; }
};

}  // namespace

IXC_TEST(Color_VideoRangeBlackAndWhite) {
    const YuvFormat f{YuvMatrix::Bt709, false};
    IXC_CHECK(Near(Unpack(YuvToBgra(16, 128, 128, f)), {0, 0, 0}, 0));
    IXC_CHECK(Near(Unpack(YuvToBgra(235, 128, 128, f)), {255, 255, 255}, 0));
    IXC_CHECK(Near(Unpack(YuvToBgra(126, 128, 128, f)), {128, 128, 128}, 1));
    IXC_CHECK_EQ(YuvToBgra(16, 128, 128, f) >> 24, 0xFFu);  // opaque alpha
}

IXC_TEST(Color_FullRangeBlackAndWhite) {
    const YuvFormat f{YuvMatrix::Bt601, true};
    IXC_CHECK(Near(Unpack(YuvToBgra(0, 128, 128, f)), {0, 0, 0}, 0));
    IXC_CHECK(Near(Unpack(YuvToBgra(255, 128, 128, f)), {255, 255, 255}, 0));
}

IXC_TEST(Color_PrimariesMatchStandards) {
    // Reference YCbCr values for pure primaries (video range) from ITU-R BT.709 / BT.601.
    const YuvFormat bt709{YuvMatrix::Bt709, false};
    IXC_CHECK(Near(Unpack(YuvToBgra(63, 102, 240, bt709)), {255, 0, 0}, 3));
    IXC_CHECK(Near(Unpack(YuvToBgra(173, 42, 26, bt709)), {0, 255, 0}, 3));
    IXC_CHECK(Near(Unpack(YuvToBgra(32, 240, 118, bt709)), {0, 0, 255}, 3));

    const YuvFormat bt601{YuvMatrix::Bt601, false};
    IXC_CHECK(Near(Unpack(YuvToBgra(81, 90, 240, bt601)), {255, 0, 0}, 3));
    IXC_CHECK(Near(Unpack(YuvToBgra(145, 54, 34, bt601)), {0, 255, 0}, 3));
    IXC_CHECK(Near(Unpack(YuvToBgra(41, 240, 110, bt601)), {0, 0, 255}, 3));
}

IXC_TEST(Color_ClampsOutOfGamutInsteadOfWrapping) {
    const YuvFormat f{YuvMatrix::Bt709, false};
    const Rgb hi = Unpack(YuvToBgra(255, 255, 255, f));
    IXC_CHECK(hi.r == 255 && hi.b == 255);
    const Rgb lo = Unpack(YuvToBgra(0, 0, 0, f));
    IXC_CHECK(lo.r == 0 && lo.b == 0);
}

IXC_TEST(Color_DefaultMatrixBySize) {
    IXC_CHECK(DefaultYuvFormat(480).matrix == YuvMatrix::Bt601);
    IXC_CHECK(DefaultYuvFormat(720).matrix == YuvMatrix::Bt709);
    IXC_CHECK(!DefaultYuvFormat(1080).fullRange);
}

IXC_TEST(Scaler_IdentitySizeReproducesPixels) {
    Nv12Image img(4, 2);
    const std::uint8_t ys[8] = {16, 60, 100, 235, 30, 90, 150, 200};
    for (int i = 0; i < 8; ++i) img.Y(i % 4, i / 4) = ys[i];
    const YuvFormat f{YuvMatrix::Bt709, false};
    std::vector<std::uint32_t> out(8);
    Nv12ToBgraScaler s;
    IXC_REQUIRE(s.Convert(img.Planes(), f, out.data(), 4, 2, 4));
    for (int i = 0; i < 8; ++i) IXC_CHECK_EQ(out[static_cast<size_t>(i)], YuvToBgra(ys[i], 128, 128, f));
}

IXC_TEST(Scaler_UsesMatchingChromaPair) {
    Nv12Image img(4, 2);
    for (int x = 0; x < 4; ++x) for (int y = 0; y < 2; ++y) img.Y(x, y) = 81;
    img.SetUV(0, 0, 128, 128);  // left 2x2 block: grey
    img.SetUV(2, 0, 90, 240);   // right 2x2 block: BT.601 red
    const YuvFormat f{YuvMatrix::Bt601, false};
    std::vector<std::uint32_t> out(8);
    Nv12ToBgraScaler s;
    IXC_REQUIRE(s.Convert(img.Planes(), f, out.data(), 4, 2, 4));
    IXC_CHECK_EQ(out[1], YuvToBgra(81, 128, 128, f));
    IXC_CHECK(Near(Unpack(out[2]), {255, 0, 0}, 3));
    IXC_CHECK(Near(Unpack(out[7]), {255, 0, 0}, 3));
}

IXC_TEST(Scaler_DownscaleAndMirror) {
    Nv12Image img(8, 4);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x) img.Y(x, y) = static_cast<std::uint8_t>(16 + x * 20);
    const YuvFormat f{YuvMatrix::Bt709, true};
    std::vector<std::uint32_t> out(4 * 2, 0);
    Nv12ToBgraScaler s;
    IXC_REQUIRE(s.Convert(img.Planes(), f, out.data(), 4, 2, 4));
    // Destination column x samples the source pixel centre: 1, 3, 5, 7.
    IXC_CHECK_EQ(Unpack(out[0]).g, 36);
    IXC_CHECK_EQ(Unpack(out[3]).g, 156);
    IXC_REQUIRE(s.Convert(img.Planes(), f, out.data(), 4, 2, 4, /*mirror=*/true));
    IXC_CHECK_EQ(Unpack(out[0]).g, 156);
    IXC_CHECK_EQ(Unpack(out[3]).g, 36);
}

IXC_TEST(Scaler_UpscaleAndStridedDestination) {
    Nv12Image img(2, 2);
    img.Y(0, 0) = img.Y(0, 1) = 16;
    img.Y(1, 0) = img.Y(1, 1) = 235;
    const YuvFormat f{YuvMatrix::Bt709, false};
    std::vector<std::uint32_t> out(6 * 3, 0xDEADBEEF);  // stride 6, width 4 → last 2 columns untouched
    Nv12ToBgraScaler s;
    IXC_REQUIRE(s.Convert(img.Planes(), f, out.data(), 4, 3, 6));
    IXC_CHECK(Near(Unpack(out[0]), {0, 0, 0}, 0));
    IXC_CHECK(Near(Unpack(out[3]), {255, 255, 255}, 0));
    IXC_CHECK_EQ(out[4], 0xDEADBEEFu);
    IXC_CHECK_EQ(out[6 * 2 + 5], 0xDEADBEEFu);
}

IXC_TEST(Scaler_RejectsInvalidInput) {
    Nv12Image img(4, 4);
    std::vector<std::uint32_t> out(16);
    Nv12ToBgraScaler s;
    const YuvFormat f;
    Nv12Planes p = img.Planes();
    IXC_CHECK(!s.Convert(p, f, nullptr, 4, 4, 4));
    IXC_CHECK(!s.Convert(p, f, out.data(), 0, 4, 4));
    IXC_CHECK(!s.Convert(p, f, out.data(), 4, 4, 3));  // stride < width
    Nv12Planes bad = p;
    bad.yStride = 2;                                    // stride < width
    IXC_CHECK(!s.Convert(bad, f, out.data(), 4, 4, 4));
    bad = p;
    bad.uv = nullptr;
    IXC_CHECK(!s.Convert(bad, f, out.data(), 4, 4, 4));
}
