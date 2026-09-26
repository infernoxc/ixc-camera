#include "ixc_test.h"
#include "processing/backend_selector.h"

using namespace ixc::processing;

namespace {
constexpr std::uint64_t k1080 = 1920ull * 1080ull;
constexpr std::uint64_t k480 = 640ull * 480ull;

BackendSelector::Config Small() {
    BackendSelector::Config c;
    c.measureFrames = 3;
    return c;
}
}  // namespace

IXC_TEST(Selector_CheapWorkNeverTouchesGpu) {
    BackendSelector s(Small());
    s.Reset(true, /*geometry*/ false, k1080);  // colour/sharpen only: not eligible
    IXC_CHECK(s.Current() == Backend::Cpu);
    for (int i = 0; i < 10; ++i) s.OnCpuFrame(8.0);
    IXC_CHECK(!s.TakeGpuRequest());
    IXC_CHECK(s.ShouldReleaseGpu());
}

IXC_TEST(Selector_RespectsGpuNotAllowedAndSmallFrames) {
    BackendSelector s(Small());
    s.Reset(false, true, k1080);  // setting off / low RAM / no adapter
    for (int i = 0; i < 10; ++i) s.OnCpuFrame(20.0);
    IXC_CHECK(!s.TakeGpuRequest());
    s.Reset(true, true, k480);    // below 720p
    for (int i = 0; i < 10; ++i) s.OnCpuFrame(20.0);
    IXC_CHECK(!s.TakeGpuRequest());
    IXC_CHECK(s.Current() == Backend::Cpu);
}

IXC_TEST(Selector_FastCpuStaysOnCpu) {
    BackendSelector s(Small());
    s.Reset(true, true, k1080);
    for (int i = 0; i < 3; ++i) s.OnCpuFrame(1.2);
    IXC_CHECK(s.state() == BackendSelector::State::CpuFinal);
    IXC_CHECK(!s.TakeGpuRequest());
}

IXC_TEST(Selector_SlowCpuTriesGpuAndKeepsItWhenFaster) {
    BackendSelector s(Small());
    s.Reset(true, true, k1080);
    for (int i = 0; i < 3; ++i) s.OnCpuFrame(6.8);
    IXC_CHECK(s.TakeGpuRequest());
    IXC_CHECK(!s.TakeGpuRequest());               // requested exactly once
    IXC_CHECK(s.Current() == Backend::Cpu);       // CPU keeps working until the GPU is ready
    IXC_CHECK(!s.ShouldReleaseGpu());
    s.OnGpuReady(true);
    IXC_CHECK(s.Current() == Backend::Gpu);
    for (int i = 0; i < 3; ++i) s.OnGpuFrame(1.65);
    IXC_CHECK(s.state() == BackendSelector::State::Gpu);
    IXC_CHECK(s.Current() == Backend::Gpu);
}

IXC_TEST(Selector_GpuNotFasterIsReleased) {
    BackendSelector s(Small());
    s.Reset(true, true, k1080);
    for (int i = 0; i < 3; ++i) s.OnCpuFrame(4.0);
    IXC_CHECK(s.TakeGpuRequest());
    s.OnGpuReady(true);
    for (int i = 0; i < 3; ++i) s.OnGpuFrame(3.5);  // not below 70% of 4.0
    IXC_CHECK(s.state() == BackendSelector::State::CpuFinal);
    IXC_CHECK(s.ShouldReleaseGpu());
}

IXC_TEST(Selector_GpuFailureIsStickyForTheSession) {
    BackendSelector s(Small());
    s.Reset(true, true, k1080);
    for (int i = 0; i < 3; ++i) s.OnCpuFrame(9.0);
    IXC_CHECK(s.TakeGpuRequest());
    s.OnGpuReady(false);  // init or self-check failed
    IXC_CHECK(s.GpuFailed());
    IXC_CHECK(s.Current() == Backend::Cpu);
    s.Reset(true, true, k1080);  // new settings: still no GPU this session
    for (int i = 0; i < 10; ++i) s.OnCpuFrame(9.0);
    IXC_CHECK(!s.TakeGpuRequest());
}

IXC_TEST(Selector_RuntimeFailureFallsBackToCpu) {
    BackendSelector s(Small());
    s.Reset(true, true, k1080);
    for (int i = 0; i < 3; ++i) s.OnCpuFrame(7.0);
    s.TakeGpuRequest();
    s.OnGpuReady(true);
    s.OnGpuFrame(1.0);
    s.OnGpuFailure();  // e.g. device removed (driver update, GPU reset)
    IXC_CHECK(s.Current() == Backend::Cpu);
    IXC_CHECK(s.ShouldReleaseGpu());
}
