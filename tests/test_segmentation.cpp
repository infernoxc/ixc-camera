#include "ixc_test.h"
#include "segmentation/segmentation_engine.h"
#include "segmentation/selfie_net.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

using namespace ixc;
using namespace ixc::seg;

namespace {

// Synthetic "head and shoulders" on a textured background (the same image the reference values
// below were computed from with the TFLite runtime, scripts/convert-selfie-model.py's source model).
void FillSynthetic(float* in) {
    for (int y = 0; y < SelfieNet::kHeight; ++y) {
        for (int x = 0; x < SelfieNet::kWidth; ++x) {
            const int i = y * SelfieNet::kWidth + x;
            const float bg = static_cast<float>((i * 37) % 101) / 101.0f * 0.6f + 0.2f;
            const float hx = static_cast<float>(x - 128) / 40.0f, hy = static_cast<float>(y - 60) / 48.0f;
            const float bx = static_cast<float>(x - 128) / 90.0f, by = static_cast<float>(y - 150) / 60.0f;
            float* p = in + static_cast<size_t>(i) * 3;
            if (hx * hx + hy * hy < 1) {
                p[0] = 0.85f, p[1] = 0.65f, p[2] = 0.55f;
            } else if (bx * bx + by * by < 1) {
                p[0] = 0.2f, p[1] = 0.25f, p[2] = 0.5f;
            } else {
                p[0] = bg, p[1] = bg * 0.9f, p[2] = bg * 0.8f;
            }
        }
    }
}

// NV12 frame with a bright ellipse ("person") in the centre on a mid-grey textured background.
struct Nv12 {
    int w, h;
    std::vector<std::uint8_t> buf;
    Nv12(int w_, int h_) : w(w_), h(h_), buf(static_cast<size_t>(w_) * h_ * 3 / 2, 128) {}
    processing::Nv12Planes Planes() const { return {buf.data(), buf.data() + static_cast<size_t>(w) * h, w, w, w, h}; }
};

}  // namespace

IXC_TEST(Segmentation_NetworkMatchesReference) {
    SelfieNet net;
    IXC_CHECK(net.Init());
    FillSynthetic(net.Input());
    const float* out = net.Run();
    IXC_CHECK(out != nullptr);
    if (!out) return;
    struct Ref {
        int y, x;
        float p;
    };
    // Edge pixels with intermediate probabilities: sensitive to any kernel or weight error.
    const Ref refs[] = {{10, 122, 0.052504f}, {19, 107, 0.925761f}, {42, 90, 0.345388f}, {71, 167, 0.920106f},
                        {92, 161, 0.086550f}, {103, 185, 0.806812f}, {122, 208, 0.883364f}, {143, 219, 0.146945f},
                        {60, 128, 1.0f},      {10, 10, 0.0f}};
    for (const Ref& r : refs) {
        const float v = out[r.y * SelfieNet::kWidth + r.x];
        IXC_CHECK(std::abs(v - r.p) < 2e-3f);
    }
    double sum = 0;
    int person = 0;
    for (int i = 0; i < SelfieNet::kWidth * SelfieNet::kHeight; ++i) {
        sum += out[i];
        person += out[i] > 0.5f;
    }
    IXC_CHECK(std::abs(sum / (SelfieNet::kWidth * SelfieNet::kHeight) - 0.347659) < 1e-3);
    IXC_CHECK(std::abs(person - 12816) < 40);

    // Repeatable: a second run on the same input gives the same answer (no state leaks between runs).
    FillSynthetic(net.Input());
    const float again = net.Run()[71 * SelfieNet::kWidth + 167];
    IXC_CHECK(std::abs(again - 0.920106f) < 2e-3f);
}

IXC_TEST(Segmentation_ProbabilityToMaskSharpensEdges) {
    IXC_CHECK_EQ(ProbabilityToMask(0.0f), 0);
    IXC_CHECK_EQ(ProbabilityToMask(0.2f), 0);
    IXC_CHECK_EQ(ProbabilityToMask(0.5f), 128);
    IXC_CHECK_EQ(ProbabilityToMask(0.8f), 255);
    IXC_CHECK_EQ(ProbabilityToMask(1.0f), 255);
    IXC_CHECK(ProbabilityToMask(0.4f) < ProbabilityToMask(0.6f));
}

IXC_TEST(Segmentation_SampleNv12ToRgb) {
    Nv12 f(640, 360);
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) f.buf[static_cast<size_t>(y) * 640 + x] = static_cast<std::uint8_t>(x < 320 ? 16 : 235);
    std::vector<std::uint8_t> rgb(static_cast<size_t>(kMaskW) * kMaskH * 3);
    IXC_CHECK(SampleNv12ToRgb(f.Planes(), {}, rgb.data()));
    IXC_CHECK(rgb[(10 * kMaskW + 10) * 3] < 5);    // left: black
    IXC_CHECK(rgb[(10 * kMaskW + 250) * 3] > 250);  // right: white
    Nv12 tiny(64, 36);  // smaller than the network input: refused
    IXC_CHECK(!SampleNv12ToRgb(tiny.Planes(), {}, rgb.data()));
}

IXC_TEST(Segmentation_EngineProducesMaskWithoutBlockingFrames) {
    SegmentationEngine e;
    SegMask m;
    IXC_CHECK(!e.Snapshot(m));  // not started: no mask
    IXC_CHECK(e.Start(0.5));
    Nv12 f(640, 360);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool got = false;
    while (!got && std::chrono::steady_clock::now() < deadline) {
        const auto t0 = std::chrono::steady_clock::now();
        e.OnFrame(f.Planes(), {}, SegmentationEngine::NowMs());
        // The frame path never waits for the network (sampling 256x144 only).
        IXC_CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));
        got = e.Snapshot(m);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    IXC_CHECK(got);
    IXC_CHECK(m.generation > 0);
    IXC_CHECK_EQ(m.value.size(), static_cast<size_t>(kMaskW) * kMaskH);
    const std::uint64_t g = m.generation;
    IXC_CHECK(e.Snapshot(m) && m.generation >= g);
    IXC_CHECK(e.Status().masks >= 1);
    IXC_CHECK(e.Status().memoryBytes > 0);
    e.Stop();
    IXC_CHECK(!e.Snapshot(m));  // stopped: no mask, memory released
    IXC_CHECK(e.Status().state == SegState::Off);
    // Restarting continues the generation count: an old copy never looks current.
    IXC_CHECK(e.Start(0.5));
    e.Stop();
}

IXC_TEST(Segmentation_LowLightGainBrightensDarkInput) {
    std::vector<std::uint8_t> rgb(300 * 3, 120);
    IXC_CHECK_EQ(seg::LowLightGain(rgb.data(), 300), 1.0f);  // normal light: untouched
    std::fill(rgb.begin(), rgb.end(), std::uint8_t{55});
    IXC_CHECK(std::abs(seg::LowLightGain(rgb.data(), 300) - 2.0f) < 0.01f);
    std::fill(rgb.begin(), rgb.end(), std::uint8_t{20});
    IXC_CHECK_EQ(seg::LowLightGain(rgb.data(), 300), 2.5f);  // capped: noise grows with gain
    std::fill(rgb.begin(), rgb.end(), std::uint8_t{1});
    IXC_CHECK_EQ(seg::LowLightGain(rgb.data(), 300), 1.0f);  // black: nothing to recover
}
