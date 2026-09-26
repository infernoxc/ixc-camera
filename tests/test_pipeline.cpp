#include "ixc_test.h"
#include "processing/image_pipeline.h"
#include "profiles/profile.h"

#include <cmath>
#include <cstdlib>
#include <vector>

using namespace ixc;
using namespace ixc::processing;

namespace {

// NV12 test image with padded strides so stride handling is exercised.
struct Image {
    int w, h, stride;
    std::vector<std::uint8_t> y, uv;
    Image(int width, int height, int pad = 16)
        : w(width), h(height), stride(width + pad), y(static_cast<size_t>(stride * height), 0), uv(static_cast<size_t>(stride * height / 2), 128) {}
    std::uint8_t& Y(int x, int yy) { return y[static_cast<size_t>(yy * stride + x)]; }
    std::uint8_t& U(int cx, int cy) { return uv[static_cast<size_t>(cy * stride + 2 * cx)]; }
    std::uint8_t& V(int cx, int cy) { return uv[static_cast<size_t>(cy * stride + 2 * cx + 1)]; }
    Nv12Planes In() const { return {y.data(), uv.data(), stride, stride, w, h}; }
    Nv12Frame Out() { return {y.data(), uv.data(), stride, stride, w, h}; }
};

Profile Neutral() {
    Profile p;
    p.image.sharpness = 0;  // the default profile sharpens subtly; tests start from true neutral
    return p;
}

Image Gradient(int w, int h) {
    Image img(w, h);
    for (int yy = 0; yy < h; ++yy)
        for (int x = 0; x < w; ++x) img.Y(x, yy) = static_cast<std::uint8_t>(16 + (x * 219) / (w - 1));
    for (int cy = 0; cy < h / 2; ++cy)
        for (int cx = 0; cx < w / 2; ++cx) {
            img.U(cx, cy) = static_cast<std::uint8_t>(100 + cx % 50);
            img.V(cx, cy) = static_cast<std::uint8_t>(150 - cx % 40);
        }
    return img;
}

double MeanY(Image& img) {
    double s = 0;
    for (int yy = 0; yy < img.h; ++yy)
        for (int x = 0; x < img.w; ++x) s += img.Y(x, yy);
    return s / (img.w * img.h);
}

}  // namespace

IXC_TEST(Pipeline_NeutralProfileIsIdentity) {
    const PipelineParams p = CompileParams(Neutral(), 64, 32, false);
    IXC_CHECK(p.identity);
    IXC_CHECK(p.lutIdentity);
    IXC_CHECK(p.geometryIdentity);
    for (int i = 0; i < 256; ++i) IXC_CHECK_EQ(static_cast<int>(p.yLut[static_cast<size_t>(i)]), i);
}

IXC_TEST(Pipeline_DefaultProfileSharpensSubtly) {
    const PipelineParams p = CompileParams(Profile{}, 64, 32, false);  // default sharpness 15
    IXC_CHECK(!p.identity);
    IXC_CHECK(p.sharpenAmount > 0 && p.sharpenAmount < 100);
}

IXC_TEST(Pipeline_IdentityProcessingCopiesExactly) {
    Image src = Gradient(64, 32), dst(64, 32);
    Nv12Processor proc;
    IXC_REQUIRE(proc.Process(src.In(), dst.Out(), CompileParams(Neutral(), 64, 32, false)));
    for (int yy = 0; yy < 32; ++yy)
        for (int x = 0; x < 64; ++x) IXC_CHECK_EQ(dst.Y(x, yy), src.Y(x, yy));
    IXC_CHECK_EQ(dst.U(5, 3), src.U(5, 3));
    IXC_CHECK_EQ(dst.V(31, 15), src.V(31, 15));
}

IXC_TEST(Pipeline_ToneCurvesAreMonotonicAndKeepBlack) {
    for (double b : {-100.0, -40.0, 40.0, 100.0}) {
        for (double c : {-100.0, 0.0, 60.0}) {
            Profile pr = Neutral();
            pr.image.brightness = b;
            pr.image.contrast = c;
            pr.image.shadows = 50;
            pr.image.lowLight = 50;
            const PipelineParams p = CompileParams(pr, 64, 32, false);
            for (int i = 1; i < 256; ++i) IXC_CHECK(p.yLut[static_cast<size_t>(i)] >= p.yLut[static_cast<size_t>(i - 1)]);
        }
    }
    Profile lift = Neutral();
    lift.image.shadows = 100;
    lift.image.lowLight = 100;
    const PipelineParams p = CompileParams(lift, 64, 32, false);
    IXC_CHECK_EQ(static_cast<int>(p.yLut[16]), 16);  // shadow lift doesn't wash out black
    IXC_CHECK(p.yLut[60] > 60);                        // but lifts dark tones
}

