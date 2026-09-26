#include "camera/device_enum.h"
#include "camera/format_select.h"
#include "camera/frame_stats.h"
#include "camera/latest_mailbox.h"
#include "camera/reconnect_policy.h"
#include "ixc_test.h"

#include <mfapi.h>

#include <atomic>
#include <memory>
#include <thread>

using namespace ixc;
using namespace ixc::camera;

namespace {

CaptureFormat F(const GUID& sub, std::uint32_t w, std::uint32_t h, std::uint32_t fps, std::uint32_t den = 1) {
    CaptureFormat f;
    f.subtype = sub;
    f.width = w;
    f.height = h;
    f.fpsNumerator = fps;
    f.fpsDenominator = den;
    return f;
}

// Shape of a typical USB 2.0 webcam: uncompressed only at low rates for large sizes.
std::vector<CaptureFormat> Usb2Webcam() {
    return NormalizeFormats({
        F(MFVideoFormat_YUY2, 1920, 1080, 5),
        F(MFVideoFormat_MJPG, 1920, 1080, 30),
        F(MFVideoFormat_MJPG, 1920, 1080, 15),
        F(MFVideoFormat_YUY2, 1280, 720, 10),
        F(MFVideoFormat_MJPG, 1280, 720, 30),
        F(MFVideoFormat_YUY2, 640, 480, 30),
        F(MFVideoFormat_MJPG, 640, 480, 30),
        F(MFVideoFormat_MJPG, 640, 480, 30),  // duplicate
    });
}

const CaptureFormat& Pick(const std::vector<CaptureFormat>& fs, FormatRequest r) {
    static CaptureFormat none;
    auto i = SelectFormat(fs, r);
    return i ? fs[*i] : none;
}

}  // namespace

IXC_TEST(Format_NamesAndDescriptions) {
    IXC_CHECK_EQ(SubtypeName(MFVideoFormat_MJPG), std::string("MJPG"));
    IXC_CHECK_EQ(SubtypeName(MFVideoFormat_NV12), std::string("NV12"));
    IXC_CHECK_EQ(SubtypeName(MFVideoFormat_RGB32), std::string("RGB32"));
    IXC_CHECK_EQ(Describe(F(MFVideoFormat_NV12, 1920, 1080, 30)), std::string("1920x1080 @ 30 FPS (NV12)"));
    IXC_CHECK_EQ(Describe(F(MFVideoFormat_NV12, 1280, 720, 30000, 1001)), std::string("1280x720 @ 29.97 FPS (NV12)"));
}

IXC_TEST(Format_NormalizeDedupesAndSorts) {
    const auto fs = Usb2Webcam();
    IXC_CHECK_EQ(fs.size(), size_t{7});
    IXC_CHECK_EQ(fs.front().width, 1920u);
    IXC_CHECK(fs.front().Fps() == 30.0);  // highest rate first within a resolution
    IXC_CHECK(IsEqualGUID(fs.back().subtype, MFVideoFormat_MJPG) || fs.back().width == 640);
    // Invalid entries are dropped.
    IXC_CHECK(NormalizeFormats({F(MFVideoFormat_NV12, 0, 0, 30), F(MFVideoFormat_NV12, 640, 480, 0)}).empty());
}

IXC_TEST(Format_PrefersRateOverUncompressed) {
    const auto fs = Usb2Webcam();
    const auto& f = Pick(fs, {1920, 1080, 30});
    IXC_CHECK(IsEqualGUID(f.subtype, MFVideoFormat_MJPG));  // YUY2 1080p is only 5 FPS
    IXC_CHECK_EQ(f.Fps(), 30.0);
}

IXC_TEST(Format_PrefersUncompressedWhenRateIsEqual) {
    const auto fs = Usb2Webcam();
    const auto& f = Pick(fs, {640, 480, 30});
    IXC_CHECK(IsEqualGUID(f.subtype, MFVideoFormat_YUY2));
}

IXC_TEST(Format_NeverInventsModes) {
    const auto fs = Usb2Webcam();
    // 4K60 isn't offered: pick the best real mode that fits, not a fabricated one.
    const auto& f = Pick(fs, {3840, 2160, 60});
    IXC_CHECK_EQ(f.width, 1920u);
    IXC_CHECK_EQ(f.height, 1080u);
    IXC_CHECK_EQ(f.Fps(), 30.0);
    // Unavailable size: largest that fits below.
    const auto& g = Pick(fs, {1600, 900, 30});
    IXC_CHECK_EQ(g.width, 1280u);
    // Request below everything: smallest above.
    const auto& h = Pick(fs, {160, 120, 30});
    IXC_CHECK_EQ(h.width, 640u);
}

