#include "ixc_test.h"
#include "processing/gpu/gpu_pipeline.h"
#include "processing/image_pipeline.h"

#include <cstdio>
#include <vector>

using namespace ixc;
using namespace ixc::processing;

namespace {

struct Frame {
    int w, h, stride;
    std::vector<std::uint8_t> y, uv;
    Frame(int width, int height, int pad) : w(width), h(height), stride(width + pad), y(static_cast<size_t>(stride * height)), uv(static_cast<size_t>(stride * height / 2)) {}
    Nv12Planes In() const { return {y.data(), uv.data(), stride, stride, w, h}; }
    Nv12Frame Out() { return {y.data(), uv.data(), stride, stride, w, h}; }
};

// Photo-like content: gradients, hard edges and low-amplitude noise, so every branch
// (flat/threshold, edge/halo, clamping) is exercised.
Frame MakeContent(int w, int h, int pad, std::uint32_t seed) {
    Frame f(w, h, pad);
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 24; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = 16 + (x * 200) / w + ((x / 9 + y / 7) % 5 == 0 ? 60 : 0) + static_cast<int>(rnd() % 3);
            if (x > w / 2 && y > h / 3) v = 250;  // clipped highlight region
            f.y[static_cast<size_t>(y * f.stride + x)] = static_cast<std::uint8_t>(std::min(v, 255));
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w; ++x) f.uv[static_cast<size_t>(y * f.stride + x)] = static_cast<std::uint8_t>(64 + rnd() % 128);
    return f;
}

struct Case {
    const char* name;
    void (*apply)(Profile&);
};
const Case kCases[] = {
    {"lut only", [](Profile& p) { p.image.brightness = 20; p.image.contrast = -15; p.image.saturation = 30; p.image.temperature = -40; }},
    {"mirror", [](Profile& p) { p.mirror = true; }},
    {"sharpen", [](Profile& p) { p.image.sharpness = 100; }},
    {"lut+sharpen", [](Profile& p) { p.image.gamma = 1.4; p.image.shadows = 50; p.image.sharpness = 35; }},
    {"zoom", [](Profile& p) { p.zoom = 1.7; }},
    {"zoom+mirror+sharpen", [](Profile& p) { p.zoom = 2.3; p.mirror = true; p.image.sharpness = 60; p.image.tint = 25; }},
    {"crop", [](Profile& p) { p.crop = {0.13, 0.07, 0.61, 0.8}; p.image.exposureEv = 0.7; }},
    {"everything", [](Profile& p) { p.crop = {0.2, 0.1, 0.7, 0.7}; p.zoom = 1.3; p.mirror = true; p.image.sharpness = 80; p.image.brightness = -30; p.image.highlights = -60; p.image.lowLight = 40; }},
};

// Runs every case on the given GPU and counts byte mismatches against the CPU reference.
int CompareAll(GpuNv12Processor& gpu, int w, int h, int pad) {
    int mismatchedCases = 0;
    Nv12Processor cpu;
    for (const auto& c : kCases) {
        const Frame src = MakeContent(w, h, pad, 7u + static_cast<std::uint32_t>(w));
        Frame outCpu(w, h, pad), outGpu(w, h, pad);
        Profile p;
        p.image.sharpness = 0;
        c.apply(p);
        const PipelineParams params = CompileParams(p, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), true);
        const bool okCpu = cpu.Process(src.In(), outCpu.Out(), params);
        const bool okGpu = gpu.Process(src.In(), outGpu.Out(), params);
        long diffs = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) diffs += outCpu.y[static_cast<size_t>(y * outCpu.stride + x)] != outGpu.y[static_cast<size_t>(y * outGpu.stride + x)];
        for (int y = 0; y < h / 2; ++y)
            for (int x = 0; x < w; ++x) diffs += outCpu.uv[static_cast<size_t>(y * outCpu.stride + x)] != outGpu.uv[static_cast<size_t>(y * outGpu.stride + x)];
        if (!okCpu || !okGpu || diffs) {
            ++mismatchedCases;
            std::fprintf(stderr, "    %dx%d %s: cpu=%d gpu=%d, %ld differing bytes\n", w, h, c.name, okCpu, okGpu, diffs);
        }
    }
    return mismatchedCases;
}

}  // namespace

IXC_TEST(Gpu_MatchesCpuBitExactly_Warp) {
    GpuNv12Processor gpu;
    GpuNv12Processor::Options o;
    o.useWarp = true;  // always available (software), so this also runs on CI without a GPU
    IXC_REQUIRE(SUCCEEDED(gpu.Initialize(o)));
    IXC_CHECK_EQ(CompareAll(gpu, 64, 36, 16), 0);
    IXC_CHECK_EQ(CompareAll(gpu, 322, 182, 30), 0);  // non-multiple-of-16 size, padded stride
}

IXC_TEST(Gpu_MatchesCpuBitExactly_Hardware) {
    GpuNv12Processor gpu;
    if (FAILED(gpu.Initialize({}))) {
        std::printf("    (no Direct3D 11 hardware adapter: skipped)\n");
        return;
    }
    std::printf("    adapter: %ls\n", gpu.AdapterName().c_str());
    IXC_CHECK_EQ(CompareAll(gpu, 64, 36, 16), 0);
    IXC_CHECK_EQ(CompareAll(gpu, 1280, 720, 64), 0);
}

IXC_TEST(Gpu_RejectsInconsistentFramesAndReinitializes) {
    GpuNv12Processor gpu;
    GpuNv12Processor::Options o;
    o.useWarp = true;
    IXC_REQUIRE(SUCCEEDED(gpu.Initialize(o)));
    Frame a(64, 32, 0), b(32, 32, 0);
    const PipelineParams p = CompileParams(Profile{}, 64, 32, true);
    IXC_CHECK(!gpu.Process(a.In(), b.Out(), p));
    gpu.Release();
    IXC_CHECK(!gpu.Ready());
    IXC_CHECK(!gpu.Process(a.In(), a.Out(), p));  // released: refuses, never crashes
}
