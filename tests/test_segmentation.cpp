#include "ixc_test.h"
#include "segmentation/segmentation_engine.h"
#include "segmentation/selfie_net.h"

#include <chrono>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
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

namespace {

// Stand-ins for the Direct3D runner (the real one is tested in test_gpu_segmentation.cpp):
// "fast" answers immediately; "flaky" works a few times, then fails like a removed GPU.
std::atomic<int> g_fakeRuns{0}, g_fakeAlive{0}, g_flakyLeft{0};
class FakeGpuRunner final : public NetRunner {
public:
    explicit FakeGpuRunner(bool flaky) : flaky_(flaky), in_(static_cast<size_t>(kNetW) * kNetH * 3), out_(static_cast<size_t>(kNetW) * kNetH, 0.9f) { ++g_fakeAlive; }
    ~FakeGpuRunner() override { --g_fakeAlive; }
    float* Input() override { return in_.data(); }
    const float* Run() override {
        ++g_fakeRuns;
        if (flaky_ && g_flakyLeft.fetch_sub(1) <= 0) return nullptr;
        return out_.data();
    }
    size_t MemoryBytes() const override { return 1000; }
    SegBackend Backend() const override { return SegBackend::Gpu; }
    std::string Device() const override { return "Fake GPU"; }

private:
    bool flaky_;
    std::vector<float> in_, out_;
};
std::unique_ptr<NetRunner> FastGpu(std::string&) { return std::make_unique<FakeGpuRunner>(false); }
std::unique_ptr<NetRunner> FlakyGpu(std::string&) { return std::make_unique<FakeGpuRunner>(true); }
std::unique_ptr<NetRunner> NoGpu(std::string& err) {
    err = "no Direct3D 11 GPU";
    return nullptr;
}

// Feeds frames until pred(status) holds or the time runs out.
template <typename Pred>
bool RunUntil(SegmentationEngine& e, Pred pred, int seconds = 20) {
    Nv12 f(640, 360);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        e.OnFrame(f.Planes(), {}, SegmentationEngine::NowMs());
        if (pred(e.Status())) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

}  // namespace

IXC_TEST(Segmentation_ModeChoosesRunnerAndSwitchesLive) {
    SegmentationEngine e;
    e.SetGpuFactory(&FastGpu);
    e.SetMode(ProcessingMode::Cpu);
    IXC_REQUIRE(e.Start(0.6));
    IXC_CHECK(RunUntil(e, [](const SegStatus& s) { return s.masks >= 2; }));
    IXC_CHECK(e.Status().backend == SegBackend::Cpu);
    IXC_CHECK_EQ(g_fakeRuns.load(), 0);  // CPU mode never touches the GPU
    // Switch to GPU while running: the next masks come from the GPU runner, no restart.
    const auto before = e.Status().masks;
    e.SetMode(ProcessingMode::Gpu);
    IXC_CHECK(RunUntil(e, [&](const SegStatus& s) { return s.masks >= before + 3 && s.backend == SegBackend::Gpu; }));
    IXC_CHECK(g_fakeRuns.load() >= 2);
    IXC_CHECK(e.Status().device == "Fake GPU");
    IXC_CHECK_EQ(g_fakeAlive.load(), 1);
    // And back: the GPU runner is destroyed (its memory freed).
    e.SetMode(ProcessingMode::Cpu);
    IXC_CHECK(RunUntil(e, [](const SegStatus& s) { return s.backend == SegBackend::Cpu; }));
    IXC_CHECK_EQ(g_fakeAlive.load(), 0);
    e.Stop();
}

IXC_TEST(Segmentation_GpuModeFallsBackToCpuWithReason) {
    {
        SegmentationEngine e;
        e.SetGpuFactory(&NoGpu);
        e.SetMode(ProcessingMode::Gpu);
        IXC_REQUIRE(e.Start(0.6));
        IXC_CHECK(RunUntil(e, [](const SegStatus& s) { return s.masks >= 1; }));
        const SegStatus s = e.Status();
        IXC_CHECK(s.backend == SegBackend::Cpu);
        IXC_CHECK(s.gpuNote.find("no Direct3D 11 GPU") != std::string::npos);
        e.Stop();
    }
    {  // the GPU dies mid-stream: masks keep coming, from the CPU
        g_flakyLeft = 3;
        SegmentationEngine e;
        e.SetGpuFactory(&FlakyGpu);
        e.SetMode(ProcessingMode::Gpu);
        IXC_REQUIRE(e.Start(0.6));
        IXC_CHECK(RunUntil(e, [](const SegStatus& s) { return s.masks >= 6 && s.backend == SegBackend::Cpu; }));
        IXC_CHECK(e.Status().state == SegState::Running);
        IXC_CHECK(!e.Status().gpuNote.empty());
        IXC_CHECK_EQ(g_fakeAlive.load(), 0);
        e.Stop();
    }
}

IXC_TEST(Segmentation_AutoKeepsFasterGpu) {
    SegmentationEngine e;
    e.SetGpuFactory(&FastGpu);  // answers in microseconds: clearly faster than the CPU network
    IXC_REQUIRE(e.Start(0.6));   // Auto by default
    IXC_CHECK(RunUntil(e, [](const SegStatus& s) { return s.masks >= 16; }));
    IXC_CHECK(e.Status().backend == SegBackend::Gpu);
    e.Stop();
    IXC_CHECK_EQ(g_fakeAlive.load(), 0);
    // Without a GPU path Auto simply stays on the CPU.
    SegmentationEngine c;
    IXC_REQUIRE(c.Start(0.6));
    IXC_CHECK(RunUntil(c, [](const SegStatus& s) { return s.masks >= 2; }));
    IXC_CHECK(c.Status().backend == SegBackend::Cpu && c.Status().gpuNote.empty());
    c.Stop();
}
