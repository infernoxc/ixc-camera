// The segmentation network on Direct3D 11 (segmentation/gpu). CI has no GPU: WARP (Direct3D's
// software device) runs the same kernels, so the shaders and the host code are tested on every
// build. On a PC with a GPU the hardware adapter is tested too.

#include "ixc_test.h"
#include "segmentation/gpu/gpu_selfie_net.h"
#include "segmentation/segmentation_engine.h"
#include "segmentation/selfie_net.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace ixc;
using namespace ixc::seg;

namespace {

// Photo-like input: a lit face-and-shoulders shape with some texture, on a busy background.
void FillInput(float* in) {
    std::uint32_t s = 99;
    for (int y = 0; y < kNetH; ++y)
        for (int x = 0; x < kNetW; ++x) {
            const double dx = x - kNetW * 0.45, dy = (y - kNetH * 0.38) * 1.25;
            const bool person = std::sqrt(dx * dx + dy * dy) < kNetH * 0.25 || (y > kNetH * 0.62 && std::abs(dx) < kNetW * 0.24);
            for (int c = 0; c < 3; ++c) {
                s = s * 1664525u + 1013904223u;
                const float noise = static_cast<float>(s >> 24) / 255.0f * 0.06f;
                const float v = person ? 0.78f - 0.17f * static_cast<float>(c) : 0.15f + 0.5f * static_cast<float>((x * 7 + y * 3) % 64) / 64.0f;
                in[(y * kNetW + x) * 3 + c] = std::clamp(v + noise, 0.0f, 1.0f);
            }
        }
}

void CompareWithCpu(NetRunner& gpu) {
    SelfieNet cpu;
    IXC_REQUIRE(cpu.Init());
    FillInput(gpu.Input());
    FillInput(cpu.Input());
    const float* pg = gpu.Run();
    const float* pc = cpu.Run();
    IXC_REQUIRE(pg != nullptr && pc != nullptr);
    float worst = 0;
    int disagree = 0;
    for (int i = 0; i < kNetW * kNetH; ++i) {
        worst = std::max(worst, std::abs(pg[i] - pc[i]));
        disagree += (pg[i] > 0.5f) != (pc[i] > 0.5f);
    }
    std::printf("    %s: max |gpu - cpu| = %.2e, mask pixels that differ: %d of %d\n", gpu.Device().c_str(), static_cast<double>(worst), disagree,
                kNetW * kNetH);
    IXC_CHECK(worst < 1e-3f);    // float rounding only
    IXC_CHECK(disagree <= 20);   // a handful of pixels sitting exactly on 0.5
    // Runs again (buffers reused, nothing left bound): same answer.
    const float first = pg[kNetW * kNetH / 2];
    pg = gpu.Run();
    IXC_REQUIRE(pg != nullptr);
    IXC_CHECK_EQ(pg[kNetW * kNetH / 2], first);
}

}  // namespace

IXC_TEST(GpuSeg_MatchesCpu_Warp) {
    std::string err;
    auto gpu = MakeGpuRunnerAllowWarp(err);
    if (!gpu) std::printf("    no runner: %s\n", err.c_str());
    IXC_REQUIRE(gpu != nullptr);
    IXC_CHECK(gpu->Backend() == SegBackend::Gpu);
    IXC_CHECK(gpu->MemoryBytes() > 2'000'000);
    CompareWithCpu(*gpu);
}

IXC_TEST(GpuSeg_MatchesCpu_Hardware) {
    std::string err;
    auto gpu = MakeGpuRunner(err);
    if (!gpu) {
        // Expected on CI and on PCs without a Direct3D 11 GPU: the reason is reported, never a crash.
        std::printf("    NOT TESTED (no hardware GPU): %s\n", err.c_str());
        IXC_CHECK(!err.empty());
        return;
    }
    CompareWithCpu(*gpu);
}

IXC_TEST(GpuSeg_EngineRunsOnGpuAndSwitchesLive) {
    SegmentationEngine e;
    e.SetGpuFactory(&MakeGpuRunnerAllowWarp);
    e.SetMode(ProcessingMode::Gpu);
    IXC_REQUIRE(e.Start(0.6));
    std::vector<std::uint8_t> y(640 * 360, 110), uv(640 * 180, 128);
    const processing::Nv12Planes f{y.data(), uv.data(), 640, 640, 640, 360};
    auto runUntil = [&](auto pred) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline) {
            e.OnFrame(f, {}, SegmentationEngine::NowMs());
            if (pred(e.Status())) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    IXC_CHECK(runUntil([](const SegStatus& s) { return s.masks >= 3; }));
    SegStatus s = e.Status();
    IXC_CHECK(s.backend == SegBackend::Gpu);
    std::printf("    GPU (%s): network %.1f ms per mask\n", s.device.c_str(), s.avgNetMs);
    e.SetMode(ProcessingMode::Cpu);
    const auto before = s.masks;
    IXC_CHECK(runUntil([&](const SegStatus& st) { return st.backend == SegBackend::Cpu && st.masks >= before + 2; }));
    s = e.Status();
    std::printf("    CPU: network %.1f ms per mask\n", s.avgNetMs);
    e.Stop();
}
