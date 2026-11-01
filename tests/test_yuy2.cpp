#include "ixc_test.h"
#include "processing/yuy2.h"

#include <cstdint>
#include <vector>

using namespace ixc::processing;

// Webcams that deliver YUY2 are processed through NV12 and converted back: luma must survive
// exactly, chroma must be right (rows of equal chroma stay equal), padded pitches must work.
IXC_TEST(Yuy2_RoundTripKeepsLumaExactlyAndChromaOfFlatAreas) {
    constexpr int w = 16, h = 8, pitch = 2 * w + 8;  // padded rows
    std::vector<std::uint8_t> yuy2(static_cast<size_t>(pitch) * h, 0xEE);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; x += 2) {
            std::uint8_t* p = yuy2.data() + static_cast<size_t>(y) * pitch + x * 2;
            p[0] = static_cast<std::uint8_t>(16 + x * 7 + y * 3);  // Y0
            p[1] = static_cast<std::uint8_t>(100 + x);             // U (same on every row)
            p[2] = static_cast<std::uint8_t>(17 + x * 7 + y * 3);  // Y1
            p[3] = static_cast<std::uint8_t>(150 + x);             // V
        }
    std::vector<std::uint8_t> y(static_cast<size_t>(w) * h), uv(static_cast<size_t>(w) * h / 2);
    const Nv12Frame frame{y.data(), uv.data(), w, w, w, h};
    Yuy2ToNv12(yuy2.data(), pitch, frame);
    for (int r = 0; r < h; ++r)
        for (int x = 0; x < w; ++x) IXC_CHECK_EQ(static_cast<int>(y[static_cast<size_t>(r) * w + x]), static_cast<int>(yuy2[static_cast<size_t>(r) * pitch + static_cast<size_t>(x) * 2]));

    std::vector<std::uint8_t> back(static_cast<size_t>(pitch) * h, 0);
    const Nv12Planes planes{y.data(), uv.data(), w, w, w, h};
    Nv12ToYuy2(planes, back.data(), pitch);
    for (int r = 0; r < h; ++r)
        for (int x = 0; x < w * 2; ++x) IXC_CHECK_EQ(static_cast<int>(back[static_cast<size_t>(r) * pitch + x]), static_cast<int>(yuy2[static_cast<size_t>(r) * pitch + x]));
    // The padding after each row is never touched.
    IXC_CHECK_EQ(static_cast<int>(back[2 * w]), 0);
}

IXC_TEST(Yuy2_ChromaOfTwoRowsIsAveraged) {
    constexpr int w = 4, h = 2;
    std::vector<std::uint8_t> yuy2(static_cast<size_t>(2 * w) * h);
    for (int r = 0; r < h; ++r)
        for (int x = 0; x < w; x += 2) {
            std::uint8_t* p = yuy2.data() + static_cast<size_t>(r) * 2 * w + x * 2;
            p[0] = p[2] = 50;
            p[1] = static_cast<std::uint8_t>(r == 0 ? 100 : 111);  // U
            p[3] = static_cast<std::uint8_t>(r == 0 ? 200 : 201);  // V
        }
    std::vector<std::uint8_t> y(w * h), uv(w * h / 2);
    Yuy2ToNv12(yuy2.data(), 2 * w, Nv12Frame{y.data(), uv.data(), w, w, w, h});
    IXC_CHECK_EQ(static_cast<int>(uv[0]), 106);  // (100 + 111 + 1) / 2
    IXC_CHECK_EQ(static_cast<int>(uv[1]), 201);  // (200 + 201 + 1) / 2
}