IXC_TEST(Format_AutoPicksLargestModeReachingTarget) {
    const auto fs = NormalizeFormats({
        F(MFVideoFormat_YUY2, 2592, 1944, 5),  // high-res but slow
        F(MFVideoFormat_MJPG, 1280, 720, 30),
        F(MFVideoFormat_MJPG, 640, 480, 30),
    });
    const auto& f = Pick(fs, {});
    IXC_CHECK_EQ(f.width, 1280u);
    IXC_CHECK_EQ(f.Fps(), 30.0);
}

IXC_TEST(Format_TierDefaults) {
    const auto fs = Usb2Webcam();
    IXC_CHECK_EQ(Pick(fs, RequestForTier(PerformanceTier::UltraLow)).height, 720u);
    IXC_CHECK_EQ(Pick(fs, RequestForTier(PerformanceTier::Balanced)).height, 1080u);
    IXC_CHECK(!SelectFormat({}, {1920, 1080, 30}));
}

IXC_TEST(Format_SlowCameraGetsBestAvailableRate) {
    const auto fs = NormalizeFormats({F(MFVideoFormat_YUY2, 1280, 720, 10), F(MFVideoFormat_YUY2, 1280, 720, 5)});
    const auto& f = Pick(fs, {1280, 720, 30});
    IXC_CHECK_EQ(f.Fps(), 10.0);  // the closest real rate; never a claimed 30
}

IXC_TEST(Device_SoftwareLinkDetection) {
    IXC_CHECK(IsSoftwareDeviceLink(L"\\\\?\\root#camera#0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global"));
    IXC_CHECK(IsSoftwareDeviceLink(L"\\\\?\\SWD#VCAMDEVAPI#abc#{e5323777-f976-4f5b-9b55-b94699c46e44}"));
    IXC_CHECK(!IsSoftwareDeviceLink(L"\\\\?\\usb#vid_17ef&pid_4831&mi_00#7&36250dec&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global"));
}

IXC_TEST(Device_SameDeviceIgnoresInterfaceClassAndCase) {
    const std::wstring cam = L"\\\\?\\usb#vid_17ef&pid_4831&mi_00#7&36250dec&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global";
    const std::wstring cap = L"\\\\?\\USB#VID_17EF&PID_4831&MI_00#7&36250DEC&0&0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global";
    const std::wstring other = L"\\\\?\\usb#vid_046d&pid_0825&mi_00#7&1111&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global";
    IXC_CHECK(SameDevice(cam, cap));
    IXC_CHECK(!SameDevice(cam, other));
    IXC_CHECK(!SameDevice(cam, L""));
}

IXC_TEST(FrameStats_SteadyStream) {
    FrameStats s(33333);
    for (int i = 0; i < 100; ++i) s.OnFrame(1'000'000 + i * 33333LL, i * 33333LL, 30000);
    const auto snap = s.Snapshot();
    IXC_CHECK_EQ(snap.framesReceived, std::uint64_t{100});
    IXC_CHECK(std::abs(snap.fps - 30.0) < 0.05);
    IXC_CHECK_EQ(snap.stalls, std::uint64_t{0});
    IXC_CHECK_EQ(snap.timestampJumps, std::uint64_t{0});
    IXC_CHECK(snap.jitterMs < 0.01);
    IXC_CHECK(std::abs(snap.meanLatencyMs - 30.0) < 0.01);
    IXC_CHECK(!snap.underSpeed);
}

IXC_TEST(FrameStats_DetectsStallsAndTimestampJumps) {
    FrameStats s(33333);
    std::int64_t t = 0;
    for (int i = 0; i < 10; ++i) {
        t += 33333;
        s.OnFrame(t, t, -1);
    }
    t += 600'000;
    s.OnFrame(t, t, -1);  // 600 ms gap: stall + timestamp skip
    t += 33333;
    s.OnFrame(t, t - 100000, -1);  // timestamp went backwards
    const auto snap = s.Snapshot();
    IXC_CHECK_EQ(snap.stalls, std::uint64_t{1});
    IXC_CHECK_EQ(snap.timestampJumps, std::uint64_t{2});
    IXC_CHECK(snap.maxIntervalMs >= 600.0);
    IXC_CHECK_EQ(snap.meanLatencyMs, -1.0);
}