IXC_TEST(Pipeline_BrightnessRaisesMeanLuma) {
    Image src = Gradient(64, 32), dst(64, 32);
    Profile pr = Neutral();
    pr.image.brightness = 50;
    Nv12Processor proc;
    IXC_REQUIRE(proc.Process(src.In(), dst.Out(), CompileParams(pr, 64, 32, false)));
    IXC_CHECK(MeanY(dst) > MeanY(src) + 20);
}

IXC_TEST(Pipeline_GammaAboveOneBrightensMidtones) {
    Profile pr = Neutral();
    pr.image.gamma = 1.8;
    const PipelineParams p = CompileParams(pr, 64, 32, false);
    IXC_CHECK(p.yLut[126] > 126);
    IXC_CHECK_EQ(static_cast<int>(p.yLut[16]), 16);
    IXC_CHECK_EQ(static_cast<int>(p.yLut[235]), 235);
}

IXC_TEST(Pipeline_SaturationAndTemperature) {
    Profile grey = Neutral();
    grey.image.saturation = -100;
    const PipelineParams g = CompileParams(grey, 64, 32, false);
    IXC_CHECK_EQ(static_cast<int>(g.uLut[40]), 128);  // fully desaturated
    IXC_CHECK_EQ(static_cast<int>(g.vLut[220]), 128);

    Profile warm = Neutral();
    warm.image.temperature = 100;
    const PipelineParams w = CompileParams(warm, 64, 32, false);
    IXC_CHECK(w.vLut[128] > 128);  // more red
    IXC_CHECK(w.uLut[128] < 128);  // less blue
    IXC_CHECK_EQ(static_cast<int>(w.yLut[100]), 100);  // luma untouched
}

IXC_TEST(Pipeline_MirrorFlipsLumaAndChromaPairs) {
    Image src = Gradient(64, 32), dst(64, 32);
    Profile pr = Neutral();
    pr.mirror = true;
    Nv12Processor proc;
    IXC_REQUIRE(proc.Process(src.In(), dst.Out(), CompileParams(pr, 64, 32, false)));
    for (int x = 0; x < 64; ++x) IXC_CHECK_EQ(dst.Y(x, 7), src.Y(63 - x, 7));
    for (int cx = 0; cx < 32; ++cx) {
        IXC_CHECK_EQ(dst.U(cx, 2), src.U(31 - cx, 2));
        IXC_CHECK_EQ(dst.V(cx, 2), src.V(31 - cx, 2));  // U/V order within a pair preserved
    }
}

IXC_TEST(Pipeline_ZoomMagnifiesCentre) {
    Image src = Gradient(64, 32), dst(64, 32);
    Profile pr = Neutral();
    pr.zoom = 2.0;
    const PipelineParams p = CompileParams(pr, 64, 32, false);
    IXC_CHECK(!p.geometryIdentity);
    IXC_CHECK(std::fabs(p.srcW - 0.5) < 1e-9 && std::fabs(p.srcX - 0.25) < 1e-9);
    Nv12Processor proc;
    IXC_REQUIRE(proc.Process(src.In(), dst.Out(), p));
    // The output spans only the middle half of the gradient, so its range is about half.
    const int range = dst.Y(63, 10) - dst.Y(0, 10);
    const int srcRange = src.Y(63, 10) - src.Y(0, 10);
    IXC_CHECK(std::abs(range - srcRange / 2) <= 6);
    IXC_CHECK(std::abs(dst.Y(32, 10) - src.Y(32, 10)) <= 3);  // centre stays centred
}

IXC_TEST(Pipeline_CropKeepsOutputAspect) {
    Profile pr = Neutral();
    pr.crop = {0.0, 0.0, 1.0, 0.5};  // a wide strip: must be narrowed to 16:9, not stretched
    const PipelineParams p = CompileParams(pr, 1920, 1080, false);
    const double aspect = (p.srcW * 1920) / (p.srcH * 1080);
    IXC_CHECK(std::fabs(aspect - 16.0 / 9.0) < 1e-6);
    IXC_CHECK(p.srcX > 0.2 && p.srcX + p.srcW < 0.8);  // centred in the crop
    IXC_CHECK(p.srcY >= 0 && p.srcY + p.srcH <= 0.5 + 1e-9);
}

IXC_TEST(Pipeline_SharpenBoostsEdgesWithoutHalosAndIgnoresNoise) {
    // Step edge.
    Image src(32, 16), dst(32, 16);
    for (int yy = 0; yy < 16; ++yy)
        for (int x = 0; x < 32; ++x) src.Y(x, yy) = x < 16 ? 60 : 180;
    Profile pr = Neutral();
    pr.image.sharpness = 100;
    const PipelineParams p = CompileParams(pr, 32, 16, false);
    Nv12Processor proc;
    IXC_REQUIRE(proc.Process(src.In(), dst.Out(), p));
    IXC_CHECK(dst.Y(15, 8) < 60);                          // dark side darker at the edge
    IXC_CHECK(dst.Y(16, 8) > 180);                         // bright side brighter
    IXC_CHECK(dst.Y(15, 8) >= 60 - p.sharpenHaloLimit);    // but bounded: no halo
    IXC_CHECK(dst.Y(16, 8) <= 180 + p.sharpenHaloLimit);
    IXC_CHECK_EQ(dst.Y(5, 8), 60);                         // flat areas untouched

    // Low-amplitude noise (±1) below the threshold is left alone.
    Image n(32, 16), nd(32, 16);
    for (int yy = 0; yy < 16; ++yy)
        for (int x = 0; x < 32; ++x) n.Y(x, yy) = static_cast<std::uint8_t>(100 + ((x + yy) & 1));
    IXC_REQUIRE(proc.Process(n.In(), nd.Out(), p));
    for (int x = 1; x < 31; ++x) IXC_CHECK_EQ(nd.Y(x, 8), n.Y(x, 8));
}

IXC_TEST(Pipeline_SharpenSse2MatchesScalarBitExactly) {
    std::uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<std::uint8_t>(seed >> 24); };
    int mismatches = 0;
    for (int w : {3, 4, 9, 10, 17, 64, 1283, 1920}) {
        for (int trial = 0; trial < 20; ++trial) {
            std::vector<std::uint8_t> a(static_cast<size_t>(w)), c(a.size()), b(a.size());
            // Mix of flat regions, edges and noise so every branch is exercised.
            for (int x = 0; x < w; ++x) {
                const bool edge = (x / 7 + trial) % 3 == 0;
                a[static_cast<size_t>(x)] = edge ? rnd() : static_cast<std::uint8_t>(100 + (rnd() & 3));
                c[static_cast<size_t>(x)] = edge ? rnd() : static_cast<std::uint8_t>(100 + (rnd() & 3));
                b[static_cast<size_t>(x)] = edge ? rnd() : static_cast<std::uint8_t>(100 + (rnd() & 3));
            }
            const int amount = static_cast<int>(rnd()) * 384 / 255;
            const int thr = rnd() % 5, halo = rnd() % 12;
            std::vector<std::uint8_t> ds = c, dv = c;
            processing::detail::SharpenRowScalar(a.data(), c.data(), b.data(), ds.data(), w, amount, thr, halo);
            processing::detail::SharpenRowSse2(a.data(), c.data(), b.data(), dv.data(), w, amount, thr, halo);
            if (ds != dv) ++mismatches;
        }
    }
    IXC_CHECK_EQ(mismatches, 0);
}

IXC_TEST(Pipeline_RejectsInconsistentFrames) {
    Image a(64, 32), b(32, 32), odd(63, 32);
    Nv12Processor proc;
    const PipelineParams p = CompileParams(Neutral(), 64, 32, false);
    IXC_CHECK(!proc.Process(a.In(), b.Out(), p));   // size mismatch
    IXC_CHECK(!proc.Process(odd.In(), odd.Out(), p));  // odd width
    Nv12Planes bad = a.In();
    bad.uv = nullptr;
    IXC_CHECK(!proc.Process(bad, a.Out(), p));
}

IXC_TEST(Pipeline_ScratchIsReusedAcrossFrames) {
    Image src = Gradient(128, 64), dst(128, 64);
    Profile pr = Neutral();
    pr.zoom = 1.5;
    pr.image.sharpness = 50;
    const PipelineParams p = CompileParams(pr, 128, 64, false);
    Nv12Processor proc;
    IXC_REQUIRE(proc.Process(src.In(), dst.Out(), p));
    const size_t first = proc.ScratchBytes();
    for (int i = 0; i < 50; ++i) proc.Process(src.In(), dst.Out(), p);
    IXC_CHECK_EQ(proc.ScratchBytes(), first);  // no growth: no per-frame allocation
}