IXC_TEST(FrameStats_UnderSpeedNeedsFullWindow) {
    FrameStats s(33333);  // negotiated 30 FPS
    std::int64_t t = 0;
    auto frame = [&] {
        t += 50000;  // 20 FPS
        s.OnFrame(t, t);
    };
    for (int i = 0; i < 20; ++i) frame();
    IXC_CHECK(!s.Snapshot().underSpeed);  // too early to judge
    for (int i = 0; i < 60; ++i) frame();
    const auto snap = s.Snapshot();
    IXC_CHECK(snap.underSpeed);
    IXC_CHECK(std::abs(snap.fps - 20.0) < 0.05);
    IXC_CHECK(std::abs(snap.nominalFps - 30.0) < 0.01);
}

IXC_TEST(FrameStats_ResetClearsEverything) {
    FrameStats s(33333);
    for (int i = 0; i < 5; ++i) s.OnFrame(i * 33333LL, i * 33333LL);
    s.OnStreamTick();
    s.Reset(16667);
    const auto snap = s.Snapshot();
    IXC_CHECK_EQ(snap.framesReceived, std::uint64_t{0});
    IXC_CHECK_EQ(snap.streamTicks, std::uint64_t{0});
    IXC_CHECK_EQ(s.StallThresholdUs(), std::int64_t{250000});
}

IXC_TEST(Mailbox_KeepsOnlyNewestAndCountsDrops) {
    LatestMailbox<int> box;
    IXC_CHECK(!box.Take());
    IXC_CHECK(!box.Put(1));
    IXC_CHECK(box.Put(2));
    IXC_CHECK(box.Put(3));
    auto v = box.Take();
    IXC_REQUIRE(v.has_value());
    IXC_CHECK_EQ(*v, 3);
    IXC_CHECK(!box.Take());
    IXC_CHECK_EQ(box.Dropped(), std::uint64_t{2});
}

IXC_TEST(Mailbox_ReleasesReplacedValues) {
    auto tracker = std::make_shared<int>(0);
    std::weak_ptr<int> weak = tracker;
    LatestMailbox<std::shared_ptr<int>> box;
    box.Put(std::move(tracker));
    box.Put(std::make_shared<int>(1));
    IXC_CHECK(weak.expired());  // the dropped frame was released, not retained
    box.Clear();
}

IXC_TEST(Mailbox_ConcurrentProducerConsumerLosesNothingUnaccounted) {
    LatestMailbox<int> box;
    constexpr int kN = 200000;
    std::atomic<bool> done{false};
    std::uint64_t taken = 0;
    int last = -1;
    bool ordered = true;
    std::thread consumer([&] {
        for (;;) {
            if (auto v = box.Take()) {
                if (*v <= last) ordered = false;
                last = *v;
                ++taken;
            } else if (done.load()) {
                break;
            }
        }
    });
    for (int i = 0; i < kN; ++i) box.Put(i);
    done = true;
    consumer.join();
    if (auto v = box.Take()) { ++taken; last = *v; }
    IXC_CHECK(ordered);  // consumer never sees an older frame after a newer one
    IXC_CHECK_EQ(taken + box.Dropped(), static_cast<std::uint64_t>(kN));
    IXC_CHECK_EQ(last, kN - 1);  // the newest frame always survives
}

IXC_TEST(Reconnect_BackoffIsBounded) {
    using ms = std::chrono::milliseconds;
    ReconnectPolicy p({ms(100), ms(200)}, 4);
    IXC_CHECK(p.NextDelay() == ms(100));
    IXC_CHECK(p.NextDelay() == ms(200));
    IXC_CHECK(p.NextDelay() == ms(200));  // last delay repeats
    IXC_CHECK(p.NextDelay() == ms(200));
    IXC_CHECK(!p.NextDelay());            // exhausted: wait for device arrival instead
    IXC_CHECK_EQ(p.Attempts(), 4);
    p.Reset();
    IXC_CHECK(p.NextDelay() == ms(100));
}

IXC_TEST(Reconnect_DefaultPolicyStopsWithinSeconds) {
    ReconnectPolicy p;
    long long total = 0;
    int n = 0;
    while (auto d = p.NextDelay()) { total += d->count(); ++n; }
    IXC_CHECK(n > 0 && n <= 10);
    IXC_CHECK(total <= 15000);  // all retries done within 15 s, then zero-CPU wait
}
